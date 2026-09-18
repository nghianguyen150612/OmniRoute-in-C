/**
 * Stage 1 tests: compatibility scenario loader and validator (Task 006).
 *
 * Pure filesystem tests only: no backend is started, no HTTP is sent, no
 * network or provider credentials are required. Fixtures are created in a
 * temporary directory (removed in `test.after`); the only permanent fixture
 * read is the checked-in Task 005 Models examples artifact.
 */

import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

import {
  loadScenarioFile,
  type ScenarioDiagnostic,
  type ScenarioLoadResult,
} from "../../native/compat/scenarioLoader.ts";

const REPO_ROOT = fileURLToPath(new URL("../..", import.meta.url));
const MODELS_EXAMPLES = path.join(
  REPO_ROOT,
  "docs",
  "native-backend",
  "contracts",
  "compat-models-examples.json"
);

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), "compat-scenario-loader-"));

test.after(() => {
  fs.rmSync(tmpRoot, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
});

function writeFixture(name: string, content: string): string {
  const file = path.join(tmpRoot, name);
  fs.writeFileSync(file, content);
  return file;
}

function baseScenario(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    schema: "compat-scenario/v1",
    id: "minimal-probe",
    description: "minimal valid scenario",
    http: { method: "GET", path: "/v1/task004-does-not-exist" },
    compare: { status: 404 },
    ...overrides,
  };
}

function envelope(ids: string[]): string {
  return JSON.stringify({
    scenarios: ids.map((id, index) => baseScenario({ id, description: `scenario ${index}` })),
  });
}

function failed(result: ScenarioLoadResult): ScenarioDiagnostic[] {
  assert.equal(result.ok, false);
  if (result.ok) return [];
  return [...result.diagnostics];
}

function codes(result: ScenarioLoadResult): string[] {
  return failed(result).map((d) => d.code);
}

// 1. valid minimal scenario -------------------------------------------------

test("loads a valid minimal scenario with kind 'scenario'", () => {
  const file = writeFixture("minimal.json", JSON.stringify(baseScenario()));
  const result = loadScenarioFile(file);
  assert.equal(result.ok, true);
  if (!result.ok) return;
  assert.equal(result.kind, "scenario");
  assert.equal(result.scenarios.length, 1);
  assert.equal(result.scenarios[0].id, "minimal-probe");
  assert.equal(result.file, path.resolve(file));
});

// 2. Task 005 Models examples artifact ---------------------------------------

test("loads the checked-in Task 005 Models examples as a six-scenario set", () => {
  const result = loadScenarioFile(MODELS_EXAMPLES);
  assert.equal(result.ok, true, JSON.stringify(!result.ok ? result.diagnostics : null));
  if (!result.ok) return;
  assert.equal(result.kind, "scenario-set");
  assert.deepEqual(
    result.scenarios.map((s) => s.id),
    [
      "models-get-authenticated",
      "models-head",
      "models-options-preflight",
      "models-alias",
      "models-trailing-slash-redirect",
      "models-neighbor-json-404",
    ]
  );
});

// 3. nonexistent file ---------------------------------------------------------

test("nonexistent file yields file-not-found (distinct from parse failure)", () => {
  const result = loadScenarioFile(path.join(tmpRoot, "does-not-exist.json"));
  assert.deepEqual(codes(result), ["file-not-found"]);
});

// 4. empty file ---------------------------------------------------------------

test("empty and whitespace-only files yield empty-file", () => {
  assert.deepEqual(codes(loadScenarioFile(writeFixture("empty.json", ""))), ["empty-file"]);
  assert.deepEqual(codes(loadScenarioFile(writeFixture("blank.json", "  \n\t "))), ["empty-file"]);
});

// 5. malformed JSON ------------------------------------------------------------

test("malformed JSON yields parse-error with position, not fixture content", () => {
  const secret = "hunter2-parse-secret-marker";
  const file = writeFixture("malformed.json", `{"schema": "compat-scenario/v1", ${secret}`);
  const result = loadScenarioFile(file);
  const diagnostics = failed(result);
  assert.deepEqual(
    diagnostics.map((d) => d.code),
    ["parse-error"]
  );
  assert.match(diagnostics[0].message, /not valid JSON/);
  assert.ok(
    !JSON.stringify(diagnostics).includes(secret),
    "parse diagnostics must not echo fixture content"
  );
});

// 6. wrong top-level JSON type --------------------------------------------------

test("valid JSON of the wrong top-level type yields wrong-top-level-type", () => {
  for (const [name, content] of [
    ["array.json", "[1, 2]"],
    ["string.json", '"just a string"'],
    ["number.json", "42"],
    ["null.json", "null"],
    ["bool.json", "true"],
  ] as Array<[string, string]>) {
    assert.deepEqual(codes(loadScenarioFile(writeFixture(name, content))), [
      "wrong-top-level-type",
    ]);
  }
});

