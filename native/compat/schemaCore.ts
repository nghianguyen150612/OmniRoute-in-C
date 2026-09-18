/**
 * Compatibility harness — shared schema-validation core (private to
 * `native/compat/`).
 *
 * Extracted from the Task 006 scenario loader so Stage 2 (observations and
 * results) reuses the exact same validation discipline without duplicating
 * it: ajv over the canonical checked-in schemas, cached compiled validators,
 * cwd-independent default schema resolution, deterministic secret-free
 * diagnostics, and freeze/clone helpers for the mutation-safety boundary.
 *
 * This module holds no domain codes and no scenario/observation/result
 * semantics — only the generic machinery. Callers keep their own diagnostic
 * types, codes, and semantic layers.
 */

import { readFileSync } from "node:fs";
import path from "node:path";
import Ajv, { type ErrorObject, type ValidateFunction } from "ajv";

export type DeepReadonly<T> = T extends (...args: never[]) => unknown
  ? T
  : T extends ReadonlyArray<unknown>
    ? ReadonlyArray<DeepReadonly<T[number]>>
    : T extends object
      ? { readonly [K in keyof T]: DeepReadonly<T[K]> }
      : T;

/** Minimal diagnostic shape shared by every compat validator. */
export interface CompatDiagnostic {
  code: string;
  /** Dotted field path, e.g. `http.method` or `mismatches[0].rule`. */
  path?: string;
  /** Concise reason. Never contains fixture content or secret values. */
  message: string;
}

export function compareCompatDiagnostics(a: CompatDiagnostic, b: CompatDiagnostic): number {
  if ((a.path ?? "") !== (b.path ?? "")) return (a.path ?? "") < (b.path ?? "") ? -1 : 1;
  if (a.code !== b.code) return a.code < b.code ? -1 : 1;
  if (a.message !== b.message) return a.message < b.message ? -1 : 1;
  return 0;
}

