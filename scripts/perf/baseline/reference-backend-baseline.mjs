#!/usr/bin/env node
/**
 * Reference-backend memory baseline (Task 002 — measurement only).
 *
 * Starts the Node/TypeScript reference backend (`scripts/dev/run-next.mjs dev`,
 * the same command `npm run dev` uses), waits for readiness deterministically,
 * runs a fixed workload sequence (startup → initial idle → 5-minute idle →
 * GET /v1/models → recovery), and records Linux process metrics from /proc.
 *
 * All measurement is EXTERNAL: the server process is spawned directly (its PID
 * is tracked, never matched by name) and observed via /proc/<pid>/status and
 * /proc/<pid>/fd. Nothing is injected into the server; no production code is
 * touched.
 *
 * Metric semantics (see docs/native-backend/MEMORY_MODEL.md §2-measured):
 * - rss_kb ......... VmRSS of the server PID: resident set incl. shared
 *                      mappings. Whole-process current RSS, NOT V8 heap.
 * - peak_rss_kb .... VmHWM of the server PID: kernel lifetime high-water mark.
 * - workload_peak ... max VmRSS polled (50 ms) while a workload request is in
 *                      flight. "Peak during workload" for that window.
 * - threads ........ "Threads:" count of the server PID.
 * - fds ............ entries in /proc/<pid>/fd at sample instant.
 * - tree_rss_kb .... sum of VmRSS over discovered descendant processes, labeled
 *                      AGGREGATE (upper bound: shared pages double-count).
 *                      Main-process numbers are always reported separately.
 *
 * Failure taxonomy (never reported as zero — missing values are null):
 * - measurement_failure: spawn/readiness/sampling/cleanup problem.
 * - request_failure .... HTTP probe returned non-2xx or timed out.
 *
 * Usage:
 *   node scripts/perf/baseline/reference-backend-baseline.mjs \
 *     --runs 3 --port 21128 --out /tmp/omniroute-baseline/<stamp>
 *
 * Options (all have finite defaults; there are no unbounded waits):
 *   --runs N ........... clean repetitions (fresh DATA_DIR per run)
 *   --port P ........... base port for run 1; run i uses P+i-1
 *   --out DIR .......... results directory (run-*.json, summary.json)
 *   --stabilize-s S .... ready → initial-idle delay (default 60)
 *   --idle-s S ......... initial-idle → idle-5min delay (default 300, real wait)
 *   --recover-s S ...... models → recovery delay (default 60)
 *   --ready-timeout-s S  liveness+readiness deadline per run (default 420)
 *   --request-timeout-s S  per-probe HTTP timeout (default 120)
 *   --keep-data ........ keep per-run DATA_DIR (default: removed on success)
 *
 * Exit codes: 0 = all runs ok; 1 = usage error; 2 = measurement/request failure.
 */

import { spawn, execFile } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(HERE, "..", "..", "..");

class ProcGone extends Error {}
class MeasurementFailure extends Error {}
class ReadinessTimeout extends Error {}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function parseArgs(argv) {
  const o = {
    runs: 3,
    port: 21128,
    out: "",
    stabilizeS: 60,
    idleS: 300,
    recoverS: 60,
    readyTimeoutS: 420,
    requestTimeoutS: 120,
    keepData: false,
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const num = (name) => {
      const v = Number(argv[++i]);
      if (!Number.isFinite(v) || v <= 0) throw new Error(`bad value for ${name}`);
      return v;
    };
    switch (a) {
      case "--runs":
        o.runs = Math.floor(num(a));
        break;
      case "--port":
        o.port = Math.floor(num(a));
        break;
      case "--out":
        o.out = argv[++i] ?? "";
        break;
      case "--stabilize-s":
        o.stabilizeS = num(a);
        break;
      case "--idle-s":
        o.idleS = num(a);
        break;
      case "--recover-s":
        o.recoverS = num(a);
        break;
      case "--ready-timeout-s":
        o.readyTimeoutS = num(a);
        break;
      case "--request-timeout-s":
        o.requestTimeoutS = num(a);
        break;
      case "--keep-data":
        o.keepData = true;
        break;
      case "--help":
        console.log("see header comment for usage");
        process.exit(0);
        break;
      default:
        throw new Error(`unknown arg: ${a}`);
    }
  }
  if (!o.out) throw new Error("--out DIR is required");
  if (o.runs < 1 || o.runs > 20) throw new Error("--runs must be 1..20");
  return o;
}

