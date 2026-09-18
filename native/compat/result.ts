/**
 * Compatibility harness — Stage 2: result runtime model (Task 007).
 *
 * Runtime representation and validation boundary for comparison results.
 * This module performs NO comparison: it never generates mismatches from
 * observations, renders reports, or executes anything. It defines how later
 * comparator stages exchange verdicts and how serialized results re-enter
 * the harness for diagnosis.
 *
 * Design references:
 * - docs/native-backend/COMPATIBILITY_HARNESS.md section 13 (verdict model,
 *   mismatch structure, size discipline).
 * - Canonical runtime schema (single source of truth — never copied here):
 *   docs/native-backend/contracts/compat-result.schema.json
 *
 * Key Task 005 invariants preserved here:
 * - Verdicts use the exact canonical terms `pass`, `fail`, `error`: a pass
 *   means every declared expectation held, a failure means at least one
 *   mismatch, an error marks harness/execution failure. The canonical schema
 *   types the verdict but cannot relate it to the mismatch count, so that
 *   relationship is enforced semantically (codes `pass-with-mismatches` /
 *   `fail-without-mismatches`). `error` verdicts carry no count constraint.
 * - Mismatches keep the contract fields (dimension, path, rule, concise
 *   reference/native summaries, normalization flag, optional artifact
 *   pointer). Full bodies stay in artifacts; only bounded summaries travel
 *   in the result — a reporting-stage discipline the representation supports
 *   by keeping summaries plain strings.
 */

import { fileURLToPath } from "node:url";
import {
  compareCompatDiagnostics,
  compileCanonicalSchema,
  deepCloneJson,
  deepFreezeValue,
  formatAjvError,
  isRecord,
  stripUndefined,
  type CompatDiagnostic,
  type CompiledJsonSchema,
  type DeepReadonly,
} from "./schemaCore.ts";

// ---------------------------------------------------------------------------
// Public types (compile-time only; runtime truth is the canonical schema file)
// ---------------------------------------------------------------------------

export type ResultVerdict = "pass" | "fail" | "error";

export type MismatchDimension =
  | "status"
  | "headers"
  | "json"
  | "error"
  | "redirect"
  | "stream"
  | "cancel"
  | "persistence"
  | "timing"
  | "outcome";

export interface CompatMismatch {
  dimension: MismatchDimension;
  path: string;
  rule: string;
  reference: string;
  native: string;
  normalized: boolean;
  artifact?: string | null;
}

export interface ResultSide {
  /** Artifact path of the retained observation JSON. */
  observation: string;
  /** The observation outcome label (e.g. `http-response`). */
  outcome: string;
}

export interface ResultArtifacts {
  dir: string;
  result: string;
}

interface ResultBase {
  schema: "compat-result/v1";
  scenarioId: string;
  verdict: ResultVerdict;
  reference: ResultSide;
  native: ResultSide;
  mismatches: readonly CompatMismatch[];
  artifacts: ResultArtifacts;
}

export interface PassResult extends ResultBase {
  verdict: "pass";
  mismatches: readonly [];
}

export interface FailResult extends ResultBase {
  verdict: "fail";
  mismatches: readonly [CompatMismatch, ...CompatMismatch[]];
}

export interface ErrorResult extends ResultBase {
  verdict: "error";
  mismatches: readonly CompatMismatch[];
}

export type CompatResult = PassResult | FailResult | ErrorResult;

/** A constructed or validated result. Instances are deep-frozen. */
export type LoadedResult = DeepReadonly<CompatResult>;

export interface ResultDiagnostic extends CompatDiagnostic {
  /** Where the validated value came from (file path, artifact ref); omit for in-memory values. */
  source?: string;
}

export interface ResultValidationSuccess {
  ok: true;
  value: LoadedResult;
}

export interface ResultValidationFailure {
  ok: false;
  diagnostics: readonly ResultDiagnostic[];
}

export type ResultValidation = ResultValidationSuccess | ResultValidationFailure;

export interface ResultValidatorOptions {
  source?: string;
  /**
   * Override the canonical schema location (resolved against the caller's
   * cwd when relative). Defaults to the checked-in Task 005 contract,
   * resolved relative to this module so validation is cwd-independent.
   */
  schemaPath?: string;
}

// ---------------------------------------------------------------------------
// Diagnostic codes
// ---------------------------------------------------------------------------

/** Every `code` a ResultDiagnostic can carry. */
export const RESULT_DIAGNOSTIC_CODES = [
  "schema-error",
  "pass-with-mismatches",
  "fail-without-mismatches",
] as const;

export const RESULT_SCHEMA_ID = "compat-result/v1";

const DEFAULT_SCHEMA_PATH = fileURLToPath(
  new URL("../../docs/native-backend/contracts/compat-result.schema.json", import.meta.url)
);

// ---------------------------------------------------------------------------
// Construction helpers (verdict-shape-safe by construction, frozen on return)
// ---------------------------------------------------------------------------

