/**
 * Stage 2 tests: observation and result runtime model (Task 007).
 *
 * Pure in-memory tests only: no backend is started, no HTTP is sent, no
 * child processes or sockets are used, no network or provider credentials
 * are required. The only filesystem reads are the canonical checked-in
 * schemas (via the validators under test).
 */

import test from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";

import {
  createHttpResponseObservation,
  createNonHttpObservation,
  cloneObservation,
  validateObservation,
  OBSERVATION_SCHEMA_ID,
  type LoadedObservation,
  type NonHttpOutcome,
  type ObservationValidation,
} from "../../native/compat/observation.ts";
import {
  createErrorResult,
  createFailResult,
  createPassResult,
  cloneResult,
  validateResult,
  RESULT_SCHEMA_ID,
  type CompatMismatch,
  type LoadedResult,
  type ResultValidation,
} from "../../native/compat/result.ts";

function failedObservation(validation: ObservationValidation) {
  assert.equal(validation.ok, false);
  if (validation.ok) return [];
  return [...validation.diagnostics];
}

function failedResult(validation: ResultValidation) {
  assert.equal(validation.ok, false);
  if (validation.ok) return [];
  return [...validation.diagnostics];
}

function httpObservation(): LoadedObservation {
  return createHttpResponseObservation({
    scenarioId: "models-get-authenticated",
    backend: { id: "reference", revision: "test-rev", mode: "dev" },
    status: 200,
    headers: [
      { name: "Content-Type", value: "application/json" },
      { name: "X-Request-Id", value: "req-1" },
    ],
    body: { bytes: 18, sha256: "abc", truncated: false },
    json: { object: "list", data: [] },
  });
}

function passResult(): LoadedResult {
  return createPassResult({
    scenarioId: "models-head",
    reference: { observation: "obs/models-head/reference.json", outcome: "http-response" },
    native: { observation: "obs/models-head/native.json", outcome: "http-response" },
    artifacts: { dir: "artifacts/models-head", result: "artifacts/models-head/result.json" },
  });
}

function mismatch(): CompatMismatch {
  return {
    dimension: "headers",
    path: "headers.content-length",
    rule: "absent",
    reference: "absent",
    native: '"12"',
    normalized: false,
  };
}

function roundTrip<T>(value: T): unknown {
  return JSON.parse(JSON.stringify(value)) as unknown;
}

// Schema identity ------------------------------------------------------------

test("observation and result schema ids match the canonical contracts", () => {
  assert.equal(OBSERVATION_SCHEMA_ID, "compat-observation/v1");
  assert.equal(RESULT_SCHEMA_ID, "compat-result/v1");
});

// Round-trip coverage ----------------------------------------------------------

test("http-response observation survives JSON round-trip and revalidation", () => {
  const original = httpObservation();
  assert.equal(validateObservation(original).ok, true);
  const revived = validateObservation(roundTrip(original));
  assert.equal(revived.ok, true);
  if (!revived.ok) return;
  assert.deepEqual(revived.value, roundTrip(original));
  assert.deepEqual((revived.value as { json?: unknown }).json, { object: "list", data: [] });
});

test("non-HTTP failure observation (timeout with partial capture) round-trips", () => {
  const original = createNonHttpObservation("timeout", {
    scenarioId: "models-get-authenticated",
    backend: { id: "native" },
    body: { bytes: 512, truncated: true, capName: "capture.maxBodyBytes" },
    failure: { kind: "timeout", detail: "no complete response within the scenario timeout" },
    timing: { wallMs: 10000, ttfbMs: null },
  });
  assert.ok(!("status" in original), "a timeout observation must not carry a status key");
  const revived = validateObservation(roundTrip(original));
  assert.equal(revived.ok, true);
  if (!revived.ok) return;
  assert.deepEqual(revived.value, roundTrip(original));
});

test("all six outcomes stay distinct through validation", () => {
  const outcomes: NonHttpOutcome[] = [
    "timeout",
    "connection-refused",
    "connection-reset",
    "backend-exit",
    "harness-error",
  ];
  const seen = new Set<string>();
  for (const outcome of [...outcomes, "http-response"] as const) {
    const observation =
      outcome === "http-response"
        ? httpObservation()
        : createNonHttpObservation(outcome, {
            scenarioId: "s",
            backend: { id: "reference" },
          });
    const validated = validateObservation(roundTrip(observation));
    assert.equal(validated.ok, true, outcome);
    if (!validated.ok) continue;
    assert.equal(validated.value.outcome, outcome);
    seen.add(validated.value.outcome);
  }
  assert.equal(seen.size, 6);
});

