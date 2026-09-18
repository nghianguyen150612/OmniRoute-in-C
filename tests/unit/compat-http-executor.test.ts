/**
 * Stage 3 tests: single-backend HTTP executor (Task 008).
 *
 * Local loopback servers only: every test binds `127.0.0.1` with an
 * ephemeral OS-assigned port, never the operator port `20128`, never
 * OmniRoute itself, never the internet or any provider. Each server is
 * tracked in a registry and closed in a `finally`; a `test.after` hook
 * fails the file if any listener remains open.
 *
 * Prima facie network shapes (timeout/refused/reset) plus pure classifier
 * unit tests pin the Task 005 outcome taxonomy without flaky timing.
 */

import test from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import net from "node:net";
import { createHash } from "node:crypto";

import {
  buildCompatRequestUrl,
  classifyCompatTransportError,
  DEFAULT_COMPAT_TIMEOUTS_MS,
  executeCompatScenario,
  isCompatJsonContentType,
  mergeCompatRequestHeaders,
  resolveCompatTimeoutMs,
} from "../../native/compat/httpExecutor.ts";
import { validateObservation, type LoadedObservation } from "../../native/compat/observation.ts";
import type { LoadedScenario } from "../../native/compat/scenarioLoader.ts";

// ---------------------------------------------------------------------------
// Loopback server harness (tracked; nothing survives the file)
// ---------------------------------------------------------------------------

const openServers = new Set<http.Server>();

test.after(async () => {
  for (const server of [...openServers]) {
    server.closeAllConnections();
    await new Promise<void>((resolve) => server.close(() => resolve()));
    openServers.delete(server);
  }
  assert.equal(openServers.size, 0, "no Task 008 listener may remain open");
});

async function startServer(
  handler: (req: http.IncomingMessage, res: http.ServerResponse) => void
): Promise<{ server: http.Server; baseUrl: string }> {
  const server = http.createServer(handler);
  openServers.add(server);
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", () => resolve());
  });
  const address = server.address();
  assert.ok(address !== null && typeof address !== "string");
  assert.notEqual(address.port, 20128, "Task 008 tests must not use the operator port");
  return { server, baseUrl: `http://127.0.0.1:${address.port}` };
}

async function stopServer(server: http.Server): Promise<void> {
  server.closeAllConnections();
  await new Promise<void>((resolve) => server.close(() => resolve()));
  openServers.delete(server);
}

async function withServer<T>(
  handler: (req: http.IncomingMessage, res: http.ServerResponse) => void,
  fn: (baseUrl: string) => Promise<T>
): Promise<T> {
  const { server, baseUrl } = await startServer(handler);
  try {
    return await fn(baseUrl);
  } finally {
    await stopServer(server);
  }
}

function readBody(req: http.IncomingMessage): Promise<Buffer> {
  return new Promise((resolve, reject) => {
    const chunks: Buffer[] = [];
    req.on("data", (chunk) => chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk)));
    req.on("end", () => resolve(Buffer.concat(chunks)));
    req.on("error", reject);
  });
}

function echoHandler(req: http.IncomingMessage, res: http.ServerResponse): void {
  void readBody(req).then((body) => {
    if (req.url !== undefined && req.url.startsWith("/redir")) {
      res.writeHead(308, { location: "/final?x=1", refresh: "0;url=/final?x=1" });
      res.end();
      return;
    }
    if (req.url !== undefined && req.url.startsWith("/final")) {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ followed: true, url: req.url }));
      return;
    }
    if (req.method === "OPTIONS") {
      res.writeHead(204);
      res.end();
      return;
    }
    if (req.method === "DELETE") {
      res.writeHead(405, { "content-type": "application/json" });
      res.end(JSON.stringify({ error: { message: "method not allowed", type: "m", code: "c" } }));
      return;
    }
    if (req.method === "HEAD") {
      res.writeHead(200, { "content-type": "application/json", "content-length": "18" });
      res.end();
      return;
    }
    res.writeHead(200, { "content-type": "application/json" });
    res.end(
      JSON.stringify({
        method: req.method,
        url: req.url,
        headers: req.headers,
        body: body.toString("utf8"),
      })
    );
  });
}

// ---------------------------------------------------------------------------
// Scenario builder (frozen like Task 006 output; never mutated by tests)
// ---------------------------------------------------------------------------