// 7. missing required field -----------------------------------------------------

test("missing required field yields schema-error naming the field path", () => {
  const { compare: _dropped, ...withoutCompare } = baseScenario();
  const result = loadScenarioFile(writeFixture("missing.json", JSON.stringify(withoutCompare)));
  const diagnostics = failed(result);
  assert.ok(diagnostics.some((d) => d.code === "schema-error" && d.path === "compare"));
  assert.ok(diagnostics.some((d) => /missing required field 'compare'/.test(d.message)));
});

// 8. invalid enum -----------------------------------------------------------------

test("invalid enum value yields schema-error listing allowed values", () => {
  const file = writeFixture("enum.json", JSON.stringify(baseScenario({ timeout: "eventually" })));
  const diagnostics = failed(loadScenarioFile(file));
  const timeoutDiag = diagnostics.find((d) => d.path === "timeout");
  assert.ok(timeoutDiag && timeoutDiag.code === "schema-error");
  assert.ok(
    timeoutDiag.message.includes('"api"'),
    `expected allowed values, got: ${timeoutDiag.message}`
  );
});

// 9. malformed scenario ID ----------------------------------------------------------

test("malformed scenario id yields schema-error at id", () => {
  const file = writeFixture("bad-id.json", JSON.stringify(baseScenario({ id: "Bad_ID!" })));
  const diagnostics = failed(loadScenarioFile(file));
  assert.ok(diagnostics.some((d) => d.code === "schema-error" && d.path === "id"));
});

// 10. unsupported schema version ------------------------------------------------------

test("unknown schema version yields unsupported-schema-version, not generic schema-error", () => {
  const file = writeFixture(
    "version.json",
    JSON.stringify(baseScenario({ schema: "compat-scenario/v99" }))
  );
  const diagnostics = failed(loadScenarioFile(file));
  assert.deepEqual(
    diagnostics.map((d) => d.code),
    ["unsupported-schema-version"]
  );
  assert.equal(diagnostics[0].path, "schema");
});

// 11. duplicate scenario IDs ------------------------------------------------------------

test("duplicate scenario ids in a set yield duplicate-scenario-id for later occurrences", () => {
  const file = writeFixture("dupes.json", envelope(["same-id", "other-id", "same-id"]));
  const result = loadScenarioFile(file);
  const diagnostics = failed(result);
  assert.deepEqual(
    diagnostics.map((d) => d.code),
    ["duplicate-scenario-id"]
  );
  assert.equal(diagnostics[0].index, 2);
  assert.equal(diagnostics[0].scenarioId, "same-id");
  assert.equal(diagnostics[0].path, "scenarios[2].id");
});

// 12. semantic cross-field violations -------------------------------------------------------

test("persistence observation outside the tables allowlist is rejected", () => {
  const file = writeFixture(
    "persistence.json",
    JSON.stringify(
      baseScenario({
        persistence: {
          tables: ["api_keys"],
          observations: [{ table: "call_logs", select: "count" }],
        },
      })
    )
  );
  const diagnostics = failed(loadScenarioFile(file));
  assert.ok(
    diagnostics.some(
      (d) =>
        d.code === "persistence-table-not-allowlisted" &&
        d.path === "persistence.observations[0].table"
    )
  );
});

test("persistence observations without a tables allowlist are rejected", () => {
  const file = writeFixture(
    "persistence-no-tables.json",
    JSON.stringify(
      baseScenario({
        persistence: { observations: [{ table: "call_logs", select: "count" }] },
      })
    )
  );
  const diagnostics = failed(loadScenarioFile(file));
  assert.ok(
    diagnostics.some(
      (d) => d.code === "persistence-allowlist-missing" && d.path === "persistence.tables"
    )
  );
});

test("redirect expectation with followRedirects true is rejected", () => {
  const file = writeFixture(
    "redirect.json",
    JSON.stringify(
      baseScenario({
        http: { method: "GET", path: "/v1/models/", followRedirects: true },
        compare: { status: 308, redirect: { location: "/v1/models" } },
      })
    )
  );
  const diagnostics = failed(loadScenarioFile(file));
  assert.ok(
    diagnostics.some(
      (d) => d.code === "redirect-follow-conflict" && d.path === "http.followRedirects"
    )
  );
});

// 13. deterministic diagnostic ordering -------------------------------------------------------