test("passing result round-trips", () => {
  const revived = validateResult(roundTrip(passResult()));
  assert.equal(revived.ok, true);
  if (!revived.ok) return;
  assert.deepEqual(revived.value, roundTrip(passResult()));
});

test("failing result with a mismatch round-trips with evidence intact", () => {
  const original = createFailResult({
    scenarioId: "models-head",
    reference: { observation: "obs/models-head/reference.json", outcome: "http-response" },
    native: { observation: "obs/models-head/native.json", outcome: "http-response" },
    mismatches: [mismatch()],
    artifacts: { dir: "artifacts/models-head", result: "artifacts/models-head/result.json" },
  });
  const revived = validateResult(roundTrip(original));
  assert.equal(revived.ok, true);
  if (!revived.ok) return;
  assert.deepEqual(revived.value, roundTrip(original));
  assert.equal(revived.value.mismatches.length, 1);
  assert.equal(revived.value.mismatches[0].path, "headers.content-length");
});

test("error result round-trips", () => {
  const original = createErrorResult({
    scenarioId: "models-get-authenticated",
    reference: { observation: "obs/x/reference.json", outcome: "backend-exit" },
    native: { observation: "obs/x/native.json", outcome: "harness-error" },
    artifacts: { dir: "artifacts/x", result: "artifacts/x/result.json" },
  });
  assert.equal(original.verdict, "error");
  const revived = validateResult(roundTrip(original));
  assert.equal(revived.ok, true);
  if (!revived.ok) return;
  assert.equal(revived.value.verdict, "error");
});

// Header representation -----------------------------------------------------------

test("header names lowercase at construction; order and repeats preserved; values untouched", () => {
  const observation = createHttpResponseObservation({
    scenarioId: "s",
    backend: { id: "reference" },
    status: 200,
    headers: [
      { name: "X-Multi", value: "b" },
      { name: "Content-Type", value: "Application/JSON" },
      { name: "x-multi", value: "a" },
    ],
  });
  assert.deepEqual(observation.headers, [
    { name: "x-multi", value: "b" },
    { name: "content-type", value: "Application/JSON" },
    { name: "x-multi", value: "a" },
  ]);
  assert.equal(validateObservation(roundTrip(observation)).ok, true);
});

test("empty header list is a valid representation of no captured headers", () => {
  const observation = createNonHttpObservation("connection-refused", {
    scenarioId: "s",
    backend: { id: "native" },
  });
  assert.deepEqual(observation.headers, []);
  assert.equal(validateObservation(roundTrip(observation)).ok, true);
});

test("malformed headers are rejected", () => {
  const missingName = {
    schema: "compat-observation/v1",
    scenarioId: "s",
    backend: { id: "reference" },
    outcome: "http-response",
    status: 200,
    headers: [{ value: "x" }],
    body: { bytes: 0, truncated: false },
  };
  assert.ok(
    failedObservation(validateObservation(missingName)).some(
      (d) => d.code === "schema-error" && (d.path ?? "").startsWith("headers")
    )
  );
  const numericValue = {
    schema: "compat-observation/v1",
    scenarioId: "s",
    backend: { id: "reference" },
    outcome: "http-response",
    status: 200,
    headers: [{ name: "x-a", value: 42 }],
    body: { bytes: 0, truncated: false },
  };
  assert.ok(
    failedObservation(validateObservation(numericValue)).some((d) => d.code === "schema-error")
  );
});

// Mutation safety --------------------------------------------------------------------

test("factory outputs are deep-frozen; clones are mutable working copies", () => {
  const observation = httpObservation();
  assert.ok(Object.isFrozen(observation));
  assert.ok(Object.isFrozen(observation.headers));
  assert.ok(Object.isFrozen(observation.body));
  assert.throws(() => {
    (observation as unknown as Record<string, unknown>)["status"] = 500;
  }, TypeError);
  assert.throws(() => {
    (observation.headers as unknown as unknown[]).push({ name: "x", value: "y" });
  }, TypeError);

  const clone = cloneObservation(observation);
  assert.ok(!Object.isFrozen(clone));
  (clone as { status: number }).status = 500;
  assert.equal(clone.status, 500);
  assert.equal(observation.status, 200);

  const result = passResult();
  assert.ok(Object.isFrozen(result));
  assert.throws(() => {
    (result as unknown as Record<string, unknown>)["verdict"] = "fail";
  }, TypeError);
  const resultClone = cloneResult(result);
  (resultClone as { scenarioId: string }).scenarioId = "edited";
  assert.equal(resultClone.scenarioId, "edited");
  assert.equal(result.scenarioId, "models-head");
});

