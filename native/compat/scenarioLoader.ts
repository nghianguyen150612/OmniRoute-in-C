/**
 * Compatibility harness — Stage 1: scenario loader and validator (Task 006).
 *
 * Loads declarative compatibility scenario documents and validates them
 * against the canonical Task 005 scenario contract. This module performs NO
 * execution: it never starts a backend, sends HTTP requests, compares
 * behavior, or touches observation/result processing (later stages).
 *
 * Design references:
 * - docs/native-backend/COMPATIBILITY_HARNESS.md sections 3 (pipeline Load
 *   stage), 4 (scenario schema), and 15 (Stage 1 exit criteria).
 * - Canonical runtime schema (single source of truth — never copied here):
 *   docs/native-backend/contracts/compat-scenario.schema.json
 *
 * Validation strategy: ajv (already a direct dependency, with in-repo
 * precedent in open-sse/vendor/codex-chatgpt-web/adapters/chatgpt-web/
 * output-validation.ts) compiles the canonical schema file. No hand-rolled
 * JSON Schema subset, no second schema copy: the TypeScript interfaces below
 * are compile-time conveniences only and are never consulted at runtime.
 *
 * Accepted document shapes:
 * - a single scenario object (validated directly against the schema), or
 * - a scenario-set envelope: an object with a `scenarios` array whose entries
 *   are each validated as scenarios (this is the shape of
 *   docs/native-backend/contracts/compat-models-examples.json). Envelope
 *   metadata is tolerated and ignored; only `scenarios` is read.
 *
 * Stage 1 boundary notes (see COMPATIBILITY_HARNESS.md Stage 1 status):
 * - `http.body: { artifact }` refs are carried through verbatim (shape-checked
 *   by the schema). Resolving them against a corpus root and checking the
 *   referenced file exists belongs to the execution stage, which owns corpus
 *   roots — not to Stage 1.
 * - No existence checks for backends, credentials, services, databases, or
 *   endpoints. Stage 1 validates definitions, not runtime availability.
 * - No fixture-size cap: Task 005 deferred body/capture caps to execution
 *   (§12) and specifies none for scenario files, so none is invented here.
 */

import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import Ajv, { type ErrorObject, type ValidateFunction } from "ajv";

// ---------------------------------------------------------------------------
// Public types (compile-time only; runtime truth is the canonical schema file)
// ---------------------------------------------------------------------------

export type DeepReadonly<T> = T extends (...args: never[]) => unknown
  ? T
  : T extends ReadonlyArray<unknown>
    ? ReadonlyArray<DeepReadonly<T[number]>>
    : T extends object
      ? { readonly [K in keyof T]: DeepReadonly<T[K]> }
      : T;

export interface CompatScenarioHttp {
  method: string;
  path: string;
  query?: Record<string, string>;
  headers?: Record<string, string>;
  body?: null | string | { artifact: string };
  followRedirects?: boolean;
}

export interface CompatScenarioAuth {
  kind: "none" | "provisioned-api-key" | "invalid-key" | "malformed-scheme" | "session";
  value?: string;
  note?: string;
}

export interface CompatScenarioSeed {
  fixture: string;
  tables?: string[];
}

export interface CompatScenarioJsonCompare {
  mode?: "exact" | "shape" | "subset" | "ordered-array" | "unordered-array" | "error-triple";
  requiredPaths?: string[];
  absentPaths?: string[];
  orderedArray?: string;
  normalizePaths?: Record<string, string>;
  ignorePaths?: string[];
  expected?: unknown;
}

export interface CompatScenarioCompare {
  status?: number;
  headers?: Record<string, string>;
  json?: CompatScenarioJsonCompare;
  redirect?: { location?: string; refresh?: string };
  stream?: Record<string, unknown>;
  cancel?: Record<string, unknown>;
  capabilities?: string[];
}

export interface CompatPersistenceObservation {
  table: string;
  where?: Record<string, unknown>;
  select: "count" | "exists" | string[];
  orderBy?: string;
  limit?: number;
  expect?: unknown;
}

