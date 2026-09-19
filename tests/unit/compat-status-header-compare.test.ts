/**
 * Stage 5 (part 1) tests: HTTP status + header comparator (Task 010).
 *
 * Pure and process-free: every case uses in-memory scenarios and
 * factory-created (or hand-rolled, for factory-independence) observations.
 * No backend, no listener, no loopback, no child process, no network. The
 * only filesystem reads are the canonical-schema loads inside the Task 007
 * validators — the same precedent Tasks 007/008 tests already establish.
 */

import test from "node:test";
import assert from "node:assert/strict";

import {
  compareStatusAndHeaders,
  compatValueMultisetsEqual,
  groupCompatHeadersByName,
  parseCompatHeaderRule,
  summarizeCompatValues,
  type StatusHeadersComparison,
} from "../../native/compat/statusHeaderCompare.ts";
import {
  createHttpResponseObservation,
  createNonHttpObservation,
  type LoadedObservation,
  type NonHttpOutcome,
} from "../../native/compat/observation.ts";
import { createFailResult, createPassResult, validateResult } from "../../native/compat/result.ts";
import type { LoadedScenario } from "../../native/compat/scenarioLoader.ts";

// ---------------------------------------------------------------------------
// Builders (frozen like Task 006/007 output)
// ---------------------------------------------------------------------------

function freezeDeep(value: unknown): void {
  if (value === null || typeof value !== "object" || Object.isFrozen(value)) return;
  for (const child of Object.values(value)) freezeDeep(child);
  Object.freeze(value);
}

function makeScenario(compare: Record<string, unknown>): LoadedScenario {
  const scenario = {
    schema: "compat-scenario/v1",
    id: "status-header-probe",
    description: "comparator probe scenario",
    http: { method: "GET", path: "/probe" },
    compare,
  };
  freezeDeep(scenario);
  return scenario as unknown as LoadedScenario;
}

function makeHttp(
  status: number,
  headers: Array<{ name: string; value: string }> = []
): LoadedObservation {
  return createHttpResponseObservation({
    scenarioId: "status-header-probe",
    backend: { id: "reference" },
    status,
    headers,
    body: { bytes: 0, truncated: false },
  });
}

/** Hand-rolled observation bypassing Task 007 factories (factory-independence). */
function makeRawHttp(
  status: number,
  headers: Array<{ name: string; value: string }>
): LoadedObservation {
  const observation = {
    schema: "compat-observation/v1",
    scenarioId: "status-header-probe",
    backend: { id: "reference" },
    outcome: "http-response",
    status,
    headers: headers.map((entry) => ({ ...entry })),
    body: { bytes: 0, truncated: false },
    redirect: null,
    stream: null,
    failure: null,
  };
  freezeDeep(observation);
  return observation as unknown as LoadedObservation;
}

function makeNonHttp(outcome: NonHttpOutcome): LoadedObservation {
  return createNonHttpObservation(outcome, {
    scenarioId: "status-header-probe",
    backend: { id: "native" },
    headers: [],
    body: { bytes: 0, truncated: false },
    failure: { kind: outcome, detail: "probe failure" },
  });
}

function mustBeComparable(
  comparison: StatusHeadersComparison
): asserts comparison is Extract<StatusHeadersComparison, { comparable: true }> {
  assert.equal(comparison.comparable, true);
}

function comparableMismatches(
  scenario: LoadedScenario,
  reference: LoadedObservation,
  native: LoadedObservation
) {
  const comparison = compareStatusAndHeaders(scenario, reference, native);
  mustBeComparable(comparison);
  return comparison.mismatches;
}

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

test("rule parsing accepts the closed catalog and rejects the rest", () => {
  assert.deepEqual(parseCompatHeaderRule("exact"), {
    name: "exact",
    operand: "",
    declared: "exact",
  });
  assert.equal(parseCompatHeaderRule("prefix:text/html")?.operand, "text/html");
  assert.equal(parseCompatHeaderRule("contains:foo")?.operand, "foo");
  assert.equal(parseCompatHeaderRule("regex:^a"), null);
  assert.equal(parseCompatHeaderRule("fuzzy"), null);
  assert.equal(parseCompatHeaderRule("EXACT"), null);
  assert.equal(parseCompatHeaderRule(""), null);
  assert.equal(parseCompatHeaderRule(42), null);
});