test("validated external values are frozen as raw evidence", () => {
  const parsed = roundTrip(httpObservation());
  const validated = validateObservation(parsed);
  assert.equal(validated.ok, true);
  if (!validated.ok) return;
  assert.ok(Object.isFrozen(validated.value));
});

// Invalid states: observations ------------------------------------------------------------

test("unknown observation outcome is rejected", () => {
  const diagnostics = failedObservation(
    validateObservation({
      schema: "compat-observation/v1",
      scenarioId: "s",
      backend: { id: "reference" },
      outcome: "exploded",
      headers: [],
      body: { bytes: 0, truncated: false },
    })
  );
  assert.ok(diagnostics.some((d) => d.code === "schema-error" && d.path === "outcome"));
});

test("http-response without status is rejected by the semantic layer", () => {
  const diagnostics = failedObservation(
    validateObservation({
      schema: "compat-observation/v1",
      scenarioId: "s",
      backend: { id: "reference" },
      outcome: "http-response",
      headers: [],
      body: { bytes: 0, truncated: false },
    })
  );
  assert.deepEqual(
    diagnostics.map((d) => d.code),
    ["status-missing-for-http-response"]
  );
  assert.equal(diagnostics[0].path, "status");
});

test("non-HTTP outcome carrying a status is rejected (never status 0 or any other)", () => {
  for (const status of [200, 503]) {
    const diagnostics = failedObservation(
      validateObservation({
        schema: "compat-observation/v1",
        scenarioId: "s",
        backend: { id: "native" },
        outcome: "timeout",
        status,
        headers: [],
        body: { bytes: 0, truncated: false },
      })
    );
    assert.ok(
      diagnostics.some((d) => d.code === "status-present-for-non-http-outcome"),
      `status ${status} must be rejected on a timeout observation`
    );
  }
  // status 0 is rejected too (by the schema range check, before the semantic
  // layer runs) — the point stands: no synthetic status survives validation.
  const zeroStatus = failedObservation(
    validateObservation({
      schema: "compat-observation/v1",
      scenarioId: "s",
      backend: { id: "native" },
      outcome: "timeout",
      status: 0,
      headers: [],
      body: { bytes: 0, truncated: false },
    })
  );
  assert.ok(zeroStatus.length > 0);
});

test("malformed body metadata is rejected", () => {
  const missingBytes = {
    schema: "compat-observation/v1",
    scenarioId: "s",
    backend: { id: "reference" },
    outcome: "http-response",
    status: 200,
    headers: [],
    body: { truncated: false },
  };
  assert.ok(
    failedObservation(validateObservation(missingBytes)).some(
      (d) => d.code === "schema-error" && d.path === "body.bytes"
    )
  );
  const negativeBytes = {
    schema: "compat-observation/v1",
    scenarioId: "s",
    backend: { id: "reference" },
    outcome: "http-response",
    status: 200,
    headers: [],
    body: { bytes: -1, truncated: false },
  };
  assert.ok(
    failedObservation(validateObservation(negativeBytes)).some((d) => d.code === "schema-error")
  );
});

test("invalid backend identity is rejected", () => {
  const badId = {
    schema: "compat-observation/v1",
    scenarioId: "s",
    backend: { id: "upstream" },
    outcome: "http-response",
    status: 200,
    headers: [],
    body: { bytes: 0, truncated: false },
  };
  assert.ok(
    failedObservation(validateObservation(badId)).some(
      (d) => d.code === "schema-error" && d.path === "backend.id"
    )
  );
  const stringBackend = {
    schema: "compat-observation/v1",
    scenarioId: "s",
    backend: "reference",
    outcome: "http-response",
    status: 200,
    headers: [],
    body: { bytes: 0, truncated: false },
  };
  assert.ok(
    failedObservation(validateObservation(stringBackend)).some((d) => d.code === "schema-error")
  );
});

