/**
 * Compatibility harness — Stage 3: single-backend HTTP executor (Task 008).
 *
 * Executes one already-loaded compatibility scenario against one
 * caller-provided backend base URL and captures a raw, bounded
 * `CompatObservation`. This module is the generic HTTP execution/capture
 * boundary only.
 *
 * Stage boundary (see `docs/native-backend/COMPATIBILITY_HARNESS.md` §3):
 * loaded scenario + backend identity + base URL + already-resolved request
 * inputs → bounded HTTP execution → raw observation. This module does NOT
 * start backends, provision authentication, compare backends, normalize
 * observations, generate results, parse SSE/WebSocket streams, persist
 * anything, or emit C code. It never imports the result model.
 *
 * Design references:
 * - `docs/native-backend/COMPATIBILITY_HARNESS.md` §§5 (observation
 *   taxonomy), 11 (adapter boundary — the caller owns lifecycle), 12
 *   (timeouts, capture bounds).
 * - Canonical runtime schema (single source of truth — never copied here):
 *   `docs/native-backend/contracts/compat-observation.schema.json`.
 * - Loaded-scenario shape: `native/compat/scenarioLoader.ts` (Task 006).
 * - Observation factories/validators: `native/compat/observation.ts`
 *   (Task 007). Every observation returned here is built through those
 *   factories, so outputs are deep-frozen and schema-valid by construction.
 *
 * Timeout categories follow the Task 005 evidence anchors (§12): `health`
 * 5 s, `api` 10 s, `chat` 20 s, `stream` 30 s (all from existing bounded
 * waits in `tests/integration/resilience-http-e2e.test.ts` and
 * `tests/e2e/protocol-clients.test.ts`). `bulk` has no evidence anchor, so
 * Stage 3 sets an explicit 60 s default; later stages may tune it. A missing
 * scenario timeout defaults to the `api` category. Every request carries a
 * finite `AbortSignal.timeout`, so no fetch is ever unbounded.
 *
 * Capture bounds follow the Task 005 range evidence (§12: 64 KiB catalog
 * chunk threshold up to the 8 MiB ingest floor). Stage 3 defaults to
 * 1 MiB in-memory body capture with no spill-to-file: bodies past the cap
 * stay `http-response` with `truncated: true` + `capName`, and JSON parsing
 * is skipped with a recorded reason. Spill-to-file (if ever needed) belongs
 * to a later capture stage that owns artifact directories — this executor
 * performs no filesystem I/O at all.
 *
 * Transport-error classification uses structured error identity
 * (`name`/`code` through the `cause` chain, cf. the `RETRYABLE_*` tables in
 * `open-sse/services/streamRecovery.ts`) — never brittle message matching
 * alone. A message pattern is only a fallback for undici socket errors that
 * already carry `UND_ERR_*` codes. Verified against Node 26 (undici) fetch:
 * timeout surfaces as `TimeoutError` (fetch and body-read phases alike),
 * refused connections as `cause.code === "ECONNREFUSED"`, resets as
 * `UND_ERR_SOCKET` (`SocketError`, "other side closed" / "terminated").
 * This executor has no process handle, so it never reports `backend-exit`;
 * correlating a dead process with a failed request belongs to Task 009+.
 *
 * Fetch fidelity limits (verified by probe, documented — not worked around):
 * - Undici merges repeated response headers with `", "` except `set-cookie`,
 *   which stays split (both in `Headers.entries()` and via
 *   `getSetCookie()`). Non-cookie duplicates therefore cannot be recovered
 *   as separate values; the observation keeps the combined form.
 * - Undici adds transport headers (`host`, `connection`, `accept`,
 *   `accept-language`, `sec-fetch-mode`, `user-agent`, `accept-encoding`,
 *   `content-length`) that the scenario never declared. Scenario string
 *   bodies are sent as UTF-8 bytes (not `string`) so undici does not add
 *   `text/plain;charset=UTF-8`; nothing else semantic is added here.
 * - Fetch forbids `CONNECT`/`TRACE`/`TRACK` and any body on `GET`/`HEAD`
 *   (throws `TypeError` before touching the network). Such scenarios yield
 *   `harness-error`, never an observation of the backend.
 * - `HEAD` responses expose a null body stream; zero bytes are captured
 *   without waiting on `Content-Length`.
 *
 * Secret safety: failure `detail` strings are fixed templates that name
 * field categories, never values. Request header values, request bodies,
 * query values, URLs with userinfo, and thrown error messages are never
 * copied into observations or diagnostics.
 */

