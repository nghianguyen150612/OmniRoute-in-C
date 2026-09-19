/**
 * Compatibility harness — Stage 5 (part 1): HTTP status + header comparator
 * (Task 010).
 *
 * Compares the HTTP status and contract-selected response headers of two
 * already-captured `CompatObservation` values according to one already-loaded
 * compatibility scenario, producing deterministic `CompatMismatch` records
 * for those two dimensions only.
 *
 * Stage boundary (see `docs/native-backend/COMPATIBILITY_HARNESS.md` §§3, 7,
 * 13): scenario + reference observation + native observation → status/header
 * mismatches. This module does NOT execute HTTP, start backends, orchestrate
 * dual-backend runs, compare JSON/bodies, normalize anything, generate full
 * `CompatResult` verdicts, render reports, or emit C code. It never imports
 * the executor or either adapter, and it constructs mismatch records only —
 * later orchestration assembles them into a `CompatResult` via the Task 007
 * factories.
 *
 * Contract reconstruction (Task 005 — the comparator detects drift, never
 * hides it):
 * - Status (§2.1 dimension 1, §13 verdict rule): when the scenario declares
 *   `compare.status`, BOTH observations must carry that exact status. Status
 *   values are semantic: no class grouping, no tolerance, no normalization,
 *   no `0` fallback. When `compare.status` is absent the status dimension
 *   yields nothing — undeclared dimensions yield nothing (§3 item 5) — even
 *   if the two statuses differ.
 * - Headers (§7.1): case-insensitive-name multisets. Names compare
 *   lowercased (correct even for validated serialized observations that
 *   bypassed the Task 007 factories); values are never lowercased, trimmed,
 *   or rewritten. A temporary grouped view is built per call; inputs are
 *   only read, so a lossy `Record<string, string>` conversion never happens:
 *   every captured occurrence and value is preserved in the grouping.
 * - Rules (§7.2, canonical names from the scenario schema): `exact`
 *   (multisets byte-equal, order-insignificant, duplicates significant),
 *   `presence` (≥1 value on both sides, values ignored),
 *   `absent` (no value on either side), `ignore` (named-header exclusion
 *   only — never global, never status, never evidence removal),
 *   `prefix:<s>` / `contains:<s>` (EVERY captured value on BOTH sides must
 *   satisfy the predicate; a missing header fails the predicate rather than
 *   passing vacuously). No regex, no fuzzy matching, no tolerance, no
 *   callbacks, no compare-all/ignore-all — the catalog is closed.
 * - Selection (§7, §13): ONLY scenario-declared header predicates are
 *   evaluated. Captured-but-unselected headers (e.g. runtime transport
 *   headers) never become mismatches.
 * - Non-HTTP outcomes (§5.1): status/header comparison requires
 *   `http-response` on both sides. Any other combination returns an explicit
 *   blocked comparison (`comparable: false`) — outcome comparison itself is
 *   the later general comparator's `outcome` dimension (the result schema
 *   already reserves it), so this module invents no status/header mismatches
 *   and never declares equality it cannot evaluate. A missing response is
 *   never status `0` and no headers are fabricated for it.
 * - Determinism: the single status mismatch (when present) comes first,
 *   header mismatches follow sorted by lowercased header name — independent
 *   of scenario key order and captured header order.
 * - Mismatch safety (§13 size discipline): `reference`/`native` summaries
 *   are bounded strings (300 chars, the existing body-head evidence bound);
 *   `presence`/`absent` summaries never carry values (`present`/`absent`
 *   only); `normalized` is always `false` because Task 010 applies no
 *   normalization; scenario request headers/bodies are never read. Response
 *   values are NOT redacted — the canonical mismatch contract expects
 *   concise values — so scenarios must prefer `presence`/`ignore` for
 *   dynamic or secret-bearing headers such as `set-cookie`.
 *
 * Fetch-fidelity note (Task 008 limitation, not comparator logic): undici
 * merges repeated response headers except `set-cookie`, so a runtime-combined
 * value compares as one combined string. The comparator sees only what was
 * captured.
 */

import type { LoadedObservation } from "./observation.ts";
import { deepFreezeValue } from "./schemaCore.ts";
import type { CompatMismatch } from "./result.ts";
import type { LoadedScenario } from "./scenarioLoader.ts";

// ---------------------------------------------------------------------------
// Public shapes
// ---------------------------------------------------------------------------