test("missing or empty scenario id is rejected", () => {
  const missing = {
    schema: "compat-observation/v1",
    backend: { id: "reference" },
    outcome: "http-response",
    status: 200,
    headers: [],
    body: { bytes: 0, truncated: false },
  };
  assert.ok(
    failedObservation(validateObservation(missing)).some(
      (d) => d.code === "schema-error" && d.path === "scenarioId"
    )
  );
  const empty = { ...missing, scenarioId: "" };
  assert.ok(failedObservation(validateObservation(empty)).some((d) => d.code === "schema-error"));
});

test("non-object observation input is rejected without throwing", () => {
  for (const input of ["str", 42, null, []]) {
    assert.deepEqual(
      failedObservation(validateObservation(input)).map((d) => d.code),
      ["schema-error"]
    );
  }
});

// Invalid states: results --------------------------------------------------------------------

test("unknown result verdict is rejected", () => {
  const diagnostics = failedResult(
    validateResult({
      schema: "compat-result/v1",
      scenarioId: "s",
      verdict: "tie",
      reference: { observation: "r.json", outcome: "http-response" },
      native: { observation: "n.json", outcome: "http-response" },
      mismatches: [],
      artifacts: { dir: "d", result: "d/result.json" },
    })
  );
  assert.ok(diagnostics.some((d) => d.code === "schema-error" && d.path === "verdict"));
});

test("pass verdict with mismatches is rejected", () => {
  const diagnostics = failedResult(
    validateResult({
      schema: "compat-result/v1",
      scenarioId: "s",
      verdict: "pass",
      reference: { observation: "r.json", outcome: "http-response" },
      native: { observation: "n.json", outcome: "http-response" },
      mismatches: [mismatch()],
      artifacts: { dir: "d", result: "d/result.json" },
    })
  );
  assert.deepEqual(
    diagnostics.map((d) => d.code),
    ["pass-with-mismatches"]
  );
  assert.equal(diagnostics[0].path, "mismatches");
});

test("fail verdict without mismatches is rejected; mismatch missing data is rejected", () => {
  const emptyFail = failedResult(
    validateResult({
      schema: "compat-result/v1",
      scenarioId: "s",
      verdict: "fail",
      reference: { observation: "r.json", outcome: "http-response" },
      native: { observation: "n.json", outcome: "http-response" },
      mismatches: [],
      artifacts: { dir: "d", result: "d/result.json" },
    })
  );
  assert.deepEqual(
    emptyFail.map((d) => d.code),
    ["fail-without-mismatches"]
  );

  const missingPath = failedResult(
    validateResult({
      schema: "compat-result/v1",
      scenarioId: "s",
      verdict: "fail",
      reference: { observation: "r.json", outcome: "http-response" },
      native: { observation: "n.json", outcome: "http-response" },
      mismatches: [
        { dimension: "status", rule: "exact", reference: "200", native: "500", normalized: false },
      ],
      artifacts: { dir: "d", result: "d/result.json" },
    })
  );
  assert.ok(missingPath.some((d) => d.code === "schema-error" && d.path === "mismatches[0].path"));
});

test("malformed mismatches are rejected", () => {
  const badDimension = {
    schema: "compat-result/v1",
    scenarioId: "s",
    verdict: "fail",
    reference: { observation: "r.json", outcome: "http-response" },
    native: { observation: "n.json", outcome: "http-response" },
    mismatches: [
      {
        dimension: "vibes",
        path: "p",
        rule: "r",
        reference: "a",
        native: "b",
        normalized: false,
      },
    ],
    artifacts: { dir: "d", result: "d/result.json" },
  };
  assert.ok(
    failedResult(validateResult(badDimension)).some(
      (d) => d.code === "schema-error" && d.path === "mismatches[0].dimension"
    )
  );
  const missingRule = {
    schema: "compat-result/v1",
    scenarioId: "s",
    verdict: "fail",
    reference: { observation: "r.json", outcome: "http-response" },
    native: { observation: "n.json", outcome: "http-response" },
    mismatches: [
      { dimension: "status", path: "p", reference: "a", native: "b", normalized: false },
    ],
    artifacts: { dir: "d", result: "d/result.json" },
  };
  assert.ok(failedResult(validateResult(missingRule)).some((d) => d.code === "schema-error"));
});