test("header grouping preserves every occurrence without mutation", () => {
  const input = [
    { name: "X-Multi", value: "b" },
    { name: "x-multi", value: "a" },
    { name: "Content-Type", value: "application/json" },
  ];
  const before = JSON.stringify(input);
  const grouped = groupCompatHeadersByName(input);
  assert.deepEqual(grouped.get("x-multi"), ["b", "a"]);
  assert.deepEqual(grouped.get("content-type"), ["application/json"]);
  assert.equal(JSON.stringify(input), before);
});

test("multiset equality is order-insensitive but multiplicity-sensitive", () => {
  assert.equal(compatValueMultisetsEqual(["a", "b"], ["b", "a"]), true);
  assert.equal(compatValueMultisetsEqual(["a", "a"], ["a"]), false);
  assert.equal(compatValueMultisetsEqual(["a"], ["b"]), false);
  assert.equal(compatValueMultisetsEqual([], []), true);
});

test("summaries are bounded and missing sides say absent", () => {
  assert.equal(summarizeCompatValues(null), "absent");
  assert.equal(summarizeCompatValues(["a"]), '["a"]');
  const long = summarizeCompatValues(["x".repeat(500)]);
  assert.ok(long.length <= 300, `bounded summary (got ${long.length})`);
  assert.ok(long.endsWith("..."));
});

// ---------------------------------------------------------------------------
// Status (§19 items 1-4 + declared-expectation semantics)
// ---------------------------------------------------------------------------

test("1. equal status produces no status mismatch", () => {
  const scenario = makeScenario({ status: 200 });
  assert.deepEqual(comparableMismatches(scenario, makeHttp(200), makeHttp(200)), []);
});

test("2. different status produces one exact status mismatch", () => {
  const scenario = makeScenario({ status: 200 });
  assert.deepEqual(comparableMismatches(scenario, makeHttp(200), makeHttp(404)), [
    {
      dimension: "status",
      path: "status",
      rule: "exact",
      reference: "200",
      native: "404",
      normalized: false,
    },
  ]);
});

test("3. 200 vs 201 remains a mismatch (no class grouping)", () => {
  const scenario = makeScenario({ status: 200 });
  const mismatches = comparableMismatches(scenario, makeHttp(200), makeHttp(201));
  assert.equal(mismatches.length, 1);
  assert.equal(mismatches[0]?.path, "status");
});

test("4. non-HTTP outcomes never fall back to status 0", () => {
  for (const outcome of [
    "timeout",
    "connection-refused",
    "connection-reset",
    "harness-error",
  ] as const) {
    const comparison = compareStatusAndHeaders(
      makeScenario({ status: 200 }),
      makeNonHttp(outcome),
      makeNonHttp(outcome)
    );
    assert.equal(comparison.comparable, false);
    assert.ok(!JSON.stringify(comparison).includes('"status"'), `no status key for ${outcome}`);
    assert.ok(!JSON.stringify(comparison).includes(':"0"'), `no zero status for ${outcome}`);
  }
});

test("both sides off-expectation still mismatch (expectation enforced, not just pairwise)", () => {
  const scenario = makeScenario({ status: 200 });
  assert.deepEqual(comparableMismatches(scenario, makeHttp(500), makeHttp(500)), [
    {
      dimension: "status",
      path: "status",
      rule: "exact",
      reference: "500",
      native: "500",
      normalized: false,
    },
  ]);
});