/** Closed header-rule catalog (§7.2). Unknown strings are rejected, never guessed. */
export type CompatHeaderRuleName = "exact" | "presence" | "absent" | "ignore";

export interface CompatParsedHeaderRule {
  name: CompatHeaderRuleName | "prefix" | "contains";
  /** Operand for `prefix:` / `contains:` (may be empty only if declared so). */
  operand: string;
  /** The declared rule string, echoed verbatim into mismatch `rule` fields. */
  declared: string;
}

export type StatusHeadersBlockedReason = "non-http-outcome" | "unknown-header-rule";

export interface StatusHeadersComparable {
  comparable: true;
  mismatches: readonly CompatMismatch[];
}

export interface StatusHeadersBlocked {
  comparable: false;
  reason: StatusHeadersBlockedReason;
  referenceOutcome: string;
  nativeOutcome: string;
  /** Set only for `unknown-header-rule`: the offending declared header. */
  header?: string;
  /** Set only for `unknown-header-rule`: the offending declared rule string. */
  rule?: string;
}

export type StatusHeadersComparison = StatusHeadersComparable | StatusHeadersBlocked;

/** Bound for mismatch `reference`/`native` summaries (§13 size discipline). */
export const COMPAT_MISMATCH_SUMMARY_CHARS = 300;

// ---------------------------------------------------------------------------
// Pure helpers (exported so tests pin them without observations)
// ---------------------------------------------------------------------------

/**
 * Parse one declared header-rule string into the closed catalog. Returns
 * null for anything outside the catalog — the caller reports it as blocked
 * instead of guessing.
 */
export function parseCompatHeaderRule(declared: unknown): CompatParsedHeaderRule | null {
  if (typeof declared !== "string" || declared.length === 0) return null;
  if (
    declared === "exact" ||
    declared === "presence" ||
    declared === "absent" ||
    declared === "ignore"
  ) {
    return { name: declared, operand: "", declared };
  }
  if (declared.startsWith("prefix:")) {
    return { name: "prefix", operand: declared.slice("prefix:".length), declared };
  }
  if (declared.startsWith("contains:")) {
    return { name: "contains", operand: declared.slice("contains:".length), declared };
  }
  return null;
}

/**
 * Group captured headers by lowercased name, preserving every occurrence and
 * value in received order. Inputs are only read.
 */
export function groupCompatHeadersByName(
  headers: readonly Readonly<{ name: string; value: string }>[]
): Map<string, string[]> {
  const grouped = new Map<string, string[]>();
  for (const entry of headers) {
    const name = entry.name.toLowerCase();
    const values = grouped.get(name) ?? [];
    values.push(entry.value);
    grouped.set(name, values);
  }
  return grouped;
}

/** Multiset equality: same length and same values after sorting copies. */
export function compatValueMultisetsEqual(
  first: readonly string[],
  second: readonly string[]
): boolean {
  if (first.length !== second.length) return false;
  const a = [...first].sort();
  const b = [...second].sort();
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) return false;
  }
  return true;
}

/** Bounded summary of one side's captured values; missing sides say `absent`. */
export function summarizeCompatValues(values: readonly string[] | null): string {
  if (values === null) return "absent";
  const encoded = JSON.stringify(values) ?? "[]";
  if (encoded.length <= COMPAT_MISMATCH_SUMMARY_CHARS) return encoded;
  return `${encoded.slice(0, COMPAT_MISMATCH_SUMMARY_CHARS - 3)}...`;
}

// ---------------------------------------------------------------------------
// Comparator
// ---------------------------------------------------------------------------

function statusOf(observation: LoadedObservation): number | null {
  return observation.outcome === "http-response" ? observation.status : null;
}

function statusMismatch(
  expected: number,
  referenceStatus: number,
  nativeStatus: number
): CompatMismatch | null {
  if (referenceStatus === expected && nativeStatus === expected) return null;
  return {
    dimension: "status",
    path: "status",
    rule: "exact",
    reference: String(referenceStatus),
    native: String(nativeStatus),
    normalized: false,
  };
}

