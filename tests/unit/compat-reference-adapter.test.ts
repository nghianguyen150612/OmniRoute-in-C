/**
 * Stage 4 tests: Node reference-backend adapter lifecycle (Task 009).
 *
 * Two layers, per the task's bounded-real-server rule:
 *
 * - Fixture layer (fast, no real server): every failure-state and
 *   state-machine assertion runs against tiny `node -e` fixture children via
 *   the documented `spawnCommand` test seam. Loopback only; each fixture is
 *   stopped in a `finally` and a file-level `after` hook stops leftovers.
 * - Integration layer (exactly ONE real boot): a single test prepares,
 *   starts, readies, health-checks, describes, provisions, proves the
 *   Task 008 executor path, and stops one isolated reference backend.
 *
 * Never the operator port 20128, never the operator DATA_DIR, never the
 * internet, never providers, never billable requests.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { spawn, type ChildProcess } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";

import {
  allocateCompatFreePort,
  buildReferenceChildEnv,
  compatDescendantsOf,
  COMPAT_OPERATOR_PORT,
  redactSecrets,
  ReferenceAdapterError,
  ReferenceBackend,
  validateCompatPort,
  type CompatSpawnCommand,
} from "../../native/compat/referenceAdapter.ts";
import { executeCompatScenario } from "../../native/compat/httpExecutor.ts";
import { validateObservation } from "../../native/compat/observation.ts";
import type { LoadedScenario } from "../../native/compat/scenarioLoader.ts";

// ---------------------------------------------------------------------------
// Fixture children (loopback/test-seam only)
// ---------------------------------------------------------------------------

const SLEEPER: CompatSpawnCommand = {
  command: process.execPath,
  args: ["-e", "setInterval(() => {}, 1000);"],
};

const exiter = (code: number): CompatSpawnCommand => ({
  command: process.execPath,
  args: ["-e", `process.exit(${code});`],
});

const READY_SERVER: CompatSpawnCommand = {
  command: process.execPath,
  args: [
    "-e",
    [
      "const http = require('node:http');",
      "const port = Number(process.env.PORT);",
      "http.createServer((req, res) => {",
      "  if (req.url === '/api/health' || req.url === '/api/health/ping') {",
      "    res.writeHead(200, { 'content-type': 'application/json' });",
      '    res.end(\'{"status":"ok"}\');',
      "  } else { res.writeHead(404); res.end('{}'); }",
      "}).listen(port, '127.0.0.1');",
      "setInterval(() => {}, 1000);",
    ].join("\n"),
  ],
};

const NOISY_SLEEPER: CompatSpawnCommand = {
  command: process.execPath,
  args: [
    "-e",
    [
      "for (let i = 0; i < 500; i++) { console.log('out-line-' + i); console.error('err-line-' + i); }",
      "setInterval(() => {}, 1000);",
    ].join("\n"),
  ],
};

const SECRET_ECHO_EXITER: CompatSpawnCommand = {
  command: process.execPath,
  args: ["-e", "console.log('leak:' + process.env.INITIAL_PASSWORD); process.exit(1);"],
};

const SPAWNING_SLEEPER: CompatSpawnCommand = {
  command: process.execPath,
  args: [
    "-e",
    [
      "const { spawn } = require('node:child_process');",
      "spawn(process.execPath, ['-e', 'setInterval(() => {}, 1000);'], { stdio: 'ignore' }).unref();",
      "setInterval(() => {}, 1000);",
    ].join("\n"),
  ],
};

// ---------------------------------------------------------------------------
// Tracking (nothing task-owned survives the file)
// ---------------------------------------------------------------------------

const liveAdapters = new Set<ReferenceBackend>();
const liveChildren: ChildProcess[] = [];

function track(adapter: ReferenceBackend): ReferenceBackend {
  liveAdapters.add(adapter);
  return adapter;
}

test.after(async () => {
  for (const adapter of [...liveAdapters]) {
    try {
      await adapter.stop();
    } catch {
      // stop() is idempotent and safe; a throw here still releases tracking.
    }
    liveAdapters.delete(adapter);
  }
  for (const child of liveChildren.splice(0)) {
    try {
      child.kill("SIGKILL");
    } catch {
      // already gone.
    }
  }
  assert.equal(liveAdapters.size, 0, "no Task 009 adapter may remain tracked");
});

function childGone(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return false;
  } catch {
    return true;
  }
}

async function canBind(port: number): Promise<boolean> {
  return new Promise((resolve) => {
    const server = net.createServer();
    server.once("error", () => resolve(false));
    server.listen(port, "127.0.0.1", () => server.close(() => resolve(true)));
  });
}

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

test("free-port allocation never returns the operator port", async () => {
  for (let i = 0; i < 3; i++) {
    const port = await allocateCompatFreePort();
    assert.ok(Number.isInteger(port) && port >= 1 && port <= 65535);
    assert.notEqual(port, COMPAT_OPERATOR_PORT);
    assert.ok(await canBind(port), "allocated port must be bindable loopback");
  }
});

test("explicit operator port is refused", async () => {
  assert.throws(() => validateCompatPort(COMPAT_OPERATOR_PORT), /operator port/);
  assert.throws(() => validateCompatPort(0), /1\.\.65535/);
  assert.throws(() => validateCompatPort(70000), /1\.\.65535/);
  const before = new Set(
    fs.readdirSync(os.tmpdir()).filter((entry) => entry.startsWith("omniroute-compat-ref-"))
  );
  await assert.rejects(
    ReferenceBackend.prepare({ port: COMPAT_OPERATOR_PORT, spawnCommand: SLEEPER }),
    /operator port/
  );
  await assert.rejects(
    ReferenceBackend.prepare({ spawnCommand: SLEEPER, readyTimeoutMs: 0 }),
    /finite positive/
  );
  const after = fs
    .readdirSync(os.tmpdir())
    .filter((entry) => entry.startsWith("omniroute-compat-ref-"));
  assert.deepEqual(
    after.filter((entry) => !before.has(entry)),
    [],
    "rejected prepare must not leak an adapter-owned DATA_DIR"
  );
});

test("child env isolates operator state and pins harness keys", () => {
  const env = buildReferenceChildEnv(
    { ...process.env, OMNIROUTE_API_KEY: "operator-secret", DATA_DIR: "/operator/data" },
    {
      port: 23117,
      dataDir: "/tmp/omniroute-compat-ref-xyz",
      secrets: { jwtSecret: "j", apiKeySecret: "a", initialPassword: "p" },
    }
  );
  assert.equal(env["PORT"], "23117");
  assert.equal(env["HOST"], "127.0.0.1");
  assert.equal(env["DATA_DIR"], "/tmp/omniroute-compat-ref-xyz");
  assert.equal(env["OMNIROUTE_API_KEY"], "");
  assert.equal(env["ROUTER_API_KEY"], "");
  assert.equal(env["JWT_SECRET"], "j");
  assert.equal(env["INITIAL_PASSWORD"], "p");
});

test("secret redaction replaces pinned secrets and ignores short values", () => {
  const secrets = {
    jwtSecret: "sentinel-jwt-abc123",
    apiKeySecret: "short",
    initialPassword: "sentinel-pw-xyz789",
  };
  const redacted = redactSecrets("a sentinel-jwt-abc123 b short c sentinel-pw-xyz789", secrets);
  assert.ok(!redacted.includes("sentinel-jwt-abc123"));
  assert.ok(!redacted.includes("sentinel-pw-xyz789"));
  assert.ok(redacted.includes("short"), "short values must not blank ordinary text");
  assert.ok(redacted.includes("[redacted]"));
});

// ---------------------------------------------------------------------------
// Prepare isolation
// ---------------------------------------------------------------------------

test("prepare creates distinct isolated state per instance", async () => {
  const first = track(await ReferenceBackend.prepare({ spawnCommand: SLEEPER }));
  const second = track(await ReferenceBackend.prepare({ spawnCommand: SLEEPER }));
  try {
    assert.equal(first.getState(), "prepared");
    const a = first.describe();
    const b = second.describe();
    assert.notEqual(a.dataDir, b.dataDir);
    for (const entry of [a, b]) {
      assert.ok(entry.dataDir.startsWith(`${os.tmpdir()}${path.sep}`));
      assert.ok(path.basename(entry.dataDir).startsWith("omniroute-compat-ref-"));
      assert.ok(fs.existsSync(entry.dataDir), "DATA_DIR exists after prepare");
      assert.equal(entry.id, "reference");
      assert.equal(entry.mode, "dev");
    }
  } finally {
    await first.stop();
    await second.stop();
  }
  assert.ok(!fs.existsSync(first.describe().dataDir), "default policy removes DATA_DIR on stop");
  assert.ok(!fs.existsSync(second.describe().dataDir));
});

test("operator DATA_DIR is never used", async () => {
  const adapter = track(await ReferenceBackend.prepare({ spawnCommand: SLEEPER }));
  try {
    const { dataDir } = adapter.describe();
    const operatorCandidates = [
      process.env["DATA_DIR"] ?? "",
      path.join(os.homedir(), ".omniroute"),
    ];
    for (const candidate of operatorCandidates) {
      if (candidate.length > 0) assert.notEqual(dataDir, candidate);
    }
  } finally {
    await adapter.stop();
  }
});

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

test("start records owned process identity; duplicate start is rejected", async () => {
  const adapter = track(await ReferenceBackend.prepare({ spawnCommand: SLEEPER }));
  try {
    adapter.start();
    assert.equal(adapter.getState(), "starting");
    const { pid } = adapter.describe();
    assert.ok(pid !== null && pid > 0);
    assert.ok(!childGone(pid), "owned child is alive after start");
    assert.throws(() => adapter.start(), /already-started/);
  } finally {
    const summary = await adapter.stop();
    assert.equal(adapter.getState(), "stopped");
    assert.equal(summary.pid, adapter.describe().pid);
  }
});

test("waitReady and provisionApiKey before start are rejected", async () => {
  const adapter = track(await ReferenceBackend.prepare({ spawnCommand: READY_SERVER }));
  try {
    await assert.rejects(adapter.waitReady(), /start\(\) before/);
    await assert.rejects(adapter.provisionApiKey(), /ready state/);
    assert.throws(() => adapter.baseUrl(), /ready state/);
  } finally {
    await adapter.stop();
  }
});

test("start after stop is rejected; a fresh instance is required", async () => {
  const adapter = track(await ReferenceBackend.prepare({ spawnCommand: SLEEPER }));
  adapter.start();
  await adapter.stop();
  assert.throws(() => adapter.start(), /cannot start from state stopped/);
});

test("health before start and after stop reports not-alive without probing", async () => {
  const adapter = track(await ReferenceBackend.prepare({ spawnCommand: SLEEPER }));
  try {
    const before = await adapter.health();
    assert.equal(before.alive, false);
    assert.equal(before.probe, "unprobed");
    adapter.start();
    await adapter.stop();
    const after = await adapter.health();
    assert.equal(after.alive, false);
    assert.equal(after.state, "stopped");
  } finally {
    await adapter.stop();
  }
});

// ---------------------------------------------------------------------------
// Readiness against a fixture ready-server
// ---------------------------------------------------------------------------

test("readiness succeeds against a healthy fixture backend", async () => {
  const adapter = track(
    await ReferenceBackend.prepare({
      spawnCommand: READY_SERVER,
      readyTimeoutMs: 15_000,
      probeTimeoutMs: 1_000,
    })
  );
  try {
    adapter.start();
    const baseUrl = await adapter.waitReady();
    assert.match(baseUrl, /^http:\/\/127\.0\.0\.1:\d+$/);
    assert.equal(adapter.baseUrl(), baseUrl);
    assert.equal(adapter.getState(), "ready");
    const probe = await fetch(`${baseUrl}/api/health`, { signal: AbortSignal.timeout(2_000) });
    assert.equal(probe.status, 200);
    const health = await adapter.health();
    assert.equal(health.alive, true);
    assert.equal(health.probe, "ok");
  } finally {
    await adapter.stop();
  }
});

test("describe exposes a fixed non-secret key allowlist", async () => {
  const adapter = track(
    await ReferenceBackend.prepare({
      spawnCommand: READY_SERVER,
      readyTimeoutMs: 15_000,
      probeTimeoutMs: 1_000,
      secrets: {
        jwtSecret: "sentinel-describe-jwt-1",
        apiKeySecret: "sentinel-describe-api-2",
        initialPassword: "sentinel-describe-pw-3",
      },
    })
  );
  try {
    adapter.start();
    await adapter.waitReady();
    const described = adapter.describe();
    const allowed = new Set(["id", "state", "baseUrl", "pid", "dataDir", "revision", "mode"]);
    for (const key of Object.keys(described)) assert.ok(allowed.has(key), `unexpected key ${key}`);
    const dumped = JSON.stringify(described);
    assert.ok(!dumped.includes("sentinel-describe-jwt-1"));
    assert.ok(!dumped.includes("sentinel-describe-api-2"));
    assert.ok(!dumped.includes("sentinel-describe-pw-3"));
  } finally {
    await adapter.stop();
  }
});

// ---------------------------------------------------------------------------
// Failure taxonomy (fixture children, no real server)
// ---------------------------------------------------------------------------

test("early child exit is classified, not mistaken for a timeout", async () => {
  const adapter = track(
    await ReferenceBackend.prepare({
      spawnCommand: exiter(3),
      readyTimeoutMs: 15_000,
      probeTimeoutMs: 500,
    })
  );
  const startedAt = Date.now();
  adapter.start();
  const error = await adapter.waitReady().then(
    () => null,
    (err: unknown) => err
  );
  assert.ok(error instanceof ReferenceAdapterError);
  assert.equal(error.code, "early-exit");
  assert.match(error.message, /code=3/);
  assert.ok(Date.now() - startedAt < 15_000, "early exit short-circuits the deadline");
  assert.equal(adapter.getState(), "failed");
  const summary = await adapter.stop();
  assert.equal(adapter.getState(), "stopped");
  assert.deepEqual(summary.exit, { code: 3, signal: null });
});

test("readiness timeout is bounded and distinct from early exit", async () => {
  const adapter = track(
    await ReferenceBackend.prepare({
      spawnCommand: SLEEPER,
      readyTimeoutMs: 900,
      probeTimeoutMs: 250,
    })
  );
  const startedAt = Date.now();
  adapter.start();
  const error = await adapter.waitReady().then(
    () => null,
    (err: unknown) => err
  );
  const elapsed = Date.now() - startedAt;
  assert.ok(error instanceof ReferenceAdapterError);
  assert.equal(error.code, "readiness-timeout");
  assert.ok(elapsed < 10_000, `deadline must bound the wait (took ${elapsed} ms)`);
  assert.equal(adapter.getState(), "failed");
  await adapter.stop();
});

test("log capture is bounded to the configured ring", async () => {
  const adapter = track(
    await ReferenceBackend.prepare({ spawnCommand: NOISY_SLEEPER, logLines: 50 })
  );
  try {
    adapter.start();
    await new Promise((resolve) => setTimeout(resolve, 1_500));
    const sizes = adapter.logSizes();
    assert.equal(sizes.cap, 50);
    assert.ok(sizes.stdout <= 50, `stdout ring bounded (got ${sizes.stdout})`);
    assert.ok(sizes.stderr <= 50, `stderr ring bounded (got ${sizes.stderr})`);
    assert.ok(sizes.stdout > 0 && sizes.stderr > 0, "fixture output was captured");
  } finally {
    await adapter.stop();
  }
});

test("generated secrets never appear in failure diagnostics", async () => {
  const sentinel = "sentinel-initial-pw-7k2q9";
  const adapter = track(
    await ReferenceBackend.prepare({
      spawnCommand: SECRET_ECHO_EXITER,
      readyTimeoutMs: 15_000,
      probeTimeoutMs: 500,
      secrets: {
        jwtSecret: "sentinel-jwt-4m8d2",
        apiKeySecret: "sentinel-api-6n3p7",
        initialPassword: sentinel,
      },
    })
  );
  adapter.start();
  const error = await adapter.waitReady().then(
    () => null,
    (err: unknown) => err
  );
  assert.ok(error instanceof ReferenceAdapterError);
  assert.equal(error.code, "early-exit");
  assert.ok(!error.message.includes(sentinel), "echoed secret must be redacted from tails");
  assert.ok(!error.message.includes("sentinel-jwt-4m8d2"));
  assert.ok(error.message.includes("[redacted]"), "redaction marker is present");
  await adapter.stop();
});

// ---------------------------------------------------------------------------
// Shutdown ownership
// ---------------------------------------------------------------------------

test("stop after ready releases the listener and the child", async () => {
  const adapter = track(
    await ReferenceBackend.prepare({
      spawnCommand: READY_SERVER,
      readyTimeoutMs: 15_000,
      probeTimeoutMs: 1_000,
    })
  );
  adapter.start();
  const baseUrl = await adapter.waitReady();
  const port = Number(new URL(baseUrl).port);
  const pid = adapter.describe().pid as number;
  const first = await adapter.stop();
  assert.equal(first.dataDirRemoved, true);
  assert.deepEqual(first.leftoverPids, []);
  assert.ok(childGone(pid), "owned child is gone after stop");
  assert.ok(await canBind(port), "owned listener is released after stop");
  const second = await adapter.stop();
  assert.deepEqual(second, first, "repeated stop returns the same summary");
});

test("stop after partial startup failure still cleans up", async () => {
  const adapter = track(
    await ReferenceBackend.prepare({
      spawnCommand: exiter(2),
      readyTimeoutMs: 15_000,
      probeTimeoutMs: 500,
    })
  );
  adapter.start();
  await adapter.waitReady().then(
    () => assert.fail("waitReady must throw on early exit"),
    () => undefined
  );
  const summary = await adapter.stop();
  assert.equal(adapter.getState(), "stopped");
  assert.ok(!fs.existsSync(adapter.describe().dataDir));
  assert.deepEqual(summary.leftoverPids, []);
});

test("keepDataDir retains the isolated dir without touching its parent", async () => {
  const adapter = track(
    await ReferenceBackend.prepare({ spawnCommand: SLEEPER, keepDataDir: true })
  );
  adapter.start();
  const { dataDir } = adapter.describe();
  const summary = await adapter.stop();
  assert.equal(summary.dataDirRemoval, "kept-by-option");
  assert.ok(fs.existsSync(dataDir), "opt-in retention keeps the dir");
  fs.rmSync(dataDir, { recursive: true, force: true });
  assert.ok(fs.existsSync(os.tmpdir()), "shared tmp parent is untouched");
});

test("stop never signals processes it did not spawn", async () => {
  const bystander = spawn(process.execPath, ["-e", "setInterval(() => {}, 1000);"], {
    stdio: "ignore",
  });
  liveChildren.push(bystander);
  assert.ok(bystander.pid !== undefined);
  const adapter = track(
    await ReferenceBackend.prepare({
      spawnCommand: READY_SERVER,
      readyTimeoutMs: 15_000,
      probeTimeoutMs: 1_000,
    })
  );
  try {
    adapter.start();
    await adapter.waitReady();
    await adapter.stop();
    assert.ok(!childGone(bystander.pid as number), "bystander survives adapter stop");
  } finally {
    await adapter.stop();
    bystander.kill("SIGKILL");
  }
});

test("owned descendants are reaped on stop (Linux /proc ownership)", async () => {
  if (process.platform !== "linux") {
    assert.ok(true, "descendant ownership check requires /proc; covered on Linux CI");
    return;
  }
  const adapter = track(
    await ReferenceBackend.prepare({
      spawnCommand: SPAWNING_SLEEPER,
      readyTimeoutMs: 15_000,
      probeTimeoutMs: 500,
      shutdownGraceMs: 1_000,
    })
  );
  try {
    adapter.start();
    await new Promise((resolve) => setTimeout(resolve, 1_500));
    const pid = adapter.describe().pid as number;
    const before = compatDescendantsOf(pid);
    assert.ok(before.length >= 1, "fixture must own at least one descendant");
    const summary = await adapter.stop();
    assert.deepEqual(summary.leftoverPids, [], "no owned descendant remains");
    assert.deepEqual(compatDescendantsOf(pid), []);
  } finally {
    await adapter.stop();
  }
});

// ---------------------------------------------------------------------------
// Integration: exactly ONE real isolated reference backend boot
// ---------------------------------------------------------------------------

function freezeDeep(value: unknown): void {
  if (value === null || typeof value !== "object" || Object.isFrozen(value)) return;
  for (const child of Object.values(value)) freezeDeep(child);
  Object.freeze(value);
}

function healthScenario(): LoadedScenario {
  const scenario = {
    schema: "compat-scenario/v1",
    id: "reference-liveness-probe",
    description: "adapter integration proof: liveness is observable through the generic executor",
    http: { method: "GET", path: "/api/health" },
    compare: { status: 200 },
    timeout: "health",
  };
  freezeDeep(scenario);
  return scenario as unknown as LoadedScenario;
}

test(
  "real isolated reference backend: ready, health, executor proof, provision, clean stop",
  { timeout: 420_000 },
  async () => {
    const adapter = track(
      await ReferenceBackend.prepare({ readyTimeoutMs: 300_000, probeTimeoutMs: 5_000 })
    );
    try {
      const { dataDir } = adapter.describe();
      assert.ok(dataDir.startsWith(`${os.tmpdir()}${path.sep}`), "isolated DATA_DIR");

      adapter.start();
      const bootStarted = Date.now();
      const baseUrl = await adapter.waitReady();
      const bootMs = Date.now() - bootStarted;
      assert.match(baseUrl, /^http:\/\/127\.0\.0\.1:(?!20128\b)\d+$/);
      assert.ok(bootMs < 300_000, `bounded boot (took ${bootMs} ms)`);

      const health = await adapter.health();
      assert.equal(health.alive, true);
      assert.equal(health.probe, "ok");

      const described = adapter.describe();
      assert.equal(described.id, "reference");
      assert.equal(described.state, "ready");
      assert.equal(described.mode, "dev");

      // Adapter → Task 008 executor proof over a cheap public endpoint.
      const observation = await executeCompatScenario({
        scenario: healthScenario(),
        backend: { id: "reference" },
        baseUrl,
      });
      assert.equal(observation.outcome, "http-response");
      assert.equal(observation.status, 200);
      const validation = validateObservation(JSON.parse(JSON.stringify(observation)) as unknown);
      assert.equal(validation.ok, true);

      // Runtime provisioning through the product flow, isolated backend only.
      const apiKey = await adapter.provisionApiKey("task009-proof");
      assert.ok(apiKey.length > 0, "a key is minted");
      assert.ok(!JSON.stringify(adapter.describe()).includes(apiKey), "key never in describe()");
    } finally {
      const summary = await adapter.stop();
      assert.equal(adapter.getState(), "stopped");
      assert.equal(summary.dataDirRemoved, true);
      assert.deepEqual(summary.leftoverPids, []);
      const pid = adapter.describe().pid;
      if (pid !== null) assert.ok(childGone(pid), "reference child is gone after stop");
      assert.ok(!fs.existsSync(adapter.describe().dataDir), "isolated DATA_DIR removed");
      const after = await adapter.health();
      assert.equal(after.alive, false);
    }
  }
);