export interface CompatScenarioPersistence {
  tables?: string[];
  observations?: CompatPersistenceObservation[];
  settleMs?: number;
}

export interface CompatScenario {
  schema: "compat-scenario/v1";
  id: string;
  description: string;
  http: CompatScenarioHttp;
  auth?: CompatScenarioAuth;
  environment?: Record<string, unknown>;
  seed?: CompatScenarioSeed;
  compare: CompatScenarioCompare;
  persistence?: CompatScenarioPersistence;
  timeout?: "health" | "api" | "chat" | "stream" | "bulk";
  tags?: string[];
  capabilities?: string[];
  extensions?: Record<string, unknown>;
}

/** A validated scenario. Instances are deep-frozen at load time. */
export type LoadedScenario = DeepReadonly<CompatScenario>;

export interface ScenarioDiagnostic {
  /** Machine-readable failure category (see FILE_DIAGNOSTIC_CODES below). */
  code: string;
  /** Absolute path of the scenario document being loaded. */
  file: string;
  /** Zero-based position within a scenario-set envelope, when applicable. */
  index?: number;
  /** Entry `id` when the entry carries a string id (even a malformed one). */
  scenarioId?: string;
  /** Dotted field path, e.g. `http.method` or `scenarios[2].id`. */
  path?: string;
  /** Concise reason. Never contains fixture content or secret values. */
  message: string;
}

export interface ScenarioLoadSuccess {
  ok: true;
  /** Whether the document held one scenario or a `scenarios` envelope. */
  kind: "scenario" | "scenario-set";
  file: string;
  scenarios: readonly LoadedScenario[];
}

export interface ScenarioLoadFailure {
  ok: false;
  kind: "unknown" | "scenario" | "scenario-set";
  file: string;
  diagnostics: readonly ScenarioDiagnostic[];
}

export type ScenarioLoadResult = ScenarioLoadSuccess | ScenarioLoadFailure;

export interface ScenarioLoaderOptions {
  /**
   * Override the canonical schema location (resolved against the caller's
   * cwd when relative). Defaults to the checked-in Task 005 contract,
   * resolved relative to this module so loading is cwd-independent.
   */
  schemaPath?: string;
}

// ---------------------------------------------------------------------------
// Diagnostic codes
// ---------------------------------------------------------------------------

/** Every `code` a ScenarioDiagnostic can carry. */
export const SCENARIO_DIAGNOSTIC_CODES = [
  "file-not-found",
  "file-unreadable",
  "read-error",
  "empty-file",
  "parse-error",
  "wrong-top-level-type",
  "envelope-invalid",
  "scenario-not-object",
  "unsupported-schema-version",
  "schema-error",
  "duplicate-scenario-id",
  "persistence-table-not-allowlisted",
  "persistence-allowlist-missing",
  "redirect-follow-conflict",
] as const;

export const SCENARIO_SCHEMA_ID = "compat-scenario/v1";

const DEFAULT_SCHEMA_PATH = fileURLToPath(
  new URL("../../docs/native-backend/contracts/compat-scenario.schema.json", import.meta.url)
);

// ---------------------------------------------------------------------------
// Schema compilation (cached per resolved schema path)
// ---------------------------------------------------------------------------

interface CompiledSchema {
  resolvedPath: string;
  validate: ValidateFunction;
}

const compiledByPath = new Map<string, CompiledSchema>();

