/**
 * Compatibility harness — Stage 2: observation runtime model (Task 007).
 *
 * Runtime representation and validation boundary for captured per-backend
 * observations. This module performs NO capture: it never sends HTTP, starts
 * backends, or reads artifact files. It defines how later execution stages
 * exchange observation data and how serialized observations re-enter the
 * harness for diagnosis.
 *
 * Design references:
 * - docs/native-backend/COMPATIBILITY_HARNESS.md section 5 (observation
 *   schema, outcome taxonomy, field notes).
 * - Canonical runtime schema (single source of truth — never copied here):
 *   docs/native-backend/contracts/compat-observation.schema.json
 *
 * Key Task 005 invariants preserved here:
 * - Six distinct outcomes (`http-response`, `timeout`, `connection-refused`,
 *   `connection-reset`, `backend-exit`, `harness-error`) stay
 *   distinguishable; they are never collapsed into a generic error.
 * - HTTP status is present if and only if `outcome === "http-response"`.
 *   The canonical schema types `status` but cannot express that cross-field
 *   rule, so it is enforced semantically (codes
 *   `status-missing-for-http-response` /
 *   `status-present-for-non-http-outcome`). A missing response is never
 *   status `0`: non-HTTP observations carry no `status` key at all.
 * - `headers`/`body` are schema-required on every observation; an outcome
 *   with nothing captured (e.g. `connection-refused`) uses the empty
 *   representation (`headers: []`, `body: { bytes: 0, truncated: false }`).
 * - Header names are lowercased at construction (capture hygiene per the
 *   design, not normalization); repeated headers keep every value in
 *   received order so future comparison stays possible. Header *values* are
 *   never touched here.
 * - Body metadata (`bytes`/`sha256`/`truncated`/`capName`/`artifact`/
 *   `textPreview`) already represents the bounded-capture strategy: no new
 *   fields, no spill implementation, no buffer limits invented in this task.
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

export type ObservationOutcome =
  | "http-response"
  | "timeout"
  | "connection-refused"
  | "connection-reset"
  | "backend-exit"
  | "harness-error";

export type NonHttpOutcome = Exclude<ObservationOutcome, "http-response">;

export type BackendId = "reference" | "native";

export interface ObservationBackend {
  id: BackendId;
  revision?: string;
  mode?: string;
}

export interface ObservationHeader {
  name: string;
  value: string;
}

export interface ObservationBody {
  bytes: number;
  sha256?: string;
  truncated: boolean;
  capName?: string;
  artifact?: string;
  textPreview?: string;
}

export interface ObservationJsonParse {
  skipped?: boolean;
  reason?: string;
}

export interface ObservationRedirect {
  location?: string;
  refresh?: string;
}

export interface ObservationStream {
  rawArtifact?: string;
  rawBytes?: number;
  rawSha256?: string;
  eventsArtifact?: string;
  eventCount?: number;
  eventsTruncated?: boolean;
  terminal?: string | null;
  closedCleanly?: boolean;
  cancelled?: boolean;
  timeoutMs?: number | null;
}

export interface ObservationFailure {
  kind: string;
  detail?: string;
}

export interface ObservationPersistence {
  observations?: Record<string, unknown>[];
  settledAfterMs?: number;
}

export interface ObservationTiming {
  wallMs?: number;
  ttfbMs?: number | null;
}

interface ObservationBase {
  schema: "compat-observation/v1";
  scenarioId: string;
  backend: ObservationBackend;
  outcome: ObservationOutcome;
  headers: ObservationHeader[];
  body: ObservationBody;
  json?: unknown;
  jsonParse?: ObservationJsonParse;
  redirect?: ObservationRedirect | null;
  stream?: ObservationStream | null;
  failure?: ObservationFailure | null;
  persistence?: ObservationPersistence;
  timing?: ObservationTiming;
  normalizedBy?: string[];
}

export interface HttpResponseObservation extends ObservationBase {
  outcome: "http-response";
  status: number;
}

export interface NonHttpObservation extends ObservationBase {
  outcome: NonHttpOutcome;
  status?: never;
}

export type CompatObservation = HttpResponseObservation | NonHttpObservation;

/** A constructed or validated observation. Instances are deep-frozen. */
export type LoadedObservation = DeepReadonly<CompatObservation>;

export interface ObservationDiagnostic extends CompatDiagnostic {
  /** Where the validated value came from (file path, artifact ref); omit for in-memory values. */
  source?: string;
}

export interface ObservationValidationSuccess {
  ok: true;
  value: LoadedObservation;
}

export interface ObservationValidationFailure {
  ok: false;
  diagnostics: readonly ObservationDiagnostic[];
}

export type ObservationValidation = ObservationValidationSuccess | ObservationValidationFailure;