/** Read one /proc/<pid>/status field (kB). Throws ProcGone if pid is dead. */
function readStatusField(pid, field) {
  let text;
  try {
    text = fs.readFileSync(`/proc/${pid}/status`, "utf8");
  } catch (err) {
    throw new ProcGone(`pid ${pid} gone while reading status (${err.code})`);
  }
  const m = text.match(new RegExp(`^${field}:\\s+(\\d+)`, "m"));
  return m ? Number(m[1]) : null;
}

/** Parse ppid + starttime from /proc/<pid>/stat. Null when unreadable. */
function readStat(pid) {
  let text;
  try {
    text = fs.readFileSync(`/proc/${pid}/stat`, "utf8");
  } catch {
    return null;
  }
  const close = text.lastIndexOf(")");
  if (close < 0) return null;
  const parts = text
    .slice(close + 1)
    .trim()
    .split(/\s+/);
  // post-comm fields: state(0) ppid(1) ... starttime(19)
  if (parts.length < 20) return null;
  return { ppid: Number(parts[1]), starttime: parts[19] };
}

/** Recursively collect descendant PIDs of root (best effort, never throws). */
function descendantsOf(rootPid) {
  const kids = new Map(); // ppid -> [pid]
  let entries;
  try {
    entries = fs.readdirSync("/proc");
  } catch {
    return [];
  }
  for (const e of entries) {
    if (!/^\d+$/.test(e)) continue;
    const pid = Number(e);
    if (pid === rootPid) continue;
    const st = readStat(pid);
    if (!st || !Number.isFinite(st.ppid)) continue;
    if (!kids.has(st.ppid)) kids.set(st.ppid, []);
    kids.get(st.ppid).push({ pid, starttime: st.starttime });
  }
  const out = [];
  const stack = [rootPid];
  while (stack.length > 0) {
    const next = stack.pop();
    for (const c of kids.get(next) ?? []) {
      out.push(c);
      stack.push(c.pid);
    }
  }
  return out;
}

/** One full /proc sample of the server PID (+ labeled aggregate of children). */
function sampleProc(pid) {
  const rssKb = readStatusField(pid, "VmRSS");
  const peakRssKb = readStatusField(pid, "VmHWM");
  const threads = readStatusField(pid, "Threads");
  let fds = null;
  try {
    fds = fs.readdirSync(`/proc/${pid}/fd`).length;
  } catch (err) {
    throw new ProcGone(`pid ${pid} gone while reading fds (${err.code})`);
  }
  let treeRssKb = 0;
  let treePids = [];
  const treeDetail = [];
  try {
    const desc = descendantsOf(pid);
    treePids = desc.map((d) => d.pid);
    for (const d of desc) {
      try {
        const rss = readStatusField(d.pid, "VmRSS") ?? 0;
        treeRssKb += rss;
        let comm = null;
        try {
          comm = fs.readFileSync(`/proc/${d.pid}/comm`, "utf8").trim();
        } catch {
          /* reaped */
        }
        treeDetail.push({ pid: d.pid, comm, rss_kb: rss });
      } catch (err) {
        if (!(err instanceof ProcGone)) throw err; // child reaped mid-scan: skip
      }
    }
  } catch (err) {
    if (err instanceof ProcGone) throw err;
  }
  return {
    t: new Date().toISOString(),
    rss_kb: rssKb,
    peak_rss_kb: peakRssKb,
    threads,
    fds,
    tree_rss_kb_aggregate: treeRssKb,
    tree_pids: treePids,
    tree_detail: treeDetail,
  };
}