function compileSchema(schemaPath: string): CompiledSchema {
  const resolvedPath = path.resolve(schemaPath);
  const cached = compiledByPath.get(resolvedPath);
  if (cached) return cached;
  let raw: string;
  try {
    raw = readFileSync(resolvedPath, "utf8");
  } catch (err) {
    throw new Error(
      `compat scenario loader: cannot read schema at ${resolvedPath}: ${messageOf(err)}`
    );
  }
  let schema: unknown;
  try {
    schema = JSON.parse(raw) as unknown;
  } catch (err) {
    throw new Error(
      `compat scenario loader: schema at ${resolvedPath} is not valid JSON: ${messageOf(err)}`
    );
  }
  if (!isRecord(schema) && typeof schema !== "boolean") {
    throw new Error(
      `compat scenario loader: schema at ${resolvedPath} is not a JSON Schema object`
    );
  }
  const ajv = new Ajv({ allErrors: true, strict: false, validateFormats: false });
  const compiled: CompiledSchema = { resolvedPath, validate: ajv.compile(schema) };
  compiledByPath.set(resolvedPath, compiled);
  return compiled;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

function messageOf(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function deepFreezeValue(value: unknown): void {
  if (value === null || typeof value !== "object" || Object.isFrozen(value)) return;
  for (const child of Object.values(value)) deepFreezeValue(child);
  Object.freeze(value);
}

/**
 * Convert an ajv JSON pointer (`/http/headers/accept`, `/tags/0`) to the
 * dotted form used in diagnostics (`http.headers.accept`, `tags[0]`).
 */
function pointerToDotted(pointer: string): string {
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

function prefixPath(prefix: string, dotted: string): string {
  if (!dotted) return prefix;
  if (!prefix) return dotted;
  return `${prefix}.${dotted}`;
}

/**
 * Reduce a JSON.parse SyntaxError to position information only. V8 messages
 * can echo a short slice of the offending input, so the raw message is never
 * propagated — fixture content must not leak into diagnostics.
 */
function parseErrorMessage(err: unknown): string {
  const text = messageOf(err);
  const position = text.match(/position (\d+)/)?.[1];
  if (position !== undefined) return `file is not valid JSON (error at byte position ${position})`;
  const lineColumn = text.match(/line (\d+) column (\d+)/);
  if (lineColumn)
    return `file is not valid JSON (error at line ${lineColumn[1]} column ${lineColumn[2]})`;
  return "file is not valid JSON";
}

function compareDiagnostics(a: ScenarioDiagnostic, b: ScenarioDiagnostic): number {
  if ((a.index ?? 0) !== (b.index ?? 0)) return (a.index ?? 0) - (b.index ?? 0);
  if ((a.path ?? "") !== (b.path ?? "")) return (a.path ?? "") < (b.path ?? "") ? -1 : 1;
  if (a.code !== b.code) return a.code < b.code ? -1 : 1;
  if (a.message !== b.message) return a.message < b.message ? -1 : 1;
  return 0;
}

// ---------------------------------------------------------------------------
// ajv error mapping (schema-controlled context only — never fixture values)
// ---------------------------------------------------------------------------

function messageForSchemaError(err: ErrorObject): string {
  switch (err.keyword) {
    case "required":
      return `missing required field '${String((err.params as { missingProperty?: unknown }).missingProperty ?? "?")}'`;
    case "additionalProperties":
      return `unknown field '${String((err.params as { additionalProperty?: unknown }).additionalProperty ?? "?")}' is not allowed`;
    case "enum": {
      const allowed = (err.params as { allowedValues?: unknown }).allowedValues;
      return Array.isArray(allowed)
        ? `value must be one of: ${allowed.map((v) => JSON.stringify(v)).join(", ")}`
        : "value is not one of the allowed values";
    }
    case "const":
      return "value does not match the required constant for this field";
    case "type":
      return `value must be of type ${(err.params as { type?: unknown }).type ?? "?"}`;
    case "pattern":
      return "value does not match the required pattern for this field";
    case "minLength":
      return "value must not be empty";
    case "minimum":
    case "maximum":
      return `value is outside the allowed range for this field`;
    case "oneOf":
    case "anyOf":
      return "value does not match any allowed shape for this field";
    default:
      return err.message ?? "value is invalid for this field";
  }
}

function pathForSchemaError(base: string, err: ErrorObject): string {
  const dotted = pointerToDotted(err.instancePath);
  const params = err.params as Record<string, unknown>;
  if (err.keyword === "required" && typeof params["missingProperty"] === "string") {
    return prefixPath(base, pointerToDotted(`${err.instancePath}/${params["missingProperty"]}`));
  }
  if (err.keyword === "additionalProperties" && typeof params["additionalProperty"] === "string") {
    return prefixPath(base, pointerToDotted(`${err.instancePath}/${params["additionalProperty"]}`));
  }
  return prefixPath(base, dotted);
}

// ---------------------------------------------------------------------------
// Per-scenario validation (schema first, then the narrow semantic layer)
// ---------------------------------------------------------------------------

interface EntryContext {
  file: string;
  index?: number;
  pathPrefix: string;
}

function validateScenarioEntry(
  entry: unknown,
  compiled: CompiledSchema,
  ctx: EntryContext
): { scenario?: LoadedScenario; diagnostics: ScenarioDiagnostic[] } {
  const scenarioId = isRecord(entry) && typeof entry["id"] === "string" ? entry["id"] : undefined;
  const diag = (code: string, message: string, path?: string): ScenarioDiagnostic => ({
    code,
    file: ctx.file,
    ...(ctx.index === undefined ? {} : { index: ctx.index }),
    ...(scenarioId === undefined ? {} : { scenarioId }),
    ...(path === undefined ? {} : { path }),
    message,
  });

  if (!isRecord(entry)) {
    return {
      diagnostics: [
        diag(
          "scenario-not-object",
          "scenario entry must be a JSON object",
          ctx.pathPrefix || undefined
        ),
      ],
    };
  }

  // Dedicated version gate before schema validation so callers can tell
  // "right shape, wrong contract version" apart from generic schema errors.
  if (typeof entry["schema"] === "string" && entry["schema"] !== SCENARIO_SCHEMA_ID) {
    return {
      diagnostics: [
        diag(
          "unsupported-schema-version",
          `unsupported scenario schema '${entry["schema"]}'; this loader supports '${SCENARIO_SCHEMA_ID}'`,
          prefixPath(ctx.pathPrefix, "schema") || "schema"
        ),
      ],
    };
  }

  const diagnostics: ScenarioDiagnostic[] = [];
  let valid = false;
  try {
    valid = compiled.validate(entry) as boolean;
  } catch {
    valid = false;
  }
  if (!valid) {
    for (const err of compiled.validate.errors ?? []) {
      diagnostics.push(
        diag("schema-error", messageForSchemaError(err), pathForSchemaError(ctx.pathPrefix, err))
      );
    }
    return { diagnostics };
  }

  // Narrow semantic layer: only cross-field invariants Task 005 states
  // explicitly and JSON Schema cannot express cleanly.
  const scenario = entry as unknown as CompatScenario;

  if (scenario.compare?.redirect !== undefined && scenario.http?.followRedirects === true) {
    diagnostics.push(
      diag(
        "redirect-follow-conflict",
        "scenarios that assert a raw redirect must not set http.followRedirects to true",
        prefixPath(ctx.pathPrefix, "http.followRedirects")
      )
    );
  }

  const tables = scenario.persistence?.tables;
  const observations = scenario.persistence?.observations ?? [];
  if (observations.length > 0 && tables === undefined) {
    diagnostics.push(
      diag(
        "persistence-allowlist-missing",
        "persistence.observations requires the persistence.tables allowlist",
        prefixPath(ctx.pathPrefix, "persistence.tables")
      )
    );
  } else if (tables !== undefined) {
    observations.forEach((observation, observationIndex) => {
      if (!tables.includes(observation.table)) {
        diagnostics.push(
          diag(
            "persistence-table-not-allowlisted",
            `persistence observation table '${observation.table}' is not listed in persistence.tables`,
            prefixPath(ctx.pathPrefix, `persistence.observations[${observationIndex}].table`)
          )
        );
      }
    });
  }

  if (diagnostics.length > 0) return { diagnostics };
  deepFreezeValue(scenario);
  return { scenario: scenario as LoadedScenario, diagnostics: [] };
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Load and validate one compatibility scenario document.
 *
 * The file may hold a single scenario object or a scenario-set envelope
 * (`{ ..., "scenarios": [...] }`). Returns deep-frozen scenarios on success
 * or deterministic, secret-free diagnostics on failure. Never throws for
 * ordinary invalid input; never exits the process; performs no I/O beyond
 * reading the requested file (plus the canonical schema on first use).
 */
export function loadScenarioFile(
  filePath: string,
  options: ScenarioLoaderOptions = {}
): ScenarioLoadResult {
  const file = path.resolve(filePath);

  let text: string;
  try {
    text = readFileSync(file, "utf8");
  } catch (err) {
    const errno = (err as NodeJS.ErrnoException)?.code;
    const code =
      errno === "ENOENT"
        ? "file-not-found"
        : errno === "EACCES" || errno === "EPERM" || errno === "EISDIR"
          ? "file-unreadable"
          : "read-error";
    const message =
      code === "file-not-found"
        ? "scenario file does not exist"
        : code === "file-unreadable"
          ? "scenario file cannot be read"
          : `scenario file cannot be read (${errno ?? "unknown error"})`;
    return { ok: false, kind: "unknown", file, diagnostics: [{ code, file, message }] };
  }

  if (text.trim() === "") {
    return {
      ok: false,
      kind: "unknown",
      file,
      diagnostics: [{ code: "empty-file", file, message: "scenario file is empty" }],
    };
  }

  let data: unknown;
  try {
    data = JSON.parse(text) as unknown;
  } catch (err) {
    return {
      ok: false,
      kind: "unknown",
      file,
      diagnostics: [{ code: "parse-error", file, message: parseErrorMessage(err) }],
    };
  }

  if (!isRecord(data)) {
    return {
      ok: false,
      kind: "unknown",
      file,
      diagnostics: [
        {
          code: "wrong-top-level-type",
          file,
          message:
            "scenario document must be a JSON object (one scenario or a scenario-set envelope)",
        },
      ],
    };
  }

  const compiled = compileSchema(options.schemaPath ?? DEFAULT_SCHEMA_PATH);

  if ("scenarios" in data) {
    const rawEntries = data["scenarios"];
    if (!Array.isArray(rawEntries)) {
      return {
        ok: false,
        kind: "scenario-set",
        file,
        diagnostics: [
          {
            code: "envelope-invalid",
            file,
            path: "scenarios",
            message: "scenario-set envelope field 'scenarios' must be an array",
          },
        ],
      };
    }
    const scenarios: LoadedScenario[] = [];
    const diagnostics: ScenarioDiagnostic[] = [];
    rawEntries.forEach((entry, index) => {
      const prefix = `scenarios[${index}]`;
      const result = validateScenarioEntry(entry, compiled, { file, index, pathPrefix: prefix });
      if (result.scenario) scenarios.push(result.scenario);
      diagnostics.push(...result.diagnostics);
    });
    // Set-level invariant (Task 005 §4.1): `id` is the observation/result join
    // key and must be unique per corpus.
    const firstIndexById = new Map<string, number>();
    rawEntries.forEach((entry, index) => {
      if (!isRecord(entry) || typeof entry["id"] !== "string") return;
      const first = firstIndexById.get(entry["id"]);
      if (first === undefined) {
        firstIndexById.set(entry["id"], index);
      } else {
        diagnostics.push({
          code: "duplicate-scenario-id",
          file,
          index,
          scenarioId: entry["id"],
          path: `scenarios[${index}].id`,
          message: `duplicate scenario id '${entry["id"]}' (first seen at scenarios[${first}].id)`,
        });
      }
    });
    if (diagnostics.length > 0) {
      diagnostics.sort(compareDiagnostics);
      return { ok: false, kind: "scenario-set", file, diagnostics };
    }
    deepFreezeValue(scenarios);
    return { ok: true, kind: "scenario-set", file, scenarios };
  }

  const result = validateScenarioEntry(data, compiled, { file, pathPrefix: "" });
  if (!result.scenario) {
    const diagnostics = [...result.diagnostics];
    diagnostics.sort(compareDiagnostics);
    return { ok: false, kind: "scenario", file, diagnostics };
  }
  const scenarios: LoadedScenario[] = [result.scenario];
  deepFreezeValue(scenarios);
  return { ok: true, kind: "scenario", file, scenarios };
}