function freezeDeep(value: unknown): void {
  if (value === null || typeof value !== "object" || Object.isFrozen(value)) return;
  for (const child of Object.values(value)) freezeDeep(child);
  Object.freeze(value);
}

function makeScenario(
  httpInit: Record<string, unknown> = {},
  extra: Record<string, unknown> = {},
  id = "executor-probe"
): LoadedScenario {
  const scenario = {
    schema: "compat-scenario/v1",
    id,
    description: "executor probe scenario",
    http: { method: "GET", path: "/probe", ...httpInit },
    compare: { status: 200 },
    ...extra,
  };
  freezeDeep(scenario);
  return scenario as unknown as LoadedScenario;
}

function mustValidate(obs: LoadedObservation): void {
  const revived = validateObservation(JSON.parse(JSON.stringify(obs)) as unknown);
  assert.equal(revived.ok, true, JSON.stringify(!revived.ok ? revived.diagnostics : null));
}

function asHttpResponse(obs: LoadedObservation): void {
  if (obs.outcome !== "http-response") assert.fail(`expected http-response, got ${obs.outcome}`);
}

function asNonHttp(obs: LoadedObservation): void {
  if (obs.outcome === "http-response") assert.fail("expected a non-HTTP observation");
}

async function closedLoopbackPort(): Promise<number> {
  const probe = net.createServer();
  await new Promise<void>((resolve) => probe.listen(0, "127.0.0.1", () => resolve()));
  const address = probe.address();
  assert.ok(address !== null && typeof address !== "string");
  const { port } = address;
  await new Promise<void>((resolve) => probe.close(() => resolve()));
  return port;
}

// ---------------------------------------------------------------------------
// 1. basic GET 200
// ---------------------------------------------------------------------------

test("basic GET 200 produces a valid frozen http-response observation", async () => {
  await withServer(echoHandler, async (baseUrl) => {
    const scenario = makeScenario({ path: "/probe" });
    const obs = await executeCompatScenario({ scenario, backend: { id: "reference" }, baseUrl });
    asHttpResponse(obs);
    assert.equal(obs.status, 200);
    assert.equal(obs.backend.id, "reference");
    assert.equal(obs.scenarioId, "executor-probe");
    assert.ok(Object.isFrozen(obs));
    assert.equal(typeof obs.timing?.wallMs, "number");
    assert.equal(typeof obs.timing?.ttfbMs, "number");
    mustValidate(obs);
  });
});

// ---------------------------------------------------------------------------
// 2. query construction/preservation
// ---------------------------------------------------------------------------

test("query entries serialize in insertion order with percent-encoding", async () => {
  await withServer(echoHandler, async (baseUrl) => {
    const scenario = makeScenario({
      path: "/search",
      query: { q: "hello world", tag: "a&b=c?d", empty: "" },
    });
    const obs = await executeCompatScenario({ scenario, backend: { id: "reference" }, baseUrl });
    asHttpResponse(obs);
    const echoed = obs.json as { url: string };
    assert.ok(echoed.url.startsWith("/search?"), echoed.url);
    assert.equal(echoed.url.slice("/search?".length), "q=hello%20world&tag=a%26b%3Dc%3Fd&empty=");
    const parsed = new URL(echoed.url, "http://loopback.test");
    assert.equal(parsed.searchParams.get("q"), "hello world");
    assert.equal(parsed.searchParams.get("tag"), "a&b=c?d");
    assert.equal(parsed.searchParams.get("empty"), "");
    mustValidate(obs);
  });
});

// ---------------------------------------------------------------------------
// 3. request headers
// ---------------------------------------------------------------------------

test("scenario and resolved request headers reach the backend", async () => {
  await withServer(echoHandler, async (baseUrl) => {
    const scenario = makeScenario({
      path: "/probe",
      headers: { "x-probe": "probe-value-7", accept: "application/json" },
    });
    const obs = await executeCompatScenario({
      scenario,
      backend: { id: "reference" },
      baseUrl,
      resolvedHeaders: { "x-extra": "extra-1" },
    });
    asHttpResponse(obs);
    const echoed = obs.json as { headers: Record<string, string> };
    assert.equal(echoed.headers["x-probe"], "probe-value-7");
    assert.equal(echoed.headers["accept"], "application/json");
    assert.equal(echoed.headers["x-extra"], "extra-1");
    mustValidate(obs);
  });
});