import { createHash } from "node:crypto";
import {
  createHttpResponseObservation,
  createNonHttpObservation,
  type LoadedObservation,
  type NonHttpOutcome,
  type ObservationBackend,
  type ObservationBody,
  type ObservationHeaderInit,
} from "./observation.ts";
import type { LoadedScenario } from "./scenarioLoader.ts";

// ---------------------------------------------------------------------------
// Public configuration
// ---------------------------------------------------------------------------

/** Timeout categories from the Task 005 scenario contract. */
export const COMPAT_TIMEOUT_CATEGORIES = ["health", "api", "chat", "stream", "bulk"] as const;

export type CompatTimeoutCategory = (typeof COMPAT_TIMEOUT_CATEGORIES)[number];

/**
 * Per-category timeout overrides (milliseconds). Each entry must be a finite
 * positive number when provided; the scenario's `timeout` category selects
 * which entry applies.
 */
export interface CompatExecutorTimeouts {
  healthMs?: number;
  apiMs?: number;
  chatMs?: number;
  streamMs?: number;
  bulkMs?: number;
}

/**
 * Stage 3 timeout defaults (milliseconds). `health`/`api`/`chat`/`stream`
 * are Task 005 evidence anchors; `bulk` is an explicit Stage 3 choice with
 * no evidence anchor (see module doc).
 */
export const DEFAULT_COMPAT_TIMEOUTS_MS: Record<CompatTimeoutCategory, number> = {
  health: 5_000,
  api: 10_000,
  chat: 20_000,
  stream: 30_000,
  bulk: 60_000,
};

/** Timeout category used when the scenario declares none. */
export const DEFAULT_COMPAT_TIMEOUT_CATEGORY: CompatTimeoutCategory = "api";

/** In-memory capture limits (bytes). All must be finite and >= 1. */
export interface CompatExecutorCaptureLimits {
  /** Maximum retained response-body bytes; past this the body truncates. */
  maxBodyBytes?: number;
  /** Maximum body size still eligible for JSON parsing. */
  maxParseBytes?: number;
  /** Maximum UTF-8 bytes surfaced as `body.textPreview`. */
  textPreviewBytes?: number;
}

/** Stage 3 capture defaults (see module doc for the evidence range). */
export const DEFAULT_COMPAT_CAPTURE_LIMITS: Required<CompatExecutorCaptureLimits> = {
  maxBodyBytes: 1_048_576,
  maxParseBytes: 1_048_576,
  textPreviewBytes: 4_096,
};

/** `capName` recorded when the body capture bound fires. */
export const COMPAT_BODY_CAP_NAME = "capture.maxBodyBytes";

/** Backend identity stamped into observations (never compared). */
export type CompatBackendIdentity = ObservationBackend;

/** Test seam for the fetch implementation (defaults to global fetch). */
export type CompatFetchImpl = typeof fetch;

export interface ExecuteCompatScenarioOptions {
  /** Already-loaded (deep-frozen) scenario from Task 006. Never mutated. */
  scenario: LoadedScenario;
  /** Caller-provided backend identity (`reference` or `native`). */
  backend: CompatBackendIdentity;
  /**
   * Caller-provided backend base URL (adapter-owned, e.g. from `waitReady`).
   * Must be an absolute `http(s)` URL without embedded credentials; the
   * scenario path/query is appended to any base path prefix.
   */
  baseUrl: string;
  /**
   * Already-resolved request headers (e.g. provisioned credentials from a
   * later auth stage). Merged over the scenario's headers
   * case-insensitively; values are never logged or echoed. The executor
   * itself never provisions credentials.
   */
  resolvedHeaders?: Record<string, string>;
  /** Explicit timeout in ms; overrides the scenario-category mapping. */
  timeoutMs?: number;
  /** Per-category timeout overrides. */
  timeouts?: CompatExecutorTimeouts;
  /** Capture-limit overrides. */
  capture?: CompatExecutorCaptureLimits;
  /** Dependency-injection seam for tests; production code omits it. */
  fetchImpl?: CompatFetchImpl;
}

// ---------------------------------------------------------------------------
// Structured harness failures (detail strings are fixed, secret-free templates)
// ---------------------------------------------------------------------------