export interface ObservationValidatorOptions {
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

/** Every `code` an ObservationDiagnostic can carry. */
export const OBSERVATION_DIAGNOSTIC_CODES = [
  "schema-error",
  "status-missing-for-http-response",
  "status-present-for-non-http-outcome",
] as const;

export const OBSERVATION_SCHEMA_ID = "compat-observation/v1";

const DEFAULT_SCHEMA_PATH = fileURLToPath(
  new URL("../../docs/native-backend/contracts/compat-observation.schema.json", import.meta.url)
);

// ---------------------------------------------------------------------------
// Construction helpers (shape-safe by construction, frozen on return)
// ---------------------------------------------------------------------------

export interface ObservationHeaderInit {
  name: string;
  value: string;
}

export interface ObservationSharedInit {
  scenarioId: string;
  backend: ObservationBackend;
  headers?: readonly ObservationHeaderInit[];
  body?: ObservationBody;
  json?: unknown;
  jsonParse?: ObservationJsonParse;
  redirect?: ObservationRedirect | null;
  stream?: ObservationStream | null;
  failure?: ObservationFailure | null;
  persistence?: ObservationPersistence;
  timing?: ObservationTiming;
  normalizedBy?: string[];
}

function buildHeaders(headers: readonly ObservationHeaderInit[] | undefined): ObservationHeader[] {
  return (headers ?? []).map((entry) => ({ name: entry.name.toLowerCase(), value: entry.value }));
}

function buildShared(init: ObservationSharedInit): ObservationBase {
  return stripUndefined({
    schema: "compat-observation/v1",
    scenarioId: init.scenarioId,
    backend: { ...init.backend },
    outcome: "http-response",
    headers: buildHeaders(init.headers),
    body: init.body ?? { bytes: 0, truncated: false },
    json: init.json,
    jsonParse: init.jsonParse,
    redirect: init.redirect ?? null,
    stream: init.stream ?? null,
    failure: init.failure ?? null,
    persistence: init.persistence,
    timing: init.timing,
    normalizedBy: init.normalizedBy,
  }) as ObservationBase;
}

/**
 * Construct an `http-response` observation. `status` is required; omitting it
 * is a compile-time error, so later executor code cannot accidentally build a
 * response observation without one.
 */
export function createHttpResponseObservation(
  init: ObservationSharedInit & { status: number }
): LoadedObservation {
  const observation = {
    ...buildShared(init),
    outcome: "http-response" as const,
    status: init.status,
  };
  deepFreezeValue(observation);
  return observation as LoadedObservation;
}

/**
 * Construct a non-HTTP observation (`timeout`, `connection-refused`,
 * `connection-reset`, `backend-exit`, `harness-error`). No `status` key is
 * accepted — a missing response can never become status `0` through this API.
 */
export function createNonHttpObservation(
  outcome: NonHttpOutcome,
  init: ObservationSharedInit
): LoadedObservation {
  const observation = { ...buildShared(init), outcome };
  deepFreezeValue(observation);
  return observation as LoadedObservation;
}

/**
 * Detached mutable copy of an observation. Normalization and later stages
 * work on clones; frozen raw evidence is never modified in place.
 */
export function cloneObservation(observation: LoadedObservation): CompatObservation {
  return deepCloneJson(observation) as CompatObservation;
}

// ---------------------------------------------------------------------------
// Runtime validation (canonical schema + the narrow semantic layer)
// ---------------------------------------------------------------------------

function compileObservationSchema(schemaPath: string | undefined): CompiledJsonSchema {
  return compileCanonicalSchema({
    label: "compat observation validator",
    defaultPath: DEFAULT_SCHEMA_PATH,
    schemaPath,
  });
}

/**
 * Validate an externally loaded or deserialized observation value against
 * the canonical schema plus the status-iff-`http-response` invariant (which
 * the schema cannot express). Returns deep-frozen values on success or
 * deterministic, secret-free diagnostics on failure. Never throws for
 * ordinary invalid data; performs no I/O beyond reading the canonical
 * schema on first use.
 */
export function validateObservation(
  value: unknown,
  options: ObservationValidatorOptions = {}
): ObservationValidation {
  const source = options.source;
  const withSource = (diagnostic: CompatDiagnostic): ObservationDiagnostic =>
    source === undefined ? diagnostic : { ...diagnostic, source };

  if (!isRecord(value)) {
    return {
      ok: false,
      diagnostics: [
        withSource({ code: "schema-error", message: "observation must be a JSON object" }),
      ],
    };
  }

  const compiled = compileObservationSchema(options.schemaPath);
  const diagnostics: ObservationDiagnostic[] = [];
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

  // Narrow semantic layer: the one cross-field invariant Task 005 states
  // explicitly that JSON Schema cannot express. Range/type checks stay
  // schema-side and are not duplicated here.
  const record = value as Record<string, unknown>;
  const hasStatus = record["status"] !== undefined;
  if (record["outcome"] === "http-response" && !hasStatus) {
    diagnostics.push(
      withSource({
        code: "status-missing-for-http-response",
        path: "status",
        message: "observations with outcome 'http-response' must carry an HTTP status",
      })
    );
  } else if (record["outcome"] !== "http-response" && hasStatus) {
    diagnostics.push(
      withSource({
        code: "status-present-for-non-http-outcome",
        path: "status",
        message: `observations with outcome '${String(record["outcome"])}' must not carry an HTTP status`,
      })
    );
  }

  if (diagnostics.length > 0) {
    diagnostics.sort(compareCompatDiagnostics);
    return { ok: false, diagnostics };
  }
  deepFreezeValue(value);
  return { ok: true, value: value as LoadedObservation };
}