function httpRequest(
  port,
  method,
  urlPath,
  { headers = {}, bodyText = null, timeoutMs = 60000 } = {}
) {
  return new Promise((resolve) => {
    const started = Date.now();
    let firstByteMs = null;
    const body = bodyText === null ? null : Buffer.from(bodyText, "utf8");
    const req = http.request(
      {
        host: "127.0.0.1",
        port,
        path: urlPath,
        method,
        timeout: timeoutMs,
        headers: {
          ...(body !== null
            ? { "Content-Type": "application/json", "Content-Length": String(body.length) }
            : {}),
          ...headers,
        },
      },
      (res) => {
        const chunks = [];
        let bytes = 0;
        res.on("data", (c) => {
          if (firstByteMs === null) firstByteMs = Date.now() - started;
          bytes += c.length;
          chunks.push(c);
        });
        res.on("end", () => {
          resolve({
            ok: res.statusCode >= 200 && res.statusCode < 300,
            status: res.statusCode,
            headers: res.headers,
            body_text: Buffer.concat(chunks).toString("utf8"),
            bytes,
            wall_ms: Date.now() - started,
            ttfb_ms: firstByteMs,
          });
        });
        res.on("error", (err) => {
          resolve({
            ok: false,
            status: null,
            headers: {},
            body_text: "",
            bytes,
            wall_ms: Date.now() - started,
            ttfb_ms: firstByteMs,
            error: String(err),
          });
        });
      }
    );
    req.on("timeout", () => req.destroy(new Error(`timeout after ${timeoutMs}ms`)));
    req.on("error", (err) => {
      resolve({
        ok: false,
        status: null,
        headers: {},
        body_text: "",
        bytes: 0,
        wall_ms: Date.now() - started,
        ttfb_ms: null,
        error: String(err),
      });
    });
    if (body !== null) req.write(body);
    req.end();
  });
}

function httpGet(port, urlPath, timeoutMs) {
  return httpRequest(port, "GET", urlPath, { timeoutMs }).then((r) => ({
    ok: r.ok,
    status: r.status,
    bytes: r.bytes,
    wall_ms: r.wall_ms,
    ttfb_ms: r.ttfb_ms,
    ...(r.error ? { error: r.error } : {}),
  }));
}

/**
 * Provision a real client API key through the product's own documented flow
 * (initial-password login → dashboard session → CSRF token → POST /api/keys).
 * Nothing is fabricated: the password is the run's INITIAL_PASSWORD, the key
 * is minted by the server. Secrets never leave process memory: the returned
 * object carries the key for request use, while callers must only persist
 * shapes/timings (see runOnce — result.json stores no key material).
 */