// ---------------------------------------------------------------------------
// 4. empty request body
// ---------------------------------------------------------------------------

test("absent request body sends no semantic content-type", async () => {
  await withServer(echoHandler, async (baseUrl) => {
    const scenario = makeScenario({ method: "POST", path: "/probe", body: null });
    const obs = await executeCompatScenario({ scenario, backend: { id: "reference" }, baseUrl });
    asHttpResponse(obs);
    const echoed = obs.json as { body: string; headers: Record<string, string> };
    assert.equal(echoed.body, "");
    assert.equal(echoed.headers["content-type"], undefined);
    mustValidate(obs);
  });
});

// ---------------------------------------------------------------------------
// 5. non-empty declared body
// ---------------------------------------------------------------------------

test("declared string body arrives intact without an invented content-type", async () => {
  await withServer(echoHandler, async (baseUrl) => {
    const scenario = makeScenario({ method: "POST", path: "/probe", body: "hello-body-123" });
    const obs = await executeCompatScenario({ scenario, backend: { id: "reference" }, baseUrl });
    asHttpResponse(obs);
    const echoed = obs.json as { body: string; headers: Record<string, string> };
    assert.equal(echoed.body, "hello-body-123");
    assert.equal(echoed.headers["content-type"], undefined);
    mustValidate(obs);
  });
});

// ---------------------------------------------------------------------------
// 6. HEAD with zero observed bytes
// ---------------------------------------------------------------------------

test("HEAD captures status and headers with zero body bytes", async () => {
  await withServer(echoHandler, async (baseUrl) => {
    const scenario = makeScenario({ method: "HEAD", path: "/probe" });
    const obs = await executeCompatScenario({ scenario, backend: { id: "reference" }, baseUrl });
    asHttpResponse(obs);
    assert.equal(obs.status, 200);
    assert.equal(obs.body.bytes, 0);
    assert.equal(obs.body.truncated, false);
    assert.ok(obs.headers.some((h) => h.name === "content-type"));
    mustValidate(obs);
  });
});

// ---------------------------------------------------------------------------
// 7. OPTIONS
// ---------------------------------------------------------------------------

test("OPTIONS observes the backend 204 without special-casing", async () => {
  await withServer(echoHandler, async (baseUrl) => {
    const scenario = makeScenario({ method: "OPTIONS", path: "/probe" });
    const obs = await executeCompatScenario({ scenario, backend: { id: "reference" }, baseUrl });
    asHttpResponse(obs);
    assert.equal(obs.status, 204);
    assert.equal(obs.body.bytes, 0);
    mustValidate(obs);
  });
});

// ---------------------------------------------------------------------------
// 8. unsupported method receiving an HTTP response
// ---------------------------------------------------------------------------

test("a backend-unsupported method still yields http-response (execution success)", async () => {
  await withServer(echoHandler, async (baseUrl) => {
    const scenario = makeScenario({ method: "DELETE", path: "/probe" });
    const obs = await executeCompatScenario({ scenario, backend: { id: "reference" }, baseUrl });
    asHttpResponse(obs);
    assert.equal(obs.status, 405);
    assert.deepEqual(obs.json, {
      error: { message: "method not allowed", type: "m", code: "c" },
    });
    mustValidate(obs);
  });
});

// ---------------------------------------------------------------------------
// 9. raw redirect with follow disabled
// ---------------------------------------------------------------------------

test("raw 308 redirect is observed verbatim when follow is disabled", async () => {
  await withServer(echoHandler, async (baseUrl) => {
    const scenario = makeScenario({ path: "/redir" });
    const obs = await executeCompatScenario({
      scenario,
      backend: { id: "reference" },
      baseUrl,
    });
    asHttpResponse(obs);
    assert.equal(obs.status, 308);
    assert.deepEqual(obs.redirect, { location: "/final?x=1", refresh: "0;url=/final?x=1" });
    const location = obs.headers.find((h) => h.name === "location");
    assert.equal(location?.value, "/final?x=1");
    mustValidate(obs);
  });
});

// ---------------------------------------------------------------------------
// 10. redirect-follow behavior
// ---------------------------------------------------------------------------