test("diagnostics for the same invalid fixture are byte-stable across loads", () => {
  const file = writeFixture(
    "multi.json",
    JSON.stringify({
      schema: "compat-scenario/v1",
      id: "Bad_ID!",
      http: { method: "GET" },
      timeout: "eventually",
      bogus: true,
    })
  );
  const snapshots = Array.from({ length: 5 }, () => {
    const result = loadScenarioFile(file);
    assert.equal(result.ok, false);
    return JSON.stringify(!result.ok ? result.diagnostics : null);
  });
  for (const snapshot of snapshots) assert.equal(snapshot, snapshots[0]);
  const paths = (JSON.parse(snapshots[0]) as ScenarioDiagnostic[]).map((d) => d.path ?? "");
  assert.deepEqual(paths, [...paths].sort());
});

// 14. mutation safety -----------------------------------------------------------------------------

test("loaded scenarios are deep-frozen; mutation attempts throw", () => {
  const file = writeFixture(
    "frozen.json",
    JSON.stringify(baseScenario({ tags: ["models"], http: { method: "GET", path: "/v1/models" } }))
  );
  const result = loadScenarioFile(file);
  assert.equal(result.ok, true);
  if (!result.ok) return;
  assert.ok(Object.isFrozen(result.scenarios));
  const scenario = result.scenarios[0];
  assert.ok(Object.isFrozen(scenario));
  assert.ok(Object.isFrozen(scenario.http));
  assert.ok(Object.isFrozen(scenario.compare));
  assert.throws(() => {
    (scenario as unknown as Record<string, unknown>)["id"] = "mutated";
  }, TypeError);
  assert.throws(() => {
    (scenario.http as unknown as Record<string, unknown>)["method"] = "POST";
  }, TypeError);
  assert.throws(() => {
    (scenario.tags as unknown as string[]).push("mutated");
  }, TypeError);
  assert.equal(scenario.id, "minimal-probe");
});

// 15. non-root cwd ----------------------------------------------------------------------------------

test("schema resolution is cwd-independent (invoked from a non-root directory)", () => {
  const previousCwd = process.cwd();
  process.chdir(tmpRoot);
  try {
    const result = loadScenarioFile(MODELS_EXAMPLES);
    assert.equal(result.ok, true, JSON.stringify(!result.ok ? result.diagnostics : null));
    if (!result.ok) return;
    assert.equal(result.scenarios.length, 6);
  } finally {
    process.chdir(previousCwd);
  }
  assert.equal(process.cwd(), previousCwd);
});

// Envelope structure and input-safety extras --------------------------------------------------------

test("unknown top-level fields are rejected, not ignored", () => {
  const file = writeFixture("unknown.json", JSON.stringify(baseScenario({ bogus: 1 })));
  const diagnostics = failed(loadScenarioFile(file));
  assert.ok(diagnostics.some((d) => d.code === "schema-error" && d.path === "bogus"));
});

test("envelope with non-array scenarios yields envelope-invalid", () => {
  const file = writeFixture("envelope.json", JSON.stringify({ scenarios: {} }));
  const diagnostics = failed(loadScenarioFile(file));
  assert.deepEqual(
    diagnostics.map((d) => d.code),
    ["envelope-invalid"]
  );
  assert.equal(diagnostics[0].path, "scenarios");
});

test("non-object scenario entries yield scenario-not-object with index", () => {
  const file = writeFixture("entry.json", JSON.stringify({ scenarios: ["nope"] }));
  const diagnostics = failed(loadScenarioFile(file));
  assert.deepEqual(
    diagnostics.map((d) => d.code),
    ["scenario-not-object"]
  );
  assert.equal(diagnostics[0].index, 0);
  assert.equal(diagnostics[0].path, "scenarios[0]");
});

test("unreadable file yields file-unreadable", async (t) => {
  const file = writeFixture("unreadable.json", JSON.stringify(baseScenario()));
  fs.chmodSync(file, 0o000);
  try {
    let readable = false;
    try {
      fs.readFileSync(file);
      readable = true;
    } catch {
      readable = false;
    }
    if (readable) {
      t.skip("file still readable after chmod 000 (elevated privileges?)");
      return;
    }
    assert.deepEqual(codes(loadScenarioFile(file)), ["file-unreadable"]);
  } finally {
    fs.chmodSync(file, 0o644);
  }
});

test("a directory path yields file-unreadable, not a crash", () => {
  assert.deepEqual(codes(loadScenarioFile(tmpRoot)), ["file-unreadable"]);
});

test("diagnostics never carry fixture secrets", () => {
  const secret = "hunter2-diag-secret-marker";
  const file = writeFixture(
    "secret.json",
    JSON.stringify(
      baseScenario({
        description: secret,
        auth: { kind: "invalid-key", value: secret },
        timeout: "eventually",
      })
    )
  );
  const dumped = JSON.stringify(failed(loadScenarioFile(file)));
  assert.ok(!dumped.includes(secret), "diagnostics must not echo fixture values");
});