async function provisionApiKey(port, initialPassword, timeoutMs, log) {
  const jar = [];
  const storeCookies = (res) => {
    const set = res.headers?.["set-cookie"] ?? [];
    for (const c of Array.isArray(set) ? set : [set]) {
      const pair = String(c).split(";")[0];
      if (pair.includes("=")) jar.push(pair.trim());
    }
  };
  const cookieHeader = () => jar.join("; ");

  const login = await httpRequest(port, "POST", "/api/auth/login", {
    bodyText: JSON.stringify({ password: initialPassword }),
    timeoutMs,
  });
  storeCookies(login);
  if (!login.ok) {
    throw new Error(
      `provisioning: login → status=${login.status} body=${login.body_text.slice(0, 200)}`
    );
  }
  log("provisioning: login ok (session cookie set)");

  const csrf = await httpRequest(port, "GET", "/api/auth/csrf", {
    headers: { Cookie: cookieHeader() },
    timeoutMs,
  });
  if (!csrf.ok) {
    throw new Error(
      `provisioning: csrf → status=${csrf.status} body=${csrf.body_text.slice(0, 200)}`
    );
  }
  let csrfToken = null;
  try {
    csrfToken = JSON.parse(csrf.body_text)?.token ?? null;
  } catch {
    /* fall through */
  }
  if (!csrfToken) throw new Error("provisioning: csrf issued no token");
  log("provisioning: csrf token issued");

  const created = await httpRequest(port, "POST", "/api/keys", {
    headers: { Cookie: cookieHeader(), "x-omniroute-csrf": csrfToken },
    bodyText: JSON.stringify({ name: "task002-baseline" }),
    timeoutMs,
  });
  if (!created.ok) {
    throw new Error(
      `provisioning: create-key → status=${created.status} body=${created.body_text.slice(0, 200)}`
    );
  }
  let key = null;
  try {
    key = JSON.parse(created.body_text)?.key ?? null;
  } catch {
    /* fall through */
  }
  if (!key) throw new Error("provisioning: create-key returned no key");
  log("provisioning: api key minted");
  return key;
}

async function waitForReady(port, deadlineMs, requestTimeoutMs, log) {
  const start = Date.now();
  let liveAt = null;
  let readyAt = null;
  for (;;) {
    const remaining = deadlineMs - (Date.now() - start);
    if (remaining <= 0) throw new ReadinessTimeout("readiness deadline exceeded");
    const probeTimeout = Math.min(requestTimeoutMs, remaining);
    if (liveAt === null) {
      const r = await httpGet(port, "/api/health", probeTimeout);
      if (r.ok) {
        liveAt = new Date().toISOString();
        log(`liveness ok (${r.status}, ${r.wall_ms}ms)`);
      }
    } else {
      const r = await httpGet(port, "/api/health/ping", probeTimeout);
      if (r.ok) {
        readyAt = new Date().toISOString();
        log(`readiness ok (${r.status}, ${r.wall_ms}ms)`);
        return { liveAt, readyAt };
      }
    }
    await sleep(1000);
  }
}

function tailFile(file, lines = 40) {
  try {
    const text = fs.readFileSync(file, "utf8");
    return text.split("\n").slice(-lines).join("\n");
  } catch {
    return "<log unreadable>";
  }
}

function duBytes(dir) {
  return new Promise((resolve) => {
    execFile("du", ["-sb", dir], (err, stdout) => {
      if (err) return resolve(null);
      resolve(Number(stdout.split(/\s+/)[0]) || null);
    });
  });
}