test("followRedirects follows to the final response", async () => {
  await withServer(echoHandler, async (baseUrl) => {
    const scenario = makeScenario({ path: "/redir", followRedirects: true });
    const obs = await executeCompatScenario({ scenario, backend: { id: "reference" }, baseUrl });
    asHttpResponse(obs);
    assert.equal(obs.status, 200);
    assert.deepEqual(obs.json, { followed: true, url: "/final?x=1" });
    mustValidate(obs);
  });
});

// ---------------------------------------------------------------------------
// 11. JSON response capture
// ---------------------------------------------------------------------------

test("JSON bodies are captured with hash, preview, and parsed value", async () => {
  await withServer(echoHandler, async (baseUrl) => {
    const scenario = makeScenario({ path: "/probe" });
    const obs = await executeCompatScenario({ scenario, backend: { id: "reference" }, baseUrl });
    asHttpResponse(obs);
    const echoed = obs.json as { method: string; url: string; body: string };
    assert.equal(echoed.method, "GET");
    assert.equal(echoed.url, "/probe");
    assert.equal(echoed.body, "");
    assert.ok(obs.body.bytes > 0);
    assert.equal(obs.body.truncated, false);
    assert.match(obs.body.sha256 ?? "", /^[0-9a-f]{64}$/);
    assert.ok((obs.body.textPreview ?? "").length > 0);
    assert.ok(!("jsonParse" in obs));
    mustValidate(obs);
  });
});

// ---------------------------------------------------------------------------
// 12. malformed JSON remains http-response
// ---------------------------------------------------------------------------

test("malformed JSON stays http-response with a recorded parse reason", async () => {
  await withServer(
    (_req, res) => {
      res.writeHead(200, { "content-type": "application/json" });
      res.end("not-json{{{");
    },
    async (baseUrl) => {
      const scenario = makeScenario({ path: "/broken" });
      const obs = await executeCompatScenario({ scenario, backend: { id: "reference" }, baseUrl });
      asHttpResponse(obs);
      assert.equal(obs.status, 200);
      assert.ok(!("json" in obs) || obs.json === undefined);
      assert.deepEqual(obs.jsonParse, { skipped: true, reason: "malformed-json" });
      mustValidate(obs);
    }
  );
});

// ---------------------------------------------------------------------------
// 13. empty response body
// ---------------------------------------------------------------------------

test("empty response body records zero bytes with an empty-body reason", async () => {
  await withServer(
    (_req, res) => {
      res.writeHead(200);
      res.end();
    },
    async (baseUrl) => {
      const scenario = makeScenario({ path: "/empty" });
      const obs = await executeCompatScenario({ scenario, backend: { id: "reference" }, baseUrl });
      asHttpResponse(obs);
      assert.equal(obs.body.bytes, 0);
      assert.equal(obs.body.truncated, false);
      assert.ok(!("sha256" in obs.body));
      assert.deepEqual(obs.jsonParse, { skipped: true, reason: "empty-body" });
      mustValidate(obs);
    }
  );
});

// ---------------------------------------------------------------------------
// 14. chunked response
// ---------------------------------------------------------------------------

test("chunked responses accumulate actual observed bytes", async () => {
  await withServer(
    (_req, res) => {
      res.writeHead(200, { "content-type": "text/plain" });
      res.write("chunk-a;");
      setTimeout(() => {
        res.write("chunk-b;");
        setTimeout(() => res.end("chunk-c"), 10);
      }, 10);
    },
    async (baseUrl) => {
      const scenario = makeScenario({ path: "/chunked" });
      const obs = await executeCompatScenario({ scenario, backend: { id: "reference" }, baseUrl });
      asHttpResponse(obs);
      assert.equal(obs.body.bytes, "chunk-a;chunk-b;chunk-c".length);
      assert.equal(obs.body.truncated, false);
      assert.equal(
        obs.body.sha256,
        createHash("sha256").update("chunk-a;chunk-b;chunk-c").digest("hex")
      );
      assert.ok((obs.body.textPreview ?? "").includes("chunk-b;"));
      mustValidate(obs);
    }
  );
});

// ---------------------------------------------------------------------------
// 15. response exactly at the capture limit
// ---------------------------------------------------------------------------