class CompatHarnessError extends Error {
  readonly detail: string;

  constructor(detail: string) {
    super(detail);
    this.name = "CompatHarnessError";
    this.detail = detail;
  }
}

// ---------------------------------------------------------------------------
// Pure helpers (exported so tests can pin them without network I/O)
// ---------------------------------------------------------------------------

/**
 * Resolve the finite timeout budget for one execution. An explicit
 * `timeoutMs` wins; otherwise the scenario category (default `api`) maps
 * through overrides-then-defaults. Throws `CompatHarnessError` for
 * non-finite/non-positive budgets instead of running an unbounded fetch.
 */
export function resolveCompatTimeoutMs(
  category: CompatTimeoutCategory | undefined,
  options: { timeoutMs?: number; timeouts?: CompatExecutorTimeouts } = {}
): number {
  if (options.timeoutMs !== undefined) {
    if (!Number.isFinite(options.timeoutMs) || options.timeoutMs <= 0) {
      throw new CompatHarnessError(
        "explicit executor timeout must be a finite positive millisecond count"
      );
    }
    return options.timeoutMs;
  }
  const selected: CompatTimeoutCategory = category ?? DEFAULT_COMPAT_TIMEOUT_CATEGORY;
  const override = options.timeouts?.[`${selected}Ms` as keyof CompatExecutorTimeouts];
  const resolved = override ?? DEFAULT_COMPAT_TIMEOUTS_MS[selected];
  if (!Number.isFinite(resolved) || resolved <= 0) {
    throw new CompatHarnessError(
      `timeout budget for category '${selected}' must be a finite positive millisecond count`
    );
  }
  return resolved;
}

/**
 * Build the request URL from the caller base URL plus the scenario path and
 * query. The scenario path is appended to any base path prefix (a trailing
 * slash on the base never produces a double slash); query entries serialize
 * in insertion order with `encodeURIComponent` (`%20`, never `+`, for
 * spaces). The loaded scenario is only read, never mutated. Throws
 * `CompatHarnessError` — without echoing the offending value — for
 * non-absolute/non-http(s) base URLs, embedded credentials, non-absolute
 * scenario paths, or a construction that would escape the base origin.
 */
export function buildCompatRequestUrl(
  baseUrl: string,
  scenarioPath: string,
  scenarioQuery?: Readonly<Record<string, string>>
): string {
  let base: URL;
  try {
    base = new URL(baseUrl);
  } catch {
    throw new CompatHarnessError("executor baseUrl must be an absolute http(s) URL");
  }
  if (base.protocol !== "http:" && base.protocol !== "https:") {
    throw new CompatHarnessError("executor baseUrl must be an absolute http(s) URL");
  }
  if (base.username !== "" || base.password !== "") {
    throw new CompatHarnessError("executor baseUrl must not embed credentials");
  }
  if (typeof scenarioPath !== "string" || !scenarioPath.startsWith("/")) {
    throw new CompatHarnessError("scenario request path must be an absolute path");
  }
  const prefix = base.pathname.replace(/\/+$/, "");
  const entries = Object.entries(scenarioQuery ?? {});
  for (const [key, value] of entries) {
    if (typeof key !== "string" || typeof value !== "string") {
      throw new CompatHarnessError("scenario query entries must be string-valued");
    }
  }
  const query =
    entries.length === 0
      ? ""
      : `?${entries.map(([key, value]) => `${encodeURIComponent(key)}=${encodeURIComponent(value)}`).join("&")}`;
  let url: URL;
  try {
    url = new URL(`${base.protocol}//${base.host}${prefix}${scenarioPath}${query}`);
  } catch {
    throw new CompatHarnessError("scenario request target is not a usable http(s) URL");
  }
  if (url.origin !== base.origin) {
    throw new CompatHarnessError("scenario request target escapes the backend origin");
  }
  return url.toString();
}

/**
 * Merge scenario headers (base) with caller-resolved headers (which win on
 * case-insensitive name collision, keeping the winner's original casing).
 * Returns a fresh object; both inputs are only read. Throws
 * `CompatHarnessError` for non-string names/values without echoing values.
 */