test("undeclared status yields no status dimension even when statuses differ", () => {
  const scenario = makeScenario({ headers: { "content-type": "presence" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, [{ name: "content-type", value: "application/json" }]),
    makeHttp(500, [{ name: "content-type", value: "application/json" }])
  );
  assert.deepEqual(mismatches, []);
});

// ---------------------------------------------------------------------------
// Exact (§19 items 5-10, 20-22)
// ---------------------------------------------------------------------------

test("5. equal exact header passes", () => {
  const scenario = makeScenario({ headers: { "content-type": "exact" } });
  const headers = [{ name: "content-type", value: "application/json" }];
  assert.deepEqual(
    comparableMismatches(scenario, makeHttp(200, headers), makeHttp(200, headers)),
    []
  );
});

test("6. exact header mismatch carries bounded value summaries", () => {
  const scenario = makeScenario({ headers: { "content-type": "exact" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, [{ name: "content-type", value: "application/json" }]),
    makeHttp(200, [{ name: "content-type", value: "text/html; charset=utf-8" }])
  );
  assert.deepEqual(mismatches, [
    {
      dimension: "headers",
      path: "headers.content-type",
      rule: "exact",
      reference: '["application/json"]',
      native: '["text/html; charset=utf-8"]',
      normalized: false,
    },
  ]);
});

test("7. header names match case-insensitively without factory help", () => {
  const scenario = makeScenario({ headers: { "Content-Type": "exact" } });
  const mismatches = comparableMismatches(
    scenario,
    makeRawHttp(200, [{ name: "CONTENT-TYPE", value: "application/json" }]),
    makeRawHttp(200, [{ name: "content-type", value: "application/json" }])
  );
  assert.deepEqual(mismatches, []);
});

test("8. header values stay case-sensitive under exact", () => {
  const scenario = makeScenario({ headers: { "content-type": "exact" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, [{ name: "content-type", value: "Application/JSON" }]),
    makeHttp(200, [{ name: "content-type", value: "application/json" }])
  );
  assert.equal(mismatches.length, 1);
});

test("9. missing exact header mismatches with an absent summary", () => {
  const scenario = makeScenario({ headers: { "x-request-id": "exact" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, [{ name: "x-request-id", value: "abc" }]),
    makeHttp(200, [])
  );
  assert.deepEqual(mismatches, [
    {
      dimension: "headers",
      path: "headers.x-request-id",
      rule: "exact",
      reference: '["abc"]',
      native: "absent",
      normalized: false,
    },
  ]);
});

test("10. present-empty value differs from missing", () => {
  const scenario = makeScenario({ headers: { "x-empty": "exact" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, [{ name: "x-empty", value: "" }]),
    makeHttp(200, [])
  );
  assert.equal(mismatches.length, 1);
  assert.equal(mismatches[0]?.reference, '[""]');
  assert.equal(mismatches[0]?.native, "absent");
});

test("20. repeated headers compare as multisets (order-insignificant)", () => {
  const scenario = makeScenario({ headers: { "x-multi": "exact" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, [
      { name: "x-multi", value: "1" },
      { name: "x-multi", value: "2" },
    ]),
    makeHttp(200, [
      { name: "x-multi", value: "2" },
      { name: "x-multi", value: "1" },
    ])
  );
  assert.deepEqual(mismatches, []);
});

test("21. repeated headers with differing multiplicity mismatch", () => {
  const scenario = makeScenario({ headers: { "x-multi": "exact" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, [
      { name: "x-multi", value: "1" },
      { name: "x-multi", value: "1" },
    ]),
    makeHttp(200, [{ name: "x-multi", value: "1" }])
  );
  assert.equal(mismatches.length, 1);
  assert.equal(mismatches[0]?.reference, '["1","1"]');
});

test("22. repeated headers with a differing value mismatch", () => {
  const scenario = makeScenario({ headers: { "x-multi": "exact" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, [
      { name: "x-multi", value: "1" },
      { name: "x-multi", value: "2" },
    ]),
    makeHttp(200, [
      { name: "x-multi", value: "1" },
      { name: "x-multi", value: "3" },
    ])
  );
  assert.equal(mismatches.length, 1);
});

// ---------------------------------------------------------------------------
// Presence / absence (§19 items 11-14)
// ---------------------------------------------------------------------------

test("11. presence succeeds when both sides carry the header", () => {
  const scenario = makeScenario({ headers: { "x-request-id": "presence" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, [{ name: "x-request-id", value: "aaa" }]),
    makeHttp(200, [{ name: "x-request-id", value: "zzz" }])
  );
  assert.deepEqual(mismatches, [], "values are ignored under presence");
});

test("12. presence fails when either side misses the header", () => {
  const scenario = makeScenario({ headers: { "x-request-id": "presence" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, [{ name: "x-request-id", value: "aaa" }]),
    makeHttp(200, [])
  );
  assert.deepEqual(mismatches, [
    {
      dimension: "headers",
      path: "headers.x-request-id",
      rule: "presence",
      reference: "present",
      native: "absent",
      normalized: false,
    },
  ]);
});

test("13. absence succeeds when neither side carries the header", () => {
  const scenario = makeScenario({ headers: { "content-length": "absent" } });
  assert.deepEqual(comparableMismatches(scenario, makeHttp(204), makeHttp(204)), []);
});

test("14. absence fails when either side carries the header", () => {
  const scenario = makeScenario({ headers: { "content-length": "absent" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, []),
    makeHttp(200, [{ name: "content-length", value: "12" }])
  );
  assert.deepEqual(mismatches, [
    {
      dimension: "headers",
      path: "headers.content-length",
      rule: "absent",
      reference: "absent",
      native: "present",
      normalized: false,
    },
  ]);
});

// ---------------------------------------------------------------------------
// Prefix / contains (§19 items 15-18)
// ---------------------------------------------------------------------------

test("15. prefix succeeds when every value on both sides matches", () => {
  const scenario = makeScenario({ headers: { "content-type": "prefix:text/html" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(404, [{ name: "content-type", value: "text/html; charset=utf-8" }]),
    makeHttp(404, [{ name: "content-type", value: "text/html" }])
  );
  assert.deepEqual(mismatches, []);
});

test("16. prefix fails when any value misses (or the header is missing)", () => {
  const scenario = makeScenario({ headers: { "content-type": "prefix:text/html" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(404, [{ name: "content-type", value: "text/html" }]),
    makeHttp(404, [{ name: "content-type", value: "application/json" }])
  );
  assert.equal(mismatches.length, 1);
  assert.equal(mismatches[0]?.rule, "prefix:text/html");
  assert.deepEqual(comparableMismatches(scenario, makeHttp(404), makeHttp(404)).length, 1);
});

test("17. contains succeeds when every value on both sides matches", () => {
  const scenario = makeScenario({ headers: { "x-fingerprint": "contains:build-" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, [{ name: "x-fingerprint", value: "omni-build-1" }]),
    makeHttp(200, [{ name: "x-fingerprint", value: "prefix-build-2-suffix" }])
  );
  assert.deepEqual(mismatches, []);
});

test("18. contains fails when any value misses", () => {
  const scenario = makeScenario({ headers: { "x-fingerprint": "contains:build-" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, [{ name: "x-fingerprint", value: "omni-build-1" }]),
    makeHttp(200, [{ name: "x-fingerprint", value: "release-9" }])
  );
  assert.equal(mismatches.length, 1);
  assert.equal(mismatches[0]?.native, '["release-9"]');
});

// ---------------------------------------------------------------------------
// Ignore / selection / ordering (§19 items 19, 23-25)
// ---------------------------------------------------------------------------

test("19. ignore suppresses comparison for the named header only", () => {
  const scenario = makeScenario({ headers: { date: "ignore", "content-type": "exact" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, [
      { name: "date", value: "Mon, 01 Jan 2024 00:00:00 GMT" },
      { name: "content-type", value: "application/json" },
    ]),
    makeHttp(200, [{ name: "content-type", value: "application/json" }])
  );
  assert.deepEqual(mismatches, [], "divergent ignored date is not a mismatch");
});

test("23. unselected captured headers never become mismatches", () => {
  const scenario = makeScenario({ headers: { "content-type": "exact" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, [
      { name: "content-type", value: "application/json" },
      { name: "x-transport-noise", value: "a" },
    ]),
    makeHttp(200, [
      { name: "content-type", value: "application/json" },
      { name: "x-transport-noise", value: "b" },
      { name: "x-extra", value: "c" },
    ])
  );
  assert.deepEqual(mismatches, []);
});

test("24. multiple selected predicates evaluate independently", () => {
  const scenario = makeScenario({
    status: 200,
    headers: { "content-type": "exact", "x-request-id": "presence" },
  });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, [
      { name: "content-type", value: "application/json" },
      { name: "x-request-id", value: "1" },
    ]),
    makeHttp(200, [
      { name: "content-type", value: "text/plain" },
      { name: "x-request-id", value: "2" },
    ])
  );
  assert.equal(mismatches.length, 1);
  assert.equal(mismatches[0]?.path, "headers.content-type");
});

test("25. mismatch ordering is deterministic (status first, then header name)", () => {
  const scenario = makeScenario({
    status: 200,
    headers: {
      "x-zulu": "presence",
      "content-type": "exact",
      "a-first": "absent",
    },
  });
  const first = comparableMismatches(
    scenario,
    makeHttp(500, [{ name: "a-first", value: "1" }]),
    makeHttp(404, [{ name: "content-type", value: "x" }])
  );
  const runAgain = comparableMismatches(
    scenario,
    makeHttp(500, [{ name: "a-first", value: "1" }]),
    makeHttp(404, [{ name: "content-type", value: "x" }])
  );
  assert.deepEqual(
    first.map((mismatch) => mismatch.path),
    ["status", "headers.a-first", "headers.content-type", "headers.x-zulu"]
  );
  assert.deepEqual(runAgain, first, "identical inputs produce identical output");
  assert.ok(Object.isFrozen(first), "mismatch collections are frozen");
});

// ---------------------------------------------------------------------------
// Mutation safety (§19 items 26-27)
// ---------------------------------------------------------------------------

test("26. frozen observations remain unchanged by comparison", () => {
  const scenario = makeScenario({ status: 200, headers: { "content-type": "exact" } });
  const reference = makeHttp(200, [{ name: "content-type", value: "application/json" }]);
  const native = makeHttp(404, [{ name: "content-type", value: "text/plain" }]);
  const beforeReference = JSON.stringify(reference);
  const beforeNative = JSON.stringify(native);
  comparableMismatches(scenario, reference, native);
  assert.equal(JSON.stringify(reference), beforeReference);
  assert.equal(JSON.stringify(native), beforeNative);
  assert.ok(Object.isFrozen(reference));
  assert.ok(Object.isFrozen(native));
});

test("27. the loaded scenario remains unchanged by comparison", () => {
  const scenario = makeScenario({ status: 200, headers: { "content-type": "exact" } });
  const before = JSON.stringify(scenario);
  comparableMismatches(
    scenario,
    makeHttp(200, [{ name: "content-type", value: "a" }]),
    makeHttp(200, [{ name: "content-type", value: "b" }])
  );
  assert.equal(JSON.stringify(scenario), before);
  assert.ok(Object.isFrozen(scenario));
});

// ---------------------------------------------------------------------------
// Non-HTTP outcomes (§19 items 28-30)
// ---------------------------------------------------------------------------

test("28. http-response vs non-HTTP is explicitly blocked, not equal", () => {
  const comparison = compareStatusAndHeaders(
    makeScenario({ status: 200, headers: { "content-type": "exact" } }),
    makeHttp(200, [{ name: "content-type", value: "application/json" }]),
    makeNonHttp("timeout")
  );
  assert.deepEqual(comparison, {
    comparable: false,
    reason: "non-http-outcome",
    referenceOutcome: "http-response",
    nativeOutcome: "timeout",
  });
});

test("29. non-HTTP vs http-response is explicitly blocked", () => {
  const comparison = compareStatusAndHeaders(
    makeScenario({ status: 200 }),
    makeNonHttp("connection-refused"),
    makeHttp(200)
  );
  assert.deepEqual(comparison, {
    comparable: false,
    reason: "non-http-outcome",
    referenceOutcome: "connection-refused",
    nativeOutcome: "http-response",
  });
});

test("30. non-HTTP vs non-HTTP is blocked even for identical outcomes", () => {
  const comparison = compareStatusAndHeaders(
    makeScenario({ status: 200 }),
    makeNonHttp("connection-reset"),
    makeNonHttp("connection-reset")
  );
  assert.deepEqual(comparison, {
    comparable: false,
    reason: "non-http-outcome",
    referenceOutcome: "connection-reset",
    nativeOutcome: "connection-reset",
  });
});

test("unknown header rules block instead of guessing", () => {
  const comparison = compareStatusAndHeaders(
    makeScenario({ headers: { "content-type": "regex:^a" } }),
    makeHttp(200),
    makeHttp(200)
  );
  assert.deepEqual(comparison, {
    comparable: false,
    reason: "unknown-header-rule",
    referenceOutcome: "http-response",
    nativeOutcome: "http-response",
    header: "content-type",
    rule: "regex:^a",
  });
});

// ---------------------------------------------------------------------------
// Models-contract representatives (§19 items 31-34, in-memory only)
// ---------------------------------------------------------------------------

test("31. HEAD predicate: content-type exact, content-length absent, connection exact", () => {
  const scenario = makeScenario({
    status: 200,
    headers: {
      "content-type": "exact",
      "content-length": "absent",
      connection: "exact",
    },
  });
  const headHeaders = [
    { name: "content-type", value: "application/json" },
    { name: "connection", value: "close" },
  ];
  assert.deepEqual(
    comparableMismatches(scenario, makeHttp(200, headHeaders), makeHttp(200, headHeaders)),
    []
  );
  const drifted = comparableMismatches(
    scenario,
    makeHttp(200, headHeaders),
    makeHttp(200, [...headHeaders, { name: "content-length", value: "18" }])
  );
  assert.equal(drifted.length, 1);
  assert.equal(drifted[0]?.path, "headers.content-length");
});

test("32. OPTIONS predicate: absent framing plus exact preflight headers", () => {
  const scenario = makeScenario({
    status: 204,
    headers: {
      "content-type": "absent",
      "content-length": "absent",
      allow: "absent",
      "access-control-allow-methods": "exact",
      vary: "exact",
    },
  });
  const optionsHeaders = [
    { name: "access-control-allow-methods", value: "GET, POST, PUT, DELETE, PATCH, OPTIONS" },
    { name: "vary", value: "Origin" },
  ];
  assert.deepEqual(
    comparableMismatches(scenario, makeHttp(204, optionsHeaders), makeHttp(204, optionsHeaders)),
    []
  );
  const drifted = comparableMismatches(
    scenario,
    makeHttp(204, optionsHeaders),
    makeHttp(204, [{ name: "vary", value: "Origin, Accept-Encoding" }])
  );
  assert.ok(drifted.some((mismatch) => mismatch.path === "headers.vary"));
});

test("33. 308 predicate: status plus query-preserving Location", () => {
  const scenario = makeScenario({
    status: 308,
    headers: { location: "exact", refresh: "exact" },
  });
  const redirectHeaders = [
    { name: "location", value: "/v1/models?limit=1&x=probe" },
    { name: "refresh", value: "0;url=/v1/models?limit=1&x=probe" },
  ];
  assert.deepEqual(
    comparableMismatches(scenario, makeHttp(308, redirectHeaders), makeHttp(308, redirectHeaders)),
    []
  );
  const stripped = comparableMismatches(
    scenario,
    makeHttp(308, redirectHeaders),
    makeHttp(308, [
      { name: "location", value: "/v1/models" },
      { name: "refresh", value: "0;url=/v1/models?limit=1&x=probe" },
    ])
  );
  assert.deepEqual(
    stripped.map((mismatch) => mismatch.path),
    ["headers.location"]
  );
});

test("34. missing-header predicate: route class absent on raw 308", () => {
  const scenario = makeScenario({
    status: 308,
    headers: { "x-omniroute-route-class": "absent", location: "exact" },
  });
  const bare = [{ name: "location", value: "/models?x=1" }];
  assert.deepEqual(comparableMismatches(scenario, makeHttp(308, bare), makeHttp(308, bare)), []);
  const drifted = comparableMismatches(
    scenario,
    makeHttp(308, bare),
    makeHttp(308, [...bare, { name: "x-omniroute-route-class", value: "CLIENT_API" }])
  );
  assert.equal(drifted.length, 1);
  assert.equal(drifted[0]?.path, "headers.x-omniroute-route-class");
});

// ---------------------------------------------------------------------------
// Task 007 structural conformance (§19 item 35)
// ---------------------------------------------------------------------------

test("35. mismatches assemble into valid Task 007 pass/fail results", () => {
  const shared = {
    scenarioId: "status-header-probe",
    reference: { observation: "obs/status-header-probe/reference.json", outcome: "http-response" },
    native: { observation: "obs/status-header-probe/native.json", outcome: "http-response" },
    artifacts: {
      dir: "artifacts/status-header-probe",
      result: "artifacts/status-header-probe/result.json",
    },
  };
  const scenario = makeScenario({ status: 200, headers: { "content-type": "exact" } });
  const mismatches = comparableMismatches(
    scenario,
    makeHttp(200, [{ name: "content-type", value: "a" }]),
    makeHttp(500, [{ name: "content-type", value: "b" }])
  );
  assert.ok(mismatches.length >= 1);
  const fail = createFailResult({
    ...shared,
    mismatches: [mismatches[0]!, ...mismatches.slice(1)],
  });
  assert.equal(validateResult(JSON.parse(JSON.stringify(fail)) as unknown).ok, true);

  const clean = comparableMismatches(
    scenario,
    makeHttp(200, [{ name: "content-type", value: "a" }]),
    makeHttp(200, [{ name: "content-type", value: "a" }])
  );
  assert.deepEqual(clean, []);
  assert.equal(
    validateResult(JSON.parse(JSON.stringify(createPassResult(shared))) as unknown).ok,
    true
  );
});