test("a body exactly at the capture limit is not truncated", async () => {
  const payload = "x".repeat(64);
  await withServer(
    (_req, res) => {
      res.writeHead(200, { "content-type": "text/plain", "content-length": "64" });
      res.end(payload);
    },
    async (baseUrl) => {
      const scenario = makeScenario({ path: "/exact" });
      const obs = await executeCompatScenario({
        scenario,
        backend: { id: "reference" },
        baseUrl,
        capture: { maxBodyBytes: 64 },
      });
      asHttpResponse(obs);
      assert.equal(obs.body.bytes, 64);
      assert.equal(obs.body.truncated, false);
      assert.ok(!("capName" in obs.body));
      assert.match(obs.body.sha256 ?? "", /^[0-9a-f]{64}$/);
      mustValidate(obs);
    }
  );
});

// ---------------------------------------------------------------------------
// 16. response above the capture limit
// ---------------------------------------------------------------------------

test("a body above the capture limit stays http-response with truncation metadata", async () => {
  const payload = `{"pad":"${"y".repeat(80)}"}`;
  await withServer(
    (_req, res) => {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(payload);
    },
    async (baseUrl) => {
      const scenario = makeScenario({ path: "/large" });
      const obs = await executeCompatScenario({
        scenario,
        backend: { id: "reference" },
        baseUrl,
        capture: { maxBodyBytes: 64 },
      });
      asHttpResponse(obs);
      assert.equal(obs.body.bytes, 64);
      assert.equal(obs.body.truncated, true);
      assert.equal(obs.body.capName, "capture.maxBodyBytes");
      assert.ok(!("sha256" in obs.body), "a truncated body has no full-content hash");
      assert.ok(!("json" in obs) || obs.json === undefined);
      assert.deepEqual(obs.jsonParse, { skipped: true, reason: "truncated" });
      mustValidate(obs);
    }
  );
});

// ---------------------------------------------------------------------------
// 17. timeout
// ---------------------------------------------------------------------------

test("a slow backend yields a distinct timeout observation", async () => {
  await withServer(
    (_req, _res) => {
      // never respond within the executor deadline
    },
    async (baseUrl) => {
      const scenario = makeScenario({ path: "/slow" }, { timeout: "health" });
      const obs = await executeCompatScenario({
        scenario,
        backend: { id: "reference" },
        baseUrl,
        timeoutMs: 100,
      });
      asNonHttp(obs);
      assert.equal(obs.outcome, "timeout");
      assert.ok(!("status" in obs), "a timeout must never carry a status");
      assert.equal(obs.failure?.kind, "timeout");
      assert.equal(obs.body.bytes, 0);
      mustValidate(obs);
    }
  );
});

// ---------------------------------------------------------------------------
// 18. connection refused
// ---------------------------------------------------------------------------

test("a closed port yields connection-refused, not a timeout", async () => {
  const port = await closedLoopbackPort();
  const scenario = makeScenario({ path: "/probe" });
  const obs = await executeCompatScenario({
    scenario,
    backend: { id: "reference" },
    baseUrl: `http://127.0.0.1:${port}`,
    timeoutMs: 2_000,
  });
  asNonHttp(obs);
  assert.equal(obs.outcome, "connection-refused");
  assert.ok(!("status" in obs));
  assert.equal(obs.failure?.kind, "connection-refused");
  mustValidate(obs);
});

// ---------------------------------------------------------------------------
// 19. connection reset
// ---------------------------------------------------------------------------

test("a socket destroyed mid-body yields connection-reset with partial bytes", async () => {
  await withServer(
    (_req, res) => {
      res.writeHead(200, { "content-type": "text/plain", "content-length": "100" });
      res.write("partial-bytes");
      setTimeout(() => res.socket?.destroy(), 20);
    },
    async (baseUrl) => {
      const scenario = makeScenario({ path: "/reset" });
      const obs = await executeCompatScenario({
        scenario,
        backend: { id: "reference" },
        baseUrl,
        timeoutMs: 5_000,
      });
      asNonHttp(obs);
      assert.equal(obs.outcome, "connection-reset");
      assert.ok(!("status" in obs));
      assert.equal(obs.failure?.kind, "connection-reset");
      assert.ok(obs.body.bytes > 0, "partial bytes are retained");
      assert.equal(obs.body.truncated, true);
      mustValidate(obs);
    }
  );
});

// ---------------------------------------------------------------------------
// 20. unexpected executor failure -> harness-error
// ---------------------------------------------------------------------------