export function mergeCompatRequestHeaders(
  scenarioHeaders?: Readonly<Record<string, string>>,
  resolvedHeaders?: Readonly<Record<string, string>>
): Record<string, string> {
  const merged: Record<string, string> = {};
  const byLower = new Map<string, string>();
  const apply = (source: Readonly<Record<string, string>> | undefined, side: string): void => {
    for (const [name, value] of Object.entries(source ?? {})) {
      if (typeof name !== "string" || name.length === 0 || typeof value !== "string") {
        throw new CompatHarnessError(`${side} request headers must be string-valued`);
      }
      const lower = name.toLowerCase();
      const existing = byLower.get(lower);
      if (existing !== undefined) delete merged[existing];
      merged[name] = value;
      byLower.set(lower, name);
    }
  };
  apply(scenarioHeaders, "scenario");
  apply(resolvedHeaders, "resolved");
  return merged;
}

/** Transport-error classes the executor can distinguish. */
export type CompatTransportClass =
  "timeout" | "connection-refused" | "connection-reset" | "unknown";

interface ErrorLink {
  name?: string;
  code?: string;
  message?: string;
}

function collectErrorChain(error: unknown): ErrorLink[] {
  const chain: ErrorLink[] = [];
  const seen = new Set<unknown>();
  let current: unknown = error;
  for (let depth = 0; depth < 6; depth++) {
    if (current === null || (typeof current !== "object" && typeof current !== "function")) break;
    if (seen.has(current)) break;
    seen.add(current);
    const record = current as {
      name?: unknown;
      code?: unknown;
      message?: unknown;
      cause?: unknown;
    };
    chain.push({
      name: typeof record.name === "string" ? record.name : undefined,
      code:
        typeof record.code === "string"
          ? record.code
          : typeof record.code === "number"
            ? String(record.code)
            : undefined,
      message: typeof record.message === "string" ? record.message : undefined,
    });
    current = record.cause;
  }
  return chain;
}

const RESET_MESSAGE_PATTERN =
  /terminated|socket hang up|other side closed|premature close|econnreset/i;

/**
 * Classify a fetch/body-read failure from structured error identity
 * (`name`/`code` through the `cause` chain). A message pattern is only a
 * fallback for undici socket errors that already carry `UND_ERR_*` codes.
 * Anything unrecognized is `unknown` (the caller maps it to
 * `harness-error`); `backend-exit` is never produced here because this
 * executor holds no process handle.
 */
export function classifyCompatTransportError(error: unknown): CompatTransportClass {
  const chain = collectErrorChain(error);
  if (chain.some((link) => link.name === "TimeoutError" || link.name === "BodyTimeoutError")) {
    return "timeout";
  }
  if (chain.some((link) => link.code === "ECONNREFUSED")) return "connection-refused";
  if (
    chain.some(
      (link) =>
        link.code === "ECONNRESET" ||
        link.code === "EPIPE" ||
        (link.code !== undefined && link.code.startsWith("UND_ERR_"))
    )
  ) {
    return "connection-reset";
  }
  if (
    chain.some((link) => link.message !== undefined && RESET_MESSAGE_PATTERN.test(link.message))
  ) {
    return "connection-reset";
  }
  return "unknown";
}

/** True when the response content-type declares a JSON body. */
export function isCompatJsonContentType(contentType: string | null): boolean {
  if (contentType === null) return false;
  return contentType.split(";")[0].trim().toLowerCase() === "application/json";
}

function sha256Hex(bytes: Uint8Array): string {
  return createHash("sha256").update(bytes).digest("hex");
}

/**
 * Capture response headers at the maximum fidelity the fetch API exposes:
 * received order preserved, `set-cookie` values kept split (via
 * `getSetCookie()` when `entries()` would combine them), every other
 * repeated header kept in the runtime-combined form. Names are lowercased
 * again by the observation factory; values pass through untouched.
 */
export function captureCompatResponseHeaders(headers: Headers): ObservationHeaderInit[] {
  const entries = [...headers.entries()] as Array<[string, string]>;
  let splitCookies: string[] | null = null;
  if (typeof headers.getSetCookie === "function") {
    try {
      splitCookies = headers.getSetCookie();
    } catch {
      splitCookies = null;
    }
  }
  const cookieEntries = entries.filter(([name]) => name.toLowerCase() === "set-cookie");
  if (splitCookies !== null && cookieEntries.length === 1 && splitCookies.length > 1) {
    const out: ObservationHeaderInit[] = [];
    for (const [name, value] of entries) {
      if (name.toLowerCase() === "set-cookie") {
        for (const cookie of splitCookies) out.push({ name, value: cookie });
      } else {
        out.push({ name, value });
      }
    }
    return out;
  }
  return entries.map(([name, value]) => ({ name, value }));
}