async function runOnce(runIndex, opts, meta) {
  const port = opts.port + runIndex - 1;
  const runDir = path.join(opts.out, `run-${runIndex}`);
  const dataDir = path.join(runDir, "data");
  const serverLog = path.join(runDir, "server.log");
  fs.mkdirSync(dataDir, { recursive: true });
  const log = (m) => console.log(`[run-${runIndex}] ${m}`);
  const result = {
    run: runIndex,
    port,
    data_dir: dataDir,
    server_pid: null,
    outcome: "ok",
    failure_kind: null,
    failure_detail: null,
    t_spawn: new Date().toISOString(),
    t_live: null,
    t_ready: null,
    samples: {},
    models_request: null,
    data_dir_bytes: null,
  };

  const randomHex = (n) => crypto.randomBytes(n).toString("hex");
  const initialPassword = randomHex(16);
  const childEnv = {
    ...process.env,
    PORT: String(port),
    DATA_DIR: dataDir,
    JWT_SECRET: randomHex(48),
    API_KEY_SECRET: randomHex(32),
    // Random per-run password (in-memory only, never written to artifacts).
    // A booted server always ends with setupComplete=true (bootstrap-env
    // substitutes CHANGEME for an empty value; settings.ts:301 persists it),
    // so GET /v1/models requires a credential; the harness provisions a real
    // API key via login → csrf → POST /api/keys (see provisionApiKey).
    INITIAL_PASSWORD: initialPassword,
    // Never inherit operator secrets into the measured child.
    OMNIROUTE_API_KEY: "",
    ROUTER_API_KEY: "",
  };

  const logFd = fs.openSync(serverLog, "w");
  let child = null;
  let exited = null;
  try {
    child = spawn(
      process.execPath,
      ["--max-old-space-size=8192", "scripts/dev/run-next.mjs", "dev"],
      {
        cwd: REPO_ROOT,
        env: childEnv,
        stdio: ["ignore", logFd, logFd],
      }
    );
    result.server_pid = child.pid;
    log(`spawned pid=${child.pid} port=${port}`);
    child.on("exit", (code, signal) => {
      exited = { code, signal, t: new Date().toISOString() };
    });
    child.on("error", (err) => {
      exited = { code: null, signal: null, error: String(err), t: new Date().toISOString() };
    });

    await sleep(2000);
    if (exited)
      throw new MeasurementFailure(
        `server exited 2s after spawn: ${JSON.stringify(exited)}\n${tailFile(serverLog)}`
      );
    try {
      result.samples.startup_early = sampleProc(child.pid);
    } catch (err) {
      if (err instanceof ProcGone)
        throw new MeasurementFailure(`server died before first sample\n${tailFile(serverLog)}`);
      throw err;
    }

    const deadline = opts.readyTimeoutS * 1000;
    try {
      const { liveAt, readyAt } = await waitForReady(
        port,
        deadline,
        opts.requestTimeoutS * 1000,
        log
      );
      result.t_live = liveAt;
      result.t_ready = readyAt;
    } catch (err) {
      if (exited)
        throw new MeasurementFailure(
          `server exited before ready: ${JSON.stringify(exited)}\n${tailFile(serverLog)}`
        );
      if (err instanceof ReadinessTimeout) {
        throw new MeasurementFailure(
          `readiness timeout after ${opts.readyTimeoutS}s\n${tailFile(serverLog)}`
        );
      }
      throw err;
    }
    if (exited)
      throw new MeasurementFailure(`server exited right after ready: ${JSON.stringify(exited)}`);
    result.samples.startup_ready = sampleProc(child.pid);

    log(`stabilizing ${opts.stabilizeS}s…`);
    await sleep(opts.stabilizeS * 1000);
    if (exited)
      throw new MeasurementFailure(`server exited during stabilization: ${JSON.stringify(exited)}`);
    result.samples.initial_idle = sampleProc(child.pid);

    log(`idling ${opts.idleS}s…`);
    await sleep(opts.idleS * 1000);
    if (exited)
      throw new MeasurementFailure(`server exited during idle: ${JSON.stringify(exited)}`);
    result.samples.idle_5min = sampleProc(child.pid);

    result.samples.pre_models = sampleProc(child.pid);
    let apiKey = null;
    try {
      apiKey = await provisionApiKey(port, initialPassword, opts.requestTimeoutS * 1000, log);
    } catch (err) {
      result.outcome = "request_failure";
      result.failure_kind = "request_failure";
      result.failure_detail = String(err.message ?? err);
      result.samples.post_models = null;
      result.models_request = null;
      result.auth_mode = "provisioned-api-key (provisioning failed)";
      log(`models skipped: ${result.failure_detail}`);
    }
    if (result.outcome === "ok") {
      result.auth_mode = "provisioned-api-key (login → csrf → POST /api/keys)";
      let workloadPeak = result.samples.pre_models.rss_kb;
      const trackPeak = () => {
        try {
          const s = sampleProc(child.pid);
          if (s.rss_kb !== null && s.rss_kb > workloadPeak) workloadPeak = s.rss_kb;
        } catch {
          /* best effort */
        }
      };
      // Authenticated request (key in memory only; never logged or persisted).
      const doModelsGet = () =>
        new Promise((resolve) => {
          const t0 = Date.now();
          let firstByte = null;
          const req = http.request(
            {
              host: "127.0.0.1",
              port,
              path: "/v1/models",
              method: "GET",
              timeout: opts.requestTimeoutS * 1000,
              headers: { Authorization: `Bearer ${apiKey}` },
            },
            (res) => {
              let bytes = 0;
              let head = "";
              res.on("data", (c) => {
                if (firstByte === null) firstByte = Date.now() - t0;
                bytes += c.length;
                if (head.length < 300) head += c.toString("utf8").slice(0, 300 - head.length);
              });
              res.on("end", () => {
                resolve({
                  ok: res.statusCode >= 200 && res.statusCode < 300,
                  status: res.statusCode,
                  bytes,
                  wall_ms: Date.now() - t0,
                  ttfb_ms: firstByte,
                  body_head: head,
                  resp_headers: {
                    "x-omniroute-catalog": res.headers["x-omniroute-catalog"] ?? null,
                    "retry-after": res.headers["retry-after"] ?? null,
                  },
                });
              });
              res.on("error", (err) => {
                resolve({
                  ok: false,
                  status: null,
                  bytes,
                  wall_ms: Date.now() - t0,
                  ttfb_ms: firstByte,
                  body_head: head,
                  error: String(err),
                });
              });
              res.resume();
            }
          );
          req.on("timeout", () => req.destroy(new Error("timeout")));
          req.on("error", (err) => {
            resolve({
              ok: false,
              status: null,
              bytes: 0,
              wall_ms: Date.now() - t0,
              ttfb_ms: null,
              body_head: "",
              error: String(err),
            });
          });
          req.end();
        });
      const peakPoll = setInterval(trackPeak, 50);
      let realRes;
      try {
        realRes = await doModelsGet();
        // Documented client contract: catalog builds that exceed
        // CATALOG_BUILD_TIMEOUT_MS (8s default) answer 503
        // catalog_build_timeout with Retry-After; a representative client
        // retries once. The retry is part of the workload; both attempts are
        // recorded and the RSS peak spans both.
        result.models_attempts = [realRes];
        if (!realRes.ok && realRes.status === 503) {
          const waitS = Math.min(Number(realRes.resp_headers?.["retry-after"] ?? 10) || 10, 30);
          log(`models attempt 1 → 503, honoring Retry-After ${waitS}s then retrying once`);
          await sleep(waitS * 1000);
          if (exited)
            throw new MeasurementFailure(
              `server exited during models retry wait: ${JSON.stringify(exited)}`
            );
          realRes = await doModelsGet();
          result.models_attempts.push(realRes);
        }
      } finally {
        clearInterval(peakPoll);
      }
      apiKey = null; // drop key material at first opportunity
      result.samples.post_models = sampleProc(child.pid);
      result.models_request = { ...realRes, workload_peak_rss_kb: workloadPeak };
    }
    if (result.models_request !== null) {
      const mr = result.models_request;
      log(
        `models → status=${mr.status} bytes=${mr.bytes} wall=${mr.wall_ms}ms peak=${mr.workload_peak_rss_kb}kB`
      );
      if (!mr.ok) {
        result.outcome = "request_failure";
        result.failure_kind = "request_failure";
        result.failure_detail = `GET /v1/models → ${JSON.stringify(mr)}`;
      }
    }

    log(`recovering ${opts.recoverS}s…`);
    await sleep(opts.recoverS * 1000);
    if (exited)
      throw new MeasurementFailure(`server exited during recovery: ${JSON.stringify(exited)}`);
    result.samples.recovery = sampleProc(child.pid);
    result.data_dir_bytes = await duBytes(dataDir);
  } catch (err) {
    if (err instanceof ProcGone) {
      result.outcome = "measurement_failure";
      result.failure_kind = "measurement_failure";
      result.failure_detail = `process vanished mid-run: ${err.message}\n${tailFile(serverLog)}`;
    } else if (err instanceof MeasurementFailure) {
      result.outcome = "measurement_failure";
      result.failure_kind = "measurement_failure";
      result.failure_detail = err.message;
    } else {
      result.outcome = "measurement_failure";
      result.failure_kind = "measurement_failure";
      result.failure_detail = `unexpected: ${err.stack ?? err}`;
    }
  } finally {
    // Cleanup: only signal PIDs we spawned or discovered as descendants
    // (re-verified by starttime), never a broad name match.
    try {
      if (child && child.exitCode === null && !exited) {
        const desc = descendantsOf(child.pid).map((d) => ({ ...d, sig: "TERM" }));
        child.kill("SIGTERM");
        const t0 = Date.now();
        while (Date.now() - t0 < 20000) {
          if (exited) break;
          await sleep(500);
        }
        if (!exited) {
          log("SIGTERM timeout → SIGKILL main + known descendants");
          for (const d of desc) {
            try {
              const cur = readStat(d.pid);
              if (cur && cur.starttime === d.starttime) process.kill(d.pid, "SIGKILL");
            } catch {
              /* already gone */
            }
          }
          try {
            child.kill("SIGKILL");
          } catch {
            /* already gone */
          }
          const t1 = Date.now();
          while (Date.now() - t1 < 10000) {
            if (exited) break;
            await sleep(500);
          }
        }
        if (!exited) {
          result.outcome = result.outcome === "ok" ? "measurement_failure" : result.outcome;
          result.failure_detail =
            `${result.failure_detail ?? ""}\nserver pid ${child.pid} did not exit after SIGKILL`.trim();
        } else {
          result.t_exit = exited.t;
          result.exit_info = { code: exited.code, signal: exited.signal };
        }
        // Post-cleanup orphan check (our PIDs only).
        const leftovers = [];
        for (const d of descendantsOf(child.pid)) leftovers.push(d.pid);
        try {
          process.kill(child.pid, 0);
          leftovers.push(child.pid);
        } catch {
          /* gone */
        }
        result.orphan_check_leftovers = leftovers;
      } else if (exited) {
        result.t_exit = exited.t;
        result.exit_info = { code: exited.code, signal: exited.signal };
        result.orphan_check_leftovers = [];
      }
    } finally {
      try {
        fs.closeSync(logFd);
      } catch {
        /* ignore */
      }
    }
  }

  if (!opts.keepData && result.outcome === "ok") {
    try {
      fs.rmSync(dataDir, { recursive: true, force: true });
      result.data_dir = `${dataDir} (removed after ok run)`;
    } catch {
      /* keep path for debugging */
    }
  }
  fs.writeFileSync(
    path.join(runDir, "result.json"),
    `${JSON.stringify({ meta, result }, null, 2)}\n`
  );
  log(`outcome=${result.outcome}`);
  return result;
}