test("an unexpected fetch failure becomes harness-error without leaking detail", async () => {
  const sentinel = "sentinel-fetch-boom-4k2x9";
  const scenario = makeScenario({ path: "/probe" });
  const obs = await executeCompatScenario({
    scenario,
    backend: { id: "reference" },
    baseUrl: "http://127.0.0.1:1",
    fetchImpl: (() => {
      throw new Error(`boom ${sentinel}`);
    }) as unknown as typeof fetch,
  });
  asNonHttp(obs);
  assert.equal(obs.outcome, "harness-error");
  assert.ok(!("status" in obs));
  assert.equal(obs.failure?.kind, "harness-error");
  assert.ok(!JSON.stringify(obs).includes(sentinel), "thrown error text must not leak");
  mustValidate(obs);
});

// ---------------------------------------------------------------------------
// 21. invalid base URL
// ---------------------------------------------------------------------------

test("invalid base URLs become harness-error without echoing input", async () => {
  const scenario = makeScenario({ path: "/probe" });
  const sentinel = "sentinel-baseurl-8m5q1";
  for (const baseUrl of [
    "not-a-url",
    "",
    "ftp://127.0.0.1:1/",
    `http://operator:${sentinel}@127.0.0.1:1/`,
  ]) {
    const obs = await executeCompatScenario({ scenario, backend: { id: "reference" }, baseUrl });
    asNonHttp(obs);
    assert.equal(obs.outcome, "harness-error", baseUrl);
    assert.ok(!JSON.stringify(obs).includes(sentinel), `baseUrl leak for ${baseUrl}`);
    mustValidate(obs);
  }
});

// ---------------------------------------------------------------------------
// 22. loaded scenario remains frozen/unmodified
// ---------------------------------------------------------------------------

test("the loaded scenario is never mutated by execution", async () => {
  await withServer(echoHandler, async (baseUrl) => {
    const scenario = makeScenario(
      { method: "POST", path: "/probe", headers: { "x-a": "1" }, query: { k: "v" }, body: "data" },
      { timeout: "api" }
    );
    const before = JSON.stringify(scenario);
    await executeCompatScenario({
      scenario,
      backend: { id: "native", revision: "test", mode: "test" },
      baseUrl,
      resolvedHeaders: { authorization: "Bearer test-credential" },
    });
    assert.equal(JSON.stringify(scenario), before);
    assert.ok(Object.isFrozen(scenario));
    assert.ok(Object.isFrozen(scenario.http));
  });
});

// ---------------------------------------------------------------------------
// 23/24. observations validate through Task 007 (both shapes)
// ---------------------------------------------------------------------------

test("http-response observations survive a JSON round-trip and revalidation", async () => {
  await withServer(echoHandler, async (baseUrl) => {
    const scenario = makeScenario({ path: "/probe" });
    const obs = await executeCompatScenario({ scenario, backend: { id: "reference" }, baseUrl });
    asHttpResponse(obs);
    mustValidate(obs);
    const revived = validateObservation(JSON.parse(JSON.stringify(obs)) as unknown);
    assert.equal(revived.ok, true);
  });
});

test("non-HTTP observations validate through Task 007 with no status key", async () => {
  const port = await closedLoopbackPort();
  const scenario = makeScenario({ path: "/probe" });
  const obs = await executeCompatScenario({
    scenario,
    backend: { id: "native" },
    baseUrl: `http://127.0.0.1:${port}`,
    timeoutMs: 2_000,
  });
  asNonHttp(obs);
  assert.ok(!("status" in obs));
  mustValidate(obs);
});

// ---------------------------------------------------------------------------
// 25. repeated response headers
// ---------------------------------------------------------------------------

test("repeated set-cookie values stay split; other repeats stay combined", async () => {
  await withServer(
    (_req, res) => {
      res.setHeader("set-cookie", ["a=1; Path=/", "b=2; Path=/"]);
      res.setHeader("x-multi", ["1", "2"]);
      res.writeHead(200, { "content-type": "text/plain" });
      res.end("ok");
    },
    async (baseUrl) => {
      const scenario = makeScenario({ path: "/cookies" });
      const obs = await executeCompatScenario({ scenario, backend: { id: "reference" }, baseUrl });
      asHttpResponse(obs);
      const cookies = obs.headers.filter((h) => h.name === "set-cookie").map((h) => h.value);
      assert.deepEqual(cookies, ["a=1; Path=/", "b=2; Path=/"]);
      const multi = obs.headers.filter((h) => h.name === "x-multi").map((h) => h.value);
      assert.deepEqual(multi, ["1, 2"]);
      mustValidate(obs);
    }
  );
});