// ---------------------------------------------------------------------------
// Request-body resolution
// ---------------------------------------------------------------------------

/**
 * Resolve the scenario-declared body to bytes. `null`/absent means no body;
 * a string is sent as UTF-8 bytes (so undici adds no `text/plain`
 * content-type); an empty string on `GET`/`HEAD` collapses to no body
 * because fetch cannot send any body there. Throws `CompatHarnessError`
 * for `{ artifact }` refs (Stage 006 left them unresolved; resolving them
 * against a corpus root belongs to a later corpus layer, so Stage 3
 * refuses rather than inventing a filesystem root) and for non-empty
 * `GET`/`HEAD` bodies.
 */
function resolveCompatRequestBody(
  scenario: LoadedScenario,
  method: string
): Uint8Array<ArrayBuffer> | undefined {
  const body = scenario.http.body ?? null;
  if (body === null) return undefined;
  if (typeof body === "string") {
    if (body.length === 0 && (method === "GET" || method === "HEAD")) return undefined;
    if (method === "GET" || method === "HEAD") {
      throw new CompatHarnessError(
        "scenario declares a request body for a GET/HEAD request, which fetch cannot send"
      );
    }
    return new TextEncoder().encode(body);
  }
  throw new CompatHarnessError(
    "scenario request body references an artifact, which Stage 3 does not resolve"
  );
}

function resolveCaptureLimit(value: number | undefined, fallback: number): number {
  const resolved = value ?? fallback;
  if (!Number.isFinite(resolved) || resolved < 1) {
    throw new CompatHarnessError(
      "executor capture limits must be finite counts of at least 1 byte"
    );
  }
  return Math.floor(resolved);
}

// ---------------------------------------------------------------------------
// Single-backend execution
// ---------------------------------------------------------------------------

type JsonParseReason =
  "non-json-content-type" | "empty-body" | "truncated" | "over-parse-cap" | "malformed-json";

function safeOriginOf(url: URL): string {
  return `${url.protocol}//${url.host}`;
}

function previewOf(retained: Buffer, textPreviewBytes: number): { textPreview?: string } {
  if (retained.length === 0) return {};
  return {
    textPreview: retained.subarray(0, Math.min(retained.length, textPreviewBytes)).toString("utf8"),
  };
}

/**
 * Execute one loaded scenario against one backend base URL and capture a
 * raw `CompatObservation`.
 *
 * - Ordinary HTTP responses (any status, including 4xx/5xx, and HTTP
 *   responses with malformed JSON) become `http-response` observations.
 * - Timeouts, refused connections, and reset connections become distinct
 *   non-HTTP observations. A missing response is never status `0`:
 *   non-HTTP observations carry no `status` key at all.
 * - Anything else — invalid URLs, artifact bodies, fetch-guard rejections,
 *   unexpected runtime failures — becomes `harness-error` with a
 *   secret-free fixed detail string.
 * - Response bodies are read incrementally and never retained past
 *   `maxBodyBytes`; over-cap bodies stay `http-response` with
 *   `truncated: true`.
 * - The scenario is only read, never mutated; every returned observation is
 *   deep-frozen by the Task 007 factories.
 */