test("malformed artifact references are rejected", () => {
  const missingDir = {
    schema: "compat-result/v1",
    scenarioId: "s",
    verdict: "pass",
    reference: { observation: "r.json", outcome: "http-response" },
    native: { observation: "n.json", outcome: "http-response" },
    mismatches: [],
    artifacts: { result: "d/result.json" },
  };
  assert.ok(
    failedResult(validateResult(missingDir)).some(
      (d) => d.code === "schema-error" && d.path === "artifacts.dir"
    )
  );
  const numericArtifact = {
    schema: "compat-result/v1",
    scenarioId: "s",
    verdict: "fail",
    reference: { observation: "r.json", outcome: "http-response" },
    native: { observation: "n.json", outcome: "http-response" },
    mismatches: [{ ...mismatch(), artifact: 42 }],
    artifacts: { dir: "d", result: "d/result.json" },
  };
  assert.ok(failedResult(validateResult(numericArtifact)).some((d) => d.code === "schema-error"));
  const nullBodyArtifact = {
    schema: "compat-observation/v1",
    scenarioId: "s",
    backend: { id: "reference" },
    outcome: "http-response",
    status: 200,
    headers: [],
    body: { bytes: 10, truncated: true, capName: "capture.maxBodyBytes", artifact: null },
  };
  assert.ok(
    failedObservation(validateObservation(nullBodyArtifact)).some((d) => d.code === "schema-error")
  );
});

test("malformed normalization metadata is rejected", () => {
  const badMember = {
    schema: "compat-observation/v1",
    scenarioId: "s",
    backend: { id: "reference" },
    outcome: "http-response",
    status: 200,
    headers: [],
    body: { bytes: 0, truncated: false },
    normalizedBy: ["request-id", 42],
  };
  assert.ok(
    failedObservation(validateObservation(badMember)).some((d) => d.code === "schema-error")
  );
  const emptyMember = {
    schema: "compat-observation/v1",
    scenarioId: "s",
    backend: { id: "reference" },
    outcome: "http-response",
    status: 200,
    headers: [],
    body: { bytes: 0, truncated: false },
    normalizedBy: [""],
  };
  assert.ok(
    failedObservation(validateObservation(emptyMember)).some((d) => d.code === "schema-error")
  );
  const stringNormalized = {
    schema: "compat-result/v1",
    scenarioId: "s",
    verdict: "fail",
    reference: { observation: "r.json", outcome: "http-response" },
    native: { observation: "n.json", outcome: "http-response" },
    mismatches: [{ ...mismatch(), normalized: "yes" }],
    artifacts: { dir: "d", result: "d/result.json" },
  };
  assert.ok(failedResult(validateResult(stringNormalized)).some((d) => d.code === "schema-error"));
});

// Determinism, cwd-independence, secrecy ------------------------------------------------------------

test("diagnostics are byte-stable across repeated validation", () => {
  const invalid = {
    schema: "compat-observation/v1",
    backend: { id: "reference" },
    outcome: "http-response",
    headers: [{ value: "x" }],
    body: { truncated: false },
  };
  const snapshots = Array.from({ length: 5 }, () =>
    JSON.stringify(failedObservation(validateObservation(invalid)))
  );
  for (const snapshot of snapshots) assert.equal(snapshot, snapshots[0]);
});

test("validators resolve schemas independent of cwd", () => {
  const previousCwd = process.cwd();
  process.chdir(path.join(os.tmpdir()));
  try {
    assert.equal(validateObservation(roundTrip(httpObservation())).ok, true);
    assert.equal(validateResult(roundTrip(passResult())).ok, true);
  } finally {
    process.chdir(previousCwd);
  }
  assert.equal(process.cwd(), previousCwd);
});

test("diagnostics never echo fixture secrets", () => {
  const secret = "hunter2-obs-secret-marker";
  const diagnostics = failedObservation(
    validateObservation({
      schema: "compat-observation/v1",
      scenarioId: "s",
      backend: { id: "reference" },
      outcome: "http-response",
      status: "not-a-status",
      headers: [],
      body: { bytes: 0, truncated: false },
      failure: { kind: "harness-error", detail: secret },
    })
  );
  assert.ok(diagnostics.length > 0);
  assert.ok(!JSON.stringify(diagnostics).includes(secret));
});

test("source labels propagate into diagnostics when provided", () => {
  const diagnostics = failedResult(validateResult("nope", { source: "artifacts/x/result.json" }));
  assert.equal(diagnostics[0].code, "schema-error");
  assert.equal(diagnostics[0].source, "artifacts/x/result.json");
  const unsourced = failedResult(validateResult("nope"));
  assert.equal(unsourced[0].code, "schema-error");
  assert.ok(!("source" in unsourced[0]));
});