function headerMismatch(
  lowerName: string,
  rule: CompatParsedHeaderRule,
  referenceValues: string[] | null,
  nativeValues: string[] | null
): CompatMismatch | null {
  const path = `headers.${lowerName}`;
  switch (rule.name) {
    case "ignore":
      return null;
    case "presence": {
      const referencePresent = referenceValues !== null && referenceValues.length > 0;
      const nativePresent = nativeValues !== null && nativeValues.length > 0;
      if (referencePresent && nativePresent) return null;
      return {
        dimension: "headers",
        path,
        rule: rule.declared,
        reference: referencePresent ? "present" : "absent",
        native: nativePresent ? "present" : "absent",
        normalized: false,
      };
    }
    case "absent": {
      const referencePresent = referenceValues !== null && referenceValues.length > 0;
      const nativePresent = nativeValues !== null && nativeValues.length > 0;
      if (!referencePresent && !nativePresent) return null;
      return {
        dimension: "headers",
        path,
        rule: rule.declared,
        reference: referencePresent ? "present" : "absent",
        native: nativePresent ? "present" : "absent",
        normalized: false,
      };
    }
    case "exact": {
      if (
        referenceValues !== null &&
        nativeValues !== null &&
        compatValueMultisetsEqual(referenceValues, nativeValues)
      ) {
        return null;
      }
      return {
        dimension: "headers",
        path,
        rule: rule.declared,
        reference: summarizeCompatValues(referenceValues),
        native: summarizeCompatValues(nativeValues),
        normalized: false,
      };
    }
    case "prefix":
    case "contains": {
      const holds = (values: string[] | null): boolean => {
        if (values === null || values.length === 0) return false;
        return rule.name === "prefix"
          ? values.every((value) => value.startsWith(rule.operand))
          : values.every((value) => value.includes(rule.operand));
      };
      if (holds(referenceValues) && holds(nativeValues)) return null;
      return {
        dimension: "headers",
        path,
        rule: rule.declared,
        reference: summarizeCompatValues(referenceValues),
        native: summarizeCompatValues(nativeValues),
        normalized: false,
      };
    }
  }
}

/**
 * Compare HTTP status and scenario-selected response headers of two
 * already-captured observations. Pure: reads its inputs, mutates nothing,
 * performs no I/O. The returned mismatches (when comparable) are frozen;
 * assemble them into a `CompatResult` in a later orchestration stage.
 */
export function compareStatusAndHeaders(
  scenario: LoadedScenario,
  reference: LoadedObservation,
  native: LoadedObservation
): StatusHeadersComparison {
  const referenceStatus = statusOf(reference);
  const nativeStatus = statusOf(native);
  if (referenceStatus === null || nativeStatus === null) {
    return {
      comparable: false,
      reason: "non-http-outcome",
      referenceOutcome: reference.outcome,
      nativeOutcome: native.outcome,
    };
  }

  const compare = (scenario as { compare?: unknown }).compare;
  const compareRecord =
    compare !== null && typeof compare === "object" ? (compare as Record<string, unknown>) : {};

  const mismatches: CompatMismatch[] = [];
  if (typeof compareRecord["status"] === "number" && Number.isInteger(compareRecord["status"])) {
    const status = statusMismatch(compareRecord["status"] as number, referenceStatus, nativeStatus);
    if (status !== null) mismatches.push(status);
  }

  const headersRecord =
    compareRecord["headers"] !== null && typeof compareRecord["headers"] === "object"
      ? (compareRecord["headers"] as Record<string, unknown>)
      : {};
  // Deterministic selection order: sort by lowercased name, then declared
  // key. A pathological same-name-different-case collision resolves
  // first-wins in that order (documented; unreachable from Task 006 loader
  // output in practice since schemas accept any key casing).
  const selected = Object.entries(headersRecord)
    .map(([name, declared]) => ({ lowerName: name.toLowerCase(), declared }))
    .sort((a, b) => (a.lowerName < b.lowerName ? -1 : a.lowerName > b.lowerName ? 1 : 0));
  const seen = new Set<string>();
  const referenceGrouped = groupCompatHeadersByName(reference.headers);
  const nativeGrouped = groupCompatHeadersByName(native.headers);
  for (const { lowerName, declared } of selected) {
    if (seen.has(lowerName)) continue;
    seen.add(lowerName);
    const rule = parseCompatHeaderRule(declared);
    if (rule === null) {
      return {
        comparable: false,
        reason: "unknown-header-rule",
        referenceOutcome: reference.outcome,
        nativeOutcome: native.outcome,
        header: lowerName,
        rule: typeof declared === "string" ? declared : undefined,
      };
    }
    const mismatch = headerMismatch(
      lowerName,
      rule,
      referenceGrouped.get(lowerName) ?? null,
      nativeGrouped.get(lowerName) ?? null
    );
    if (mismatch !== null) mismatches.push(mismatch);
  }

  deepFreezeValue(mismatches);
  return { comparable: true, mismatches };
}