export async function executeCompatScenario(
  options: ExecuteCompatScenarioOptions
): Promise<LoadedObservation> {
  const startedMs = Date.now();
  const scenarioId =
    typeof options?.scenario?.id === "string" && options.scenario.id.length > 0
      ? options.scenario.id
      : "unknown-scenario";
  const backendId = options?.backend?.id === "native" ? "native" : "reference";
  const backend: ObservationBackend = { id: backendId };
  if (typeof options?.backend?.revision === "string") backend.revision = options.backend.revision;
  if (typeof options?.backend?.mode === "string") backend.mode = options.backend.mode;

  const nonHttp = (
    outcome: NonHttpOutcome,
    failureKind: string,
    detail: string,
    partial: Buffer,
    wallMs: number,
    ttfbMs: number | null,
    previewBytes: number = DEFAULT_COMPAT_CAPTURE_LIMITS.textPreviewBytes
  ): LoadedObservation => {
    const body: ObservationBody = {
      bytes: partial.length,
      truncated: partial.length > 0,
      ...previewOf(partial, previewBytes),
    };
    return createNonHttpObservation(outcome, {
      scenarioId,
      backend,
      headers: [],
      body,
      failure: { kind: failureKind, detail },
      timing: { wallMs, ttfbMs },
    });
  };

  try {
    const scenario = options.scenario;
    if (scenario === null || typeof scenario !== "object" || typeof scenario.http !== "object") {
      throw new CompatHarnessError("executor requires a loaded compatibility scenario");
    }
    const maxBodyBytes = resolveCaptureLimit(
      options.capture?.maxBodyBytes,
      DEFAULT_COMPAT_CAPTURE_LIMITS.maxBodyBytes
    );
    const maxParseBytes = resolveCaptureLimit(
      options.capture?.maxParseBytes,
      DEFAULT_COMPAT_CAPTURE_LIMITS.maxParseBytes
    );
    const textPreviewBytes = resolveCaptureLimit(
      options.capture?.textPreviewBytes,
      DEFAULT_COMPAT_CAPTURE_LIMITS.textPreviewBytes
    );
    const timeoutMs = resolveCompatTimeoutMs(scenario.timeout, options);
    const category = scenario.timeout ?? DEFAULT_COMPAT_TIMEOUT_CATEGORY;
    const urlText = buildCompatRequestUrl(
      baseUrlOf(options),
      scenario.http.path,
      scenario.http.query
    );
    const baseForOrigin = new URL(urlText);
    const safeOrigin = safeOriginOf(baseForOrigin);
    const headers = mergeCompatRequestHeaders(scenario.http.headers, options.resolvedHeaders);
    const method = scenario.http.method;
    if (typeof method !== "string" || method.length === 0) {
      throw new CompatHarnessError("scenario request method must be a non-empty token");
    }
    const bodyBytes = resolveCompatRequestBody(scenario, method);
    const redirectMode = scenario.http.followRedirects === true ? "follow" : "manual";
    const fetchImpl = options.fetchImpl ?? globalThis.fetch;
    if (typeof fetchImpl !== "function") {
      throw new CompatHarnessError("executor fetch implementation is unavailable");
    }

    const fetchStartedMs = Date.now();
    let response: Response;
    try {
      response = await fetchImpl(urlText, {
        method,
        headers,
        body: bodyBytes,
        redirect: redirectMode,
        signal: AbortSignal.timeout(timeoutMs),
      });
    } catch (error) {
      const wallMs = Date.now() - startedMs;
      const transport = classifyCompatTransportError(error);
      if (transport === "timeout") {
        return nonHttp(
          "timeout",
          "timeout",
          `no complete response within the scenario timeout (category '${category}', ${timeoutMs} ms)`,
          Buffer.alloc(0),
          wallMs,
          null
        );
      }
      if (transport === "connection-refused") {
        return nonHttp(
          "connection-refused",
          "connection-refused",
          `TCP connect failed against ${safeOrigin} (backend not listening)`,
          Buffer.alloc(0),
          wallMs,
          null
        );
      }
      if (transport === "connection-reset") {
        return nonHttp(
          "connection-reset",
          "connection-reset",
          "connection dropped before the response completed",
          Buffer.alloc(0),
          wallMs,
          null
        );
      }
      return nonHttp(
        "harness-error",
        "harness-error",
        `executor request failed (${error instanceof Error ? error.name : "unknown-error"})`,
        Buffer.alloc(0),
        wallMs,
        null
      );
    }
    const ttfbMs = Date.now() - fetchStartedMs;

    if (response === null || typeof response !== "object" || typeof response.status !== "number") {
      return nonHttp(
        "harness-error",
        "harness-error",
        "executor fetch returned an unusable response",
        Buffer.alloc(0),
        Date.now() - startedMs,
        ttfbMs
      );
    }

    const capturedHeaders = captureCompatResponseHeaders(response.headers);
    const location = response.headers.get("location");
    const refresh = response.headers.get("refresh");
    const redirect =
      location !== null || refresh !== null
        ? {
            ...(location !== null ? { location } : {}),
            ...(refresh !== null ? { refresh } : {}),
          }
        : null;

    // Bounded incremental body capture: never retain past maxBodyBytes and
    // never consult Content-Length (it is advisory, not proof of bytes).
    const chunks: Uint8Array[] = [];
    let retained = 0;
    let truncated = false;
    let readError: unknown = null;
    if (response.body !== null) {
      const reader = response.body.getReader();
      try {
        for (;;) {
          let result: ReadableStreamReadResult<Uint8Array>;
          try {
            result = await reader.read();
          } catch (error) {
            readError = error;
            break;
          }
          if (result.done) break;
          const value = result.value;
          const length = value?.byteLength ?? 0;
          if (length === 0) continue;
          if (retained + length > maxBodyBytes) {
            chunks.push(value.slice(0, maxBodyBytes - retained));
            retained = maxBodyBytes;
            truncated = true;
            try {
              await reader.cancel();
            } catch {
              // cancelling a saturated stream is best-effort cleanup.
            }
            break;
          }
          chunks.push(value.slice());
          retained += length;
        }
      } finally {
        try {
          reader.releaseLock();
        } catch {
          // releasing the lock is best-effort cleanup.
        }
      }
    }
    const retainedBytes = Buffer.concat(chunks.map((chunk) => Buffer.from(chunk)));
    const wallMs = Date.now() - startedMs;

    if (readError !== null) {
      const transport = classifyCompatTransportError(readError);
      if (transport === "timeout") {
        return nonHttp(
          "timeout",
          "timeout",
          `no complete response within the scenario timeout (category '${category}', ${timeoutMs} ms)`,
          retainedBytes,
          wallMs,
          ttfbMs,
          textPreviewBytes
        );
      }
      if (transport === "connection-reset") {
        return nonHttp(
          "connection-reset",
          "connection-reset",
          `connection dropped mid-response after ${retainedBytes.length} observed bytes`,
          retainedBytes,
          wallMs,
          ttfbMs,
          textPreviewBytes
        );
      }
      return nonHttp(
        "harness-error",
        "harness-error",
        `executor body capture failed (${readError instanceof Error ? readError.name : "unknown-error"})`,
        retainedBytes,
        wallMs,
        ttfbMs,
        textPreviewBytes
      );
    }

    const body: ObservationBody = truncated
      ? {
          bytes: maxBodyBytes,
          truncated: true,
          capName: COMPAT_BODY_CAP_NAME,
          ...previewOf(retainedBytes, textPreviewBytes),
        }
      : {
          bytes: retainedBytes.length,
          ...(retainedBytes.length > 0 ? { sha256: sha256Hex(retainedBytes) } : {}),
          truncated: false,
          ...previewOf(retainedBytes, textPreviewBytes),
        };

    // JSON capture: parse only fully-captured JSON bodies under the parse
    // cap. Malformed JSON stays an http-response with a recorded reason —
    // comparison of expected JSON belongs to a later stage.
    const contentType = response.headers.get("content-type");
    let json: unknown;
    let hasJson = false;
    let jsonReason: JsonParseReason | null = null;
    if (!isCompatJsonContentType(contentType)) {
      jsonReason = retainedBytes.length === 0 ? "empty-body" : "non-json-content-type";
    } else if (retainedBytes.length === 0) {
      jsonReason = "empty-body";
    } else if (truncated) {
      jsonReason = "truncated";
    } else if (retainedBytes.length > maxParseBytes) {
      jsonReason = "over-parse-cap";
    } else {
      try {
        json = JSON.parse(retainedBytes.toString("utf8")) as unknown;
        hasJson = true;
      } catch {
        jsonReason = "malformed-json";
      }
    }

    return createHttpResponseObservation({
      scenarioId,
      backend,
      status: response.status,
      headers: capturedHeaders,
      body,
      ...(hasJson ? { json } : {}),
      ...(!hasJson && jsonReason !== null
        ? { jsonParse: { skipped: true, reason: jsonReason } }
        : {}),
      redirect,
      timing: { wallMs, ttfbMs },
    });
  } catch (error) {
    const wallMs = Date.now() - startedMs;
    if (error instanceof CompatHarnessError) {
      return nonHttp("harness-error", "harness-error", error.detail, Buffer.alloc(0), wallMs, null);
    }
    return nonHttp(
      "harness-error",
      "harness-error",
      `executor failed (${error instanceof Error ? error.name : "unknown-error"})`,
      Buffer.alloc(0),
      wallMs,
      null
    );
  }
}

function baseUrlOf(options: ExecuteCompatScenarioOptions): string {
  if (typeof options?.baseUrl !== "string") {
    throw new CompatHarnessError("executor baseUrl must be an absolute http(s) URL");
  }
  return options.baseUrl;
}