export function messageOf(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

export function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

/** Deep-freeze one value graph in place (JSON data is acyclic by construction). */
export function deepFreezeValue(value: unknown): void {
  if (value === null || typeof value !== "object" || Object.isFrozen(value)) return;
  for (const child of Object.values(value)) deepFreezeValue(child);
  Object.freeze(value);
}

/**
 * Detached deep copy for JSON-shaped data. Used at mutation boundaries
 * (e.g. normalization works on clones, never on frozen raw evidence).
 */
export function deepCloneJson<T>(value: T): T {
  return structuredClone(value);
}

/**
 * Remove `undefined`-valued properties recursively (arrays mapped
 * element-wise). `null` is meaningful contract data and is preserved. Used
 * by factories so omitted optionals never materialize as explicit
 * `undefined` keys in serialized artifacts.
 */
export function stripUndefined<T>(value: T): T {
  if (Array.isArray(value)) {
    for (let index = 0; index < value.length; index++) {
      value[index] = stripUndefined(value[index]);
    }
    return value;
  }
  if (isRecord(value)) {
    for (const key of Object.keys(value)) {
      const child = value[key];
      if (child === undefined) delete value[key];
      else stripUndefined(child);
    }
    return value;
  }
  return value;
}

/**
 * Convert an ajv JSON pointer (`/http/headers/accept`, `/tags/0`) to the
 * dotted form used in diagnostics (`http.headers.accept`, `tags[0]`).
 */
export function pointerToDotted(pointer: string): string {
  if (!pointer) return "";
  const parts = pointer
    .split("/")
    .slice(1)
    .map((p) => p.replace(/~1/g, "/").replace(/~0/g, "~"));
  let out = "";
  for (const part of parts) {
    if (/^(0|[1-9][0-9]*)$/.test(part) && out.length > 0) out += `[${part}]`;
    else out += (out.length > 0 ? "." : "") + part;
  }
  return out;
}

export function prefixPath(prefix: string, dotted: string): string {
  if (!dotted) return prefix;
  if (!prefix) return dotted;
  return `${prefix}.${dotted}`;
}

export interface FormattedSchemaError {
  path: string;
  message: string;
}

/**
 * Map one ajv error to a diagnostic path/message pair using schema-controlled
 * context only (allowed values, patterns, property names) — never the
 * offending fixture value.
 */
export function formatAjvError(base: string, err: ErrorObject): FormattedSchemaError {
  const dotted = pointerToDotted(err.instancePath);
  const params = err.params as Record<string, unknown>;
  let path = prefixPath(base, dotted);
  let message: string;
  switch (err.keyword) {
    case "required":
      message = `missing required field '${String(params["missingProperty"] ?? "?")}'`;
      if (typeof params["missingProperty"] === "string") {
        path = prefixPath(
          base,
          pointerToDotted(`${err.instancePath}/${params["missingProperty"]}`)
        );
      }
      break;
    case "additionalProperties":
      message = `unknown field '${String(params["additionalProperty"] ?? "?")}' is not allowed`;
      if (typeof params["additionalProperty"] === "string") {
        path = prefixPath(
          base,
          pointerToDotted(`${err.instancePath}/${params["additionalProperty"]}`)
        );
      }
      break;
    case "enum": {
      const allowed = params["allowedValues"];
      message = Array.isArray(allowed)
        ? `value must be one of: ${allowed.map((v) => JSON.stringify(v)).join(", ")}`
        : "value is not one of the allowed values";
      break;
    }
    case "const":
      message = "value does not match the required constant for this field";
      break;
    case "type":
      message = `value must be of type ${String(params["type"] ?? "?")}`;
      break;
    case "pattern":
      message = "value does not match the required pattern for this field";
      break;
    case "minLength":
      message = "value must not be empty";
      break;
    case "minimum":
    case "maximum":
      message = `value is outside the allowed range for this field`;
      break;
    case "oneOf":
    case "anyOf":
      message = "value does not match any allowed shape for this field";
      break;
    default:
      message = err.message ?? "value is invalid for this field";
      break;
  }
  return { path, message };
}

// ---------------------------------------------------------------------------
// Canonical schema compilation (cached per resolved schema path)
// ---------------------------------------------------------------------------

export interface CompiledJsonSchema {
  resolvedPath: string;
  validate: ValidateFunction;
}

const compiledByPath = new Map<string, CompiledJsonSchema>();

export interface CompileSchemaOptions {
  /** Caller/module label used in programmer-error messages. */
  label: string;
  /** Absolute default path (callers derive it from `import.meta.url`). */
  defaultPath: string;
  /** Optional override; resolved against the caller's cwd when relative. */
  schemaPath?: string;
}

/**
 * Read, parse, and ajv-compile a canonical JSON Schema file, caching the
 * result per resolved path. Throws only for programmer errors (unreadable or
 * malformed schema file) — never for invalid validated data.
 */
export function compileCanonicalSchema(options: CompileSchemaOptions): CompiledJsonSchema {
  const resolvedPath = path.resolve(options.schemaPath ?? options.defaultPath);
  const cached = compiledByPath.get(resolvedPath);
  if (cached) return cached;
  let raw: string;
  try {
    raw = readFileSync(resolvedPath, "utf8");
  } catch (err) {
    throw new Error(`${options.label}: cannot read schema at ${resolvedPath}: ${messageOf(err)}`);
  }
  let schema: unknown;
  try {
    schema = JSON.parse(raw) as unknown;
  } catch (err) {
    throw new Error(
      `${options.label}: schema at ${resolvedPath} is not valid JSON: ${messageOf(err)}`
    );
  }
  if (!isRecord(schema) && typeof schema !== "boolean") {
    throw new Error(`${options.label}: schema at ${resolvedPath} is not a JSON Schema object`);
  }
  const ajv = new Ajv({ allErrors: true, strict: false, validateFormats: false });
  const compiled: CompiledJsonSchema = { resolvedPath, validate: ajv.compile(schema) };
  compiledByPath.set(resolvedPath, compiled);
  return compiled;
}