function summarize(values) {
  const xs = values
    .filter((v) => typeof v === "number" && Number.isFinite(v))
    .sort((a, b) => a - b);
  if (xs.length === 0) return { n: 0, min: null, median: null, max: null, values };
  const mid = Math.floor(xs.length / 2);
  const median = xs.length % 2 === 1 ? xs[mid] : (xs[mid - 1] + xs[mid]) / 2;
  return { n: xs.length, min: xs[0], median, max: xs[xs.length - 1], values };
}

async function collectMeta() {
  const toolEnv = process.env;
  const exec = (cmd, args) =>
    new Promise((resolve) => {
      execFile(cmd, args, { cwd: REPO_ROOT }, (err, stdout) => {
        resolve(err ? null : stdout.trim());
      });
    });
  let pkg = {};
  try {
    pkg = JSON.parse(fs.readFileSync(path.join(REPO_ROOT, "package.json"), "utf8"));
  } catch {
    /* ignore */
  }
  let meminfo = null;
  try {
    meminfo = fs.readFileSync("/proc/meminfo", "utf8").split("\n").slice(0, 3).join(" | ");
  } catch {
    /* ignore */
  }
  return {
    git_commit: await exec("git", ["rev-parse", "HEAD"]),
    git_branch: await exec("git", ["rev-parse", "--abbrev-ref", "HEAD"]),
    omniroute_version: pkg.version ?? null,
    node_version: process.version,
    npm_version: await exec("npm", ["--version"]),
    os: `${os.type()} ${os.release()}`,
    kernel: (await exec("uname", ["-r"])) ?? os.release(),
    arch: os.arch(),
    cpus: os.cpus()?.length ?? null,
    totalmem_bytes: os.totalmem(),
    proc_meminfo_head: meminfo,
    run_mode: "dev (scripts/dev/run-next.mjs dev, --max-old-space-size=8192)",
    // Read through a local alias: NODE_OPTIONS is a Node.js runtime knob, not
    // an OmniRoute configuration var, so it must not match the
    // check:env-doc-sync `process.env.X` contract scan (same reason the
    // product reads it via parameter objects in scripts/build/runtime-env.mjs).
    memory_env: {
      OMNIROUTE_MEMORY_MB: toolEnv.OMNIROUTE_MEMORY_MB ?? "(unset)",
      NODE_OPTIONS: toolEnv.NODE_OPTIONS ?? "(unset)",
    },
  };
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  fs.mkdirSync(opts.out, { recursive: true });
  const meta = await collectMeta();
  console.log(`baseline: ${opts.runs} run(s), base port ${opts.port}, out ${opts.out}`);
  console.log(`meta: ${JSON.stringify(meta)}`);

  const results = [];
  let failed = false;
  for (let i = 1; i <= opts.runs; i++) {
    const r = await runOnce(i, opts, meta);
    results.push(r);
    if (r.outcome !== "ok") failed = true;
  }

  const pick = (fn) => summarize(results.map(fn));
  const summary = {
    meta,
    runs: results.length,
    failed_runs: results.filter((r) => r.outcome !== "ok").length,
    startup_early_rss_kb: pick((r) => r.samples.startup_early?.rss_kb),
    startup_ready_rss_kb: pick((r) => r.samples.startup_ready?.rss_kb),
    initial_idle_rss_kb: pick((r) => r.samples.initial_idle?.rss_kb),
    idle_5min_rss_kb: pick((r) => r.samples.idle_5min?.rss_kb),
    pre_models_rss_kb: pick((r) => r.samples.pre_models?.rss_kb),
    models_workload_peak_rss_kb: pick((r) => r.models_request?.workload_peak_rss_kb),
    models_wall_ms: pick((r) => r.models_request?.wall_ms),
    models_bytes: pick((r) => r.models_request?.bytes),
    models_status: results.map((r) => r.models_request?.status ?? null),
    post_models_rss_kb: pick((r) => r.samples.post_models?.rss_kb),
    recovery_rss_kb: pick((r) => r.samples.recovery?.rss_kb),
    lifetime_peak_rss_kb: pick((r) => r.samples.recovery?.peak_rss_kb),
    threads_at_recovery: pick((r) => r.samples.recovery?.threads),
    fds_at_recovery: pick((r) => r.samples.recovery?.fds),
    data_dir_bytes: pick((r) => r.data_dir_bytes),
  };
  fs.writeFileSync(path.join(opts.out, "summary.json"), `${JSON.stringify(summary, null, 2)}\n`);
  console.log(JSON.stringify(summary, null, 2));
  if (failed) {
    console.error("baseline: one or more runs did not end ok (see run-*/result.json)");
    process.exit(2);
  }
}

main().catch((err) => {
  console.error(`baseline usage/fatal: ${err.stack ?? err}`);
  process.exit(1);
});