export interface ResultSharedInit {
  scenarioId: string;
  reference: ResultSide;
  native: ResultSide;
  artifacts: ResultArtifacts;
}

/** Construct a `pass` result. Mismatches cannot be attached — by type. */
export function createPassResult(init: ResultSharedInit): LoadedResult {
  const result = stripUndefined({
    schema: "compat-result/v1",
    scenarioId: init.scenarioId,
    verdict: "pass" as const,
    reference: { ...init.reference },
    native: { ...init.native },
    mismatches: [],
    artifacts: { ...init.artifacts },
  });
  deepFreezeValue(result);
  return result as LoadedResult;
}

/**
 * Construct a `fail` result. At least one mismatch is required — by type —
 * so later comparator code cannot accidentally report failure without
 * evidence.
 */
export function createFailResult(
  init: ResultSharedInit & { mismatches: readonly [CompatMismatch, ...CompatMismatch[]] }
): LoadedResult {
  const result = stripUndefined({
    schema: "compat-result/v1",
    scenarioId: init.scenarioId,
    verdict: "fail" as const,
    reference: { ...init.reference },
    native: { ...init.native },
    mismatches: [...init.mismatches],
    artifacts: { ...init.artifacts },
  });
  deepFreezeValue(result);
  return result as LoadedResult;
}

/** Construct an `error` result. Mismatches may accompany it but need not. */
export function createErrorResult(
  init: ResultSharedInit & { mismatches?: readonly CompatMismatch[] }
): LoadedResult {
  const result = stripUndefined({
    schema: "compat-result/v1",
    scenarioId: init.scenarioId,
    verdict: "error" as const,
    reference: { ...init.reference },
    native: { ...init.native },
    mismatches: [...(init.mismatches ?? [])],
    artifacts: { ...init.artifacts },
  });
  deepFreezeValue(result);
  return result as LoadedResult;
}

/**
 * Detached mutable copy of a result. Report rendering and later stages work
 * on clones; frozen results are never modified in place.
 */
export function cloneResult(result: LoadedResult): CompatResult {
  return deepCloneJson(result) as CompatResult;
}

// ---------------------------------------------------------------------------
// Runtime validation (canonical schema + the narrow semantic layer)
// ---------------------------------------------------------------------------

function compileResultSchema(schemaPath: string | undefined): CompiledJsonSchema {
  return compileCanonicalSchema({
    label: "compat result validator",
    defaultPath: DEFAULT_SCHEMA_PATH,
    schemaPath,
  });
}

/**
 * Validate an externally loaded or deserialized result value against the
 * canonical schema plus the verdict/mismatch-count relationship (which the
 * schema cannot express). Returns deep-frozen values on success or
 * deterministic, secret-free diagnostics on failure. Never throws for
 * ordinary invalid data; performs no I/O beyond reading the canonical
 * schema on first use.
 */
export function validateResult(
  value: unknown,
  options: ResultValidatorOptions = {}
): ResultValidation {
  const source = options.source;
  const withSource = (diagnostic: CompatDiagnostic): ResultDiagnostic =>
    source === undefined ? diagnostic : { ...diagnostic, source };

  if (!isRecord(value)) {
    return {
      ok: false,
      diagnostics: [withSource({ code: "schema-error", message: "result must be a JSON object" })],
    };
  }

  const compiled = compileResultSchema(options.schemaPath);
  const diagnostics: ResultDiagnostic[] = [];
  let valid = false;
  try {
    valid = compiled.validate(value) as boolean;
  } catch {
    valid = false;
  }
  if (!valid) {
    for (const err of compiled.validate.errors ?? []) {
      const formatted = formatAjvError("", err);
      diagnostics.push(
        withSource({
          code: "schema-error",
          path: formatted.path || undefined,
          message: formatted.message,
        })
      );
    }
    diagnostics.sort(compareCompatDiagnostics);
    return { ok: false, diagnostics };
  }

  // Narrow semantic layer: the verdict/mismatch-count relationship Task 005
  // section 13 states explicitly. Dimension enums, required mismatch fields,
  // and artifact shapes stay schema-side and are not duplicated here.
  const record = value as Record<string, unknown>;
  const mismatchCount = Array.isArray(record["mismatches"]) ? record["mismatches"].length : 0;
  if (record["verdict"] === "pass" && mismatchCount > 0) {
    diagnostics.push(
      withSource({
        code: "pass-with-mismatches",
        path: "mismatches",
        message: "results with verdict 'pass' must not contain mismatches",
      })
    );
  } else if (record["verdict"] === "fail" && mismatchCount === 0) {
    diagnostics.push(
      withSource({
        code: "fail-without-mismatches",
        path: "mismatches",
        message: "results with verdict 'fail' must contain at least one mismatch",
      })
    );
  }

  if (diagnostics.length > 0) {
    diagnostics.sort(compareCompatDiagnostics);
    return { ok: false, diagnostics };
  }
  deepFreezeValue(value);
  return { ok: true, value: value as LoadedResult };
}