// ---------------------------------------------------------------------------
// Secret safety (§16)
// ---------------------------------------------------------------------------

test("a failing request carrying credentials never leaks them", async () => {
  const sentinel = "sentinel-bearer-z9k4v2";
  const port = await closedLoopbackPort();
  const scenario = makeScenario({
    method: "POST",
    path: "/probe",
    headers: { "x-probe": "plain" },
    body: "non-secret-body",
  });
  const obs = await executeCompatScenario({
    scenario,
    backend: { id: "reference" },
    baseUrl: `http://127.0.0.1:${port}`,
    timeoutMs: 2_000,
    resolvedHeaders: { authorization: `Bearer ${sentinel}`, cookie: `session=${sentinel}` },
  });
  asNonHttp(obs);
  assert.equal(obs.outcome, "connection-refused");
  const dumped = JSON.stringify(obs);
  assert.ok(!dumped.includes(sentinel), "credential material must not appear in observations");
  assert.ok(!dumped.includes("non-secret-body"), "request bodies must not appear in observations");
  mustValidate(obs);
});

// ---------------------------------------------------------------------------
// Pure unit coverage: timeout mapping, URL building, header merge,
// transport classification, content-type detection
// ---------------------------------------------------------------------------

test("timeout categories map to the Task 005 evidence anchors", () => {
  assert.equal(DEFAULT_COMPAT_TIMEOUTS_MS.health, 5_000);
  assert.equal(DEFAULT_COMPAT_TIMEOUTS_MS.api, 10_000);
  assert.equal(DEFAULT_COMPAT_TIMEOUTS_MS.chat, 20_000);
  assert.equal(DEFAULT_COMPAT_TIMEOUTS_MS.stream, 30_000);
  assert.equal(resolveCompatTimeoutMs(undefined, {}), 10_000);
  assert.equal(resolveCompatTimeoutMs("health", {}), 5_000);
  assert.equal(resolveCompatTimeoutMs("chat", { timeouts: { chatMs: 1_500 } }), 1_500);
  assert.equal(resolveCompatTimeoutMs("api", { timeoutMs: 250 }), 250);
  assert.throws(() => resolveCompatTimeoutMs("api", { timeoutMs: 0 }), /finite positive/);
  assert.throws(
    () => resolveCompatTimeoutMs("api", { timeouts: { apiMs: Number.NaN } }),
    /finite positive/
  );
});

test("URL construction avoids double slashes and encodes queries deterministically", () => {
  assert.equal(
    buildCompatRequestUrl("http://127.0.0.1:9/", "/v1/models", { limit: "1", x: "probe" }),
    "http://127.0.0.1:9/v1/models?limit=1&x=probe"
  );
  assert.equal(
    buildCompatRequestUrl("http://127.0.0.1:9/v1/", "/models", undefined),
    "http://127.0.0.1:9/v1/models"
  );
  assert.equal(
    buildCompatRequestUrl("http://127.0.0.1:9", "/s", { q: "a b&c" }),
    "http://127.0.0.1:9/s?q=a%20b%26c"
  );
  assert.throws(() => buildCompatRequestUrl("not-a-url", "/v1/models"), /absolute http/);
  assert.throws(() => buildCompatRequestUrl("http://127.0.0.1:9", "v1/models"), /absolute path/);
  assert.throws(
    () => buildCompatRequestUrl("http://u:pw@127.0.0.1:9/", "/v1/models"),
    /must not embed credentials/
  );
});

test("header merge is case-insensitive with resolved headers winning", () => {
  const merged = mergeCompatRequestHeaders(
    { Accept: "application/json", "X-A": "scenario" },
    { "x-a": "resolved", "X-B": "extra" }
  );
  assert.deepEqual(merged, { Accept: "application/json", "x-a": "resolved", "X-B": "extra" });
  assert.throws(
    () => mergeCompatRequestHeaders({ "x-a": 42 as unknown as string }, undefined),
    /string-valued/
  );
});

test("transport classifier distinguishes timeout/refused/reset from unknown", () => {
  assert.equal(classifyCompatTransportError({ name: "TimeoutError" }), "timeout");
  assert.equal(classifyCompatTransportError({ name: "BodyTimeoutError" }), "timeout");
  const refused = new TypeError("fetch failed");
  (refused as unknown as { cause: unknown }).cause = new Error("connect ECONNREFUSED 127.0.0.1:1");
  (refused.cause as { code: string }).code = "ECONNREFUSED";
  assert.equal(classifyCompatTransportError(refused), "connection-refused");
  const reset = new TypeError("terminated");
  const socketError = new Error("other side closed");
  (socketError as unknown as { code: string }).code = "UND_ERR_SOCKET";
  (reset as unknown as { cause: unknown }).cause = socketError;
  assert.equal(classifyCompatTransportError(reset), "connection-reset");
  assert.equal(classifyCompatTransportError({ code: "ECONNRESET" }), "connection-reset");
  assert.equal(
    classifyCompatTransportError(new TypeError("Request with GET/HEAD method cannot have body.")),
    "unknown"
  );
  assert.equal(
    classifyCompatTransportError(new TypeError("'TRACE' HTTP method is unsupported.")),
    "unknown"
  );
  assert.equal(classifyCompatTransportError(new Error("boom")), "unknown");
  assert.equal(classifyCompatTransportError(null), "unknown");
});

test("JSON content-type detection ignores parameters and case", () => {
  assert.equal(isCompatJsonContentType("application/json"), true);
  assert.equal(isCompatJsonContentType("application/json; charset=utf-8"), true);
  assert.equal(isCompatJsonContentType("Application/JSON"), true);
  assert.equal(isCompatJsonContentType("text/plain"), false);
  assert.equal(isCompatJsonContentType(null), false);
});

test("artifact request bodies are rejected as harness-error without network use", async () => {
  const scenario = makeScenario({
    method: "POST",
    path: "/probe",
    body: { artifact: "corpus/large.bin" },
  });
  let fetched = false;
  const obs = await executeCompatScenario({
    scenario,
    backend: { id: "reference" },
    baseUrl: "http://127.0.0.1:1",
    fetchImpl: (async () => {
      fetched = true;
      throw new Error("must not be called");
    }) as unknown as typeof fetch,
  });
  asNonHttp(obs);
  assert.equal(obs.outcome, "harness-error");
  assert.equal(fetched, false);
  assert.match(obs.failure?.detail ?? "", /artifact/);
  mustValidate(obs);
});

test("non-empty GET bodies are rejected as harness-error without network use", async () => {
  const scenario = makeScenario({ method: "GET", path: "/probe", body: "should-not-send" });
  let fetched = false;
  const obs = await executeCompatScenario({
    scenario,
    backend: { id: "reference" },
    baseUrl: "http://127.0.0.1:1",
    fetchImpl: (async () => {
      fetched = true;
      throw new Error("must not be called");
    }) as unknown as typeof fetch,
  });
  asNonHttp(obs);
  assert.equal(obs.outcome, "harness-error");
  assert.equal(fetched, false);
  mustValidate(obs);
});

test("a missing scenario still yields harness-error instead of throwing", async () => {
  const obs = await executeCompatScenario({
    scenario: undefined as unknown as LoadedScenario,
    backend: { id: "reference" },
    baseUrl: "http://127.0.0.1:1",
  });
  asNonHttp(obs);
  assert.equal(obs.outcome, "harness-error");
  mustValidate(obs);
});

test("timeout while headers stream in retains partial bytes as timeout", async () => {
  await withServer(
    (_req, res) => {
      res.writeHead(200, { "content-type": "text/plain" });
      res.write("prefix-");
      // body never completes within the executor deadline
    },
    async (baseUrl) => {
      const scenario = makeScenario({ path: "/slow-body" });
      const obs = await executeCompatScenario({
        scenario,
        backend: { id: "reference" },
        baseUrl,
        timeoutMs: 150,
      });
      asNonHttp(obs);
      assert.equal(obs.outcome, "timeout");
      assert.ok(!("status" in obs));
      assert.ok(obs.body.bytes > 0, "bytes already read before the deadline are retained");
      assert.equal(obs.body.truncated, true);
      mustValidate(obs);
    }
  );
});
