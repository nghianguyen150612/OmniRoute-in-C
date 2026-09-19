/**
 * Compatibility harness — Stage 4: Node reference-backend adapter (Task 009).
 *
 * Deterministic lifecycle for one isolated Node/TypeScript reference
 * OmniRoute backend instance: prepare → start → waitReady → baseUrl →
 * health/describe → stop. This module owns every reference-specific detail
 * (Next.js entrypoint, `DATA_DIR`, port, secrets, login/CSRF provisioning,
 * PID tracking, descendant cleanup) so the generic Task 008 executor
 * (`native/compat/httpExecutor.ts`) stays backend-agnostic — that file is
 * untouched by this task and imports nothing from here.
 *
 * Stage boundary (see `docs/native-backend/COMPATIBILITY_HARNESS.md` §§3, 11):
 * the adapter supplies a running backend, its base URL, and runtime-resolved
 * setup; the generic executor performs bounded HTTP capture; later stages
 * compare. This module does NOT compare backends, normalize observations,
 * generate results, implement a native adapter, or emit C code.
 *
 * Process-management discipline reused from proven repository precedent
 * (never reimplemented from memory):
 * - Spawn identity: direct `spawn()` PID tracking, never name-match —
 *   `scripts/perf/baseline/reference-backend-baseline.mjs:479-496` (Task 002).
 * - Per-run isolated `DATA_DIR` via `mkdtemp`, removed on success —
 *   `reference-backend-baseline.mjs:437-439,762-769`.
 * - Random per-run secrets + operator-secret blanking (`JWT_SECRET` /
 *   `API_KEY_SECRET` random, `OMNIROUTE_API_KEY` / `ROUTER_API_KEY` blanked) —
 *   `reference-backend-baseline.mjs:457-474`.
 * - Two-phase readiness (liveness `GET /api/health`, then readiness
 *   `GET /api/health/ping`) with a finite deadline and premature-exit
 *   detection — `reference-backend-baseline.mjs:390-414`.
 * - Bounded log rings (200-line cap) + last-40-line tails on failure —
 *   `tests/integration/resilience-http-e2e.test.ts:213-222,235-279`.
 * - `SIGTERM` → grace → `SIGKILL`, descendant-scoped only, with
 *   starttime-verified descendant discovery (PID-reuse safe) —
 *   `reference-backend-baseline.mjs:698-760`.
 * - Free-port allocation via `net.createServer` + `listen(0)` —
 *   `tests/e2e/helpers/mockUpstreamServer.ts:19-37`.
 * - E2E boot env isolation (`DATA_DIR`, `PORT`, background-service disables) —
 *   `tests/integration/resilience-http-e2e.test.ts:183-208`.
 * - API-key provisioning through the product flow
 *   (login → CSRF → create-key, `x-omniroute-csrf`) —
 *   `reference-backend-baseline.mjs:328-388`.
 *
 * Deliberate deviations from the E2E precedent (documented, not accidental):
 * - The adapter boots `scripts/dev/run-next.mjs dev` (the same command
 *   `npm run dev` and the Task 002 baseline use), NOT
 *   `scripts/dev/run-next-playwright.mjs dev`: the playwright runner carries
 *   QA app-dir backup machinery and forces open-mode auth bypass, which would
 *   invalidate the harness's real auth semantics. The harness needs a random
 *   `INITIAL_PASSWORD` and the genuine login flow (Task 002 shape).
 * - `OMNIROUTE_E2E_BOOTSTRAP_MODE=open` is therefore NOT set.
 *
 * Residual port race (documented per Task 005 §12): the free port is reserved
 * by binding `127.0.0.1:0` and then released before the child binds it, so a
 * third process could theoretically claim it in between. The window is small
 * (prepare → start are adjacent), and a lost race surfaces deterministically
 * as `early-exit` (bind failure → child dies) or `readiness-timeout`, never
 * as traffic to the wrong server — readiness probes only accept the expected
 * health payloads on the expected port. No retry is implemented: the caller
 * prepares a fresh instance instead.
 *
 * Credential boundary: provisioning (`provisionApiKey()`) is an explicit
 * adapter operation because only the adapter knows the isolated backend's
 * `INITIAL_PASSWORD` and CSRF flow. It runs against the isolated backend
 * only, keeps the key in memory (never logged, never persisted), and returns
 * it through this narrow boundary for future stages to pass as Task 008
 * `resolvedHeaders`. Provider credentials are out of scope; no upstream
 * provider is ever contacted (background services are disabled in the child
 * env). If Task 005 had assigned credential resolution to a later stage,
 * this method would not exist — but §11 names provisioned keys as adapter
 * work, so the minimum lives here.
 *
 * Secret safety: generated secrets travel only in the child env block. They
 * never appear in `describe()`, `health()`, error details, or log tails —
 * exposed tails pass through `redactSecrets()`, which replaces every
 * generated secret with `[redacted]`.
 */

import { spawn, type ChildProcess } from "node:child_process";
import { execFileSync } from "node:child_process";
import { existsSync } from "node:fs";
import fs from "node:fs";
import net from "node:net";
import crypto from "node:crypto";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/** Operator port the harness must never bind, probe destructively, or kill. */
export const COMPAT_OPERATOR_PORT = 20128;

/** Reference entrypoint: the same command `npm run dev` uses (Task 002). */
export const COMPAT_REFERENCE_COMMAND = ["scripts/dev/run-next.mjs", "dev"] as const;

/** Two-phase readiness endpoints (Task 002 evidence). */
export const COMPAT_LIVENESS_PATH = "/api/health";
export const COMPAT_READINESS_PATH = "/api/health/ping";

/** Bounded in-memory log ring per stream (resilience E2E precedent: 200). */
export const DEFAULT_COMPAT_LOG_LINES = 200;

/** Failure-diagnostic tail (resilience E2E precedent: last 40 lines). */
export const COMPAT_DIAGNOSTIC_TAIL_LINES = 40;

/** Readiness deadline default (E2E-proven 120 s, §12). */
export const DEFAULT_COMPAT_READY_TIMEOUT_MS = 120_000;

/** Per-probe HTTP timeout default (resilience E2E precedent: 5 s). */
export const DEFAULT_COMPAT_PROBE_TIMEOUT_MS = 5_000;

/** Shutdown grace default (E2E precedent: 5 s, §12). */
export const DEFAULT_COMPAT_SHUTDOWN_GRACE_MS = 5_000;

/** Force-kill settle wait after SIGKILL (baseline precedent: 10 s). */
const FORCE_KILL_SETTLE_MS = 10_000;

/** Readiness poll interval (resilience E2E precedent: 500 ms). */
const READINESS_POLL_MS = 500;

/** API provisioning timeout (Task 008 `api` category anchor: 10 s). */
const PROVISION_TIMEOUT_MS = 10_000;

/** Prefix for adapter-owned temp dirs (ownership check on cleanup). */
const DATA_DIR_PREFIX = "omniroute-compat-ref-";

// ---------------------------------------------------------------------------
// Lifecycle states and errors
// ---------------------------------------------------------------------------

/** Minimum valid lifecycle states (Task 005 §11, made explicit). */
export type ReferenceAdapterState =
  "prepared" | "starting" | "ready" | "stopping" | "stopped" | "failed";

export type ReferenceAdapterErrorCode =
  | "not-prepared"
  | "already-started"
  | "not-started"
  | "not-ready"
  | "invalid-transition"
  | "operator-port-refused"
  | "spawn-failed"
  | "early-exit"
  | "readiness-timeout"
  | "provision-failed";

export class ReferenceAdapterError extends Error {
  readonly code: ReferenceAdapterErrorCode;

  constructor(code: ReferenceAdapterErrorCode, detail: string) {
    super(`${code}: ${detail}`);
    this.name = "ReferenceAdapterError";
    this.code = code;
  }
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

/** Test-only spawn override: run a fixture command instead of the reference. */
export interface CompatSpawnCommand {
  command: string;
  args: string[];
}

/** Test-only secret override: pin generated secrets to sentinel values. */
export interface CompatAdapterSecrets {
  jwtSecret?: string;
  apiKeySecret?: string;
  initialPassword?: string;
}

export interface ReferenceAdapterOptions {
  /** Explicit port (must not be 20128); default allocates a free loopback port. */
  port?: number;
  /** Overall readiness deadline (default 120 s per E2E evidence). */
  readyTimeoutMs?: number;
  /** Per-probe HTTP timeout (default 5 s). */
  probeTimeoutMs?: number;
  /** SIGTERM grace before SIGKILL escalation (default 5 s). */
  shutdownGraceMs?: number;
  /** In-memory log-ring cap per stream (default 200 lines). */
  logLines?: number;
  /** Keep the isolated DATA_DIR after stop (default false: remove on stop). */
  keepDataDir?: boolean;
  /** Repo root override (default: resolved from this module). */
  repoRoot?: string;
  /** Test seam: fixture child instead of the real reference server. */
  spawnCommand?: CompatSpawnCommand;
  /** Test seam: pin generated secrets (never used in production paths). */
  secrets?: CompatAdapterSecrets;
}

// ---------------------------------------------------------------------------
// Public result shapes (secret-free by construction)
// ---------------------------------------------------------------------------

export interface ReferenceAdapterExit {
  code: number | null;
  signal: NodeJS.Signals | null;
}

export interface ReferenceAdapterHealth {
  alive: boolean;
  pid: number | null;
  state: ReferenceAdapterState;
  exit: ReferenceAdapterExit | null;
  /** One bounded liveness probe; `unprobed` when no probe was attempted. */
  probe: "ok" | "unreachable" | "unprobed";
}

export interface ReferenceAdapterDescription {
  id: "reference";
  state: ReferenceAdapterState;
  baseUrl: string | null;
  pid: number | null;
  dataDir: string;
  revision?: string;
  mode: "dev";
}

export interface ReferenceAdapterStopSummary {
  pid: number | null;
  exit: ReferenceAdapterExit | null;
  dataDirRemoved: boolean;
  dataDirRemoval: "removed" | "kept-by-option" | "skipped-unowned" | "remove-failed";
  leftoverPids: number[];
}

// ---------------------------------------------------------------------------
// Pure helpers (exported for focused unit tests; no process I/O except where
// noted — port allocation and revision probing touch loopback/git only)
// ---------------------------------------------------------------------------

function defaultRepoRoot(): string {
  return path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
}

function resolveRepoRoot(override: string | undefined): string {
  const root = override ?? defaultRepoRoot();
  if (!existsSync(path.join(root, "package.json"))) {
    throw new ReferenceAdapterError(
      "not-prepared",
      "adapter repository root does not contain package.json"
    );
  }
  return root;
}

/** Allocate a free loopback port (mockUpstreamServer precedent). */
export function allocateCompatFreePort(): Promise<number> {
  return new Promise<number>((resolve, reject) => {
    const server = net.createServer();
    server.once("error", reject);
    server.listen(0, "127.0.0.1", () => {
      const address = server.address();
      if (address === null || typeof address === "string") {
        server.close();
        reject(new ReferenceAdapterError("not-prepared", "free port allocation failed"));
        return;
      }
      const { port } = address;
      server.close((err) => {
        if (err) reject(err);
        else resolve(port);
      });
    });
  });
}

export function validateCompatPort(port: number): void {
  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    throw new ReferenceAdapterError("not-prepared", "adapter port must be an integer in 1..65535");
  }
  if (port === COMPAT_OPERATOR_PORT) {
    throw new ReferenceAdapterError(
      "operator-port-refused",
      `adapter must never bind the operator port ${COMPAT_OPERATOR_PORT}`
    );
  }
}

function randomHex(n: number): string {
  return crypto.randomBytes(n).toString("hex");
}

export interface ResolvedAdapterSecrets {
  jwtSecret: string;
  apiKeySecret: string;
  initialPassword: string;
}

function resolveSecrets(override: CompatAdapterSecrets | undefined): ResolvedAdapterSecrets {
  return {
    jwtSecret: override?.jwtSecret ?? randomHex(48),
    apiKeySecret: override?.apiKeySecret ?? randomHex(32),
    initialPassword: override?.initialPassword ?? randomHex(16),
  };
}

/**
 * Build the deliberate child environment: inherit the process env (Node needs
 * PATH and friends), then pin every harness-significant key. Operator secret
 * vars are blanked so the isolated backend can never inherit them (Task 002
 * precedent). Background/token/local healthcheck services are disabled so the
 * isolated backend never contacts upstream providers during harness runs.
 */
export function buildReferenceChildEnv(
  parent: NodeJS.ProcessEnv,
  args: { port: number; dataDir: string; secrets: ResolvedAdapterSecrets }
): Record<string, string | undefined> {
  return {
    ...parent,
    PORT: String(args.port),
    DASHBOARD_PORT: String(args.port),
    API_PORT: String(args.port),
    HOST: "127.0.0.1",
    DATA_DIR: args.dataDir,
    JWT_SECRET: args.secrets.jwtSecret,
    API_KEY_SECRET: args.secrets.apiKeySecret,
    INITIAL_PASSWORD: args.secrets.initialPassword,
    OMNIROUTE_API_KEY: "",
    ROUTER_API_KEY: "",
    NEXT_TELEMETRY_DISABLED: "1",
    DISABLE_SQLITE_AUTO_BACKUP: "true",
    OMNIROUTE_DISABLE_BACKGROUND_SERVICES: "true",
    OMNIROUTE_DISABLE_TOKEN_HEALTHCHECK: "true",
    OMNIROUTE_DISABLE_LOCAL_HEALTHCHECK: "true",
    OMNIROUTE_HIDE_HEALTHCHECK_LOGS: "true",
  };
}

/** Best-effort reference revision for `describe()`; undefined when unavailable. */
function probeRevision(repoRoot: string): string | undefined {
  try {
    const out = execFileSync("git", ["rev-parse", "--short", "HEAD"], {
      cwd: repoRoot,
      timeout: 5_000,
      stdio: ["ignore", "pipe", "ignore"],
    });
    const revision = String(out).trim();
    return revision.length > 0 ? revision : undefined;
  } catch {
    return undefined;
  }
}

// --- /proc helpers (baseline precedent; Linux only, never throw) ---

interface ProcStat {
  ppid: number;
  starttime: string;
}

function readProcStat(pid: number): ProcStat | null {
  let text: string;
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
  if (parts.length < 20) return null;
  const ppid = Number(parts[1]);
  if (!Number.isFinite(ppid)) return null;
  return { ppid, starttime: parts[19] };
}

/**
 * Starttime-verified descendants of `rootPid` (baseline `descendantsOf`
 * precedent). Returns [] where /proc is unavailable. Never throws.
 */
export function compatDescendantsOf(rootPid: number): Array<{ pid: number; starttime: string }> {
  let entries: string[];
  try {
    entries = fs.readdirSync("/proc");
  } catch {
    return [];
  }
  const kids = new Map<number, Array<{ pid: number; starttime: string }>>();
  for (const entry of entries) {
    if (!/^\d+$/.test(entry)) continue;
    const pid = Number(entry);
    if (pid === rootPid) continue;
    const stat = readProcStat(pid);
    if (stat === null) continue;
    const list = kids.get(stat.ppid) ?? [];
    list.push({ pid, starttime: stat.starttime });
    kids.set(stat.ppid, list);
  }
  const out: Array<{ pid: number; starttime: string }> = [];
  const stack = [rootPid];
  while (stack.length > 0) {
    const next = stack.pop() as number;
    for (const child of kids.get(next) ?? []) {
      out.push(child);
      stack.push(child.pid);
    }
  }
  return out;
}

function isProcessAlive(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
}

const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));

// ---------------------------------------------------------------------------
// ReferenceBackend: stateful single-instance lifecycle owner
// ---------------------------------------------------------------------------

export class ReferenceBackend {
  private state: ReferenceAdapterState = "prepared";
  private readonly port: number;
  private readonly dataDir: string;
  private readonly repoRoot: string;
  private readonly secrets: ResolvedAdapterSecrets;
  private readonly childEnv: Record<string, string | undefined>;
  private readonly readyTimeoutMs: number;
  private readonly probeTimeoutMs: number;
  private readonly shutdownGraceMs: number;
  private readonly logLineCap: number;
  private readonly keepDataDir: boolean;
  private readonly spawnCommand: CompatSpawnCommand;
  private readonly revision: string | undefined;

  private child: ChildProcess | null = null;
  private pid: number | null = null;
  private exitInfo: ReferenceAdapterExit | null = null;
  private spawnError: string | null = null;
  private readonly stdoutLines: string[] = [];
  private readonly stderrLines: string[] = [];
  private stopSummary: ReferenceAdapterStopSummary | null = null;
  private stopPromise: Promise<ReferenceAdapterStopSummary> | null = null;

  private constructor(init: {
    port: number;
    dataDir: string;
    repoRoot: string;
    secrets: ResolvedAdapterSecrets;
    childEnv: Record<string, string | undefined>;
    readyTimeoutMs: number;
    probeTimeoutMs: number;
    shutdownGraceMs: number;
    logLineCap: number;
    keepDataDir: boolean;
    spawnCommand: CompatSpawnCommand;
    revision: string | undefined;
  }) {
    this.port = init.port;
    this.dataDir = init.dataDir;
    this.repoRoot = init.repoRoot;
    this.secrets = init.secrets;
    this.childEnv = init.childEnv;
    this.readyTimeoutMs = init.readyTimeoutMs;
    this.probeTimeoutMs = init.probeTimeoutMs;
    this.shutdownGraceMs = init.shutdownGraceMs;
    this.logLineCap = init.logLineCap;
    this.keepDataDir = init.keepDataDir;
    this.spawnCommand = init.spawnCommand;
    this.revision = init.revision;
  }

  /**
   * Prepare an isolated instance: unique temp DATA_DIR, loopback port,
   * per-run secrets, deliberate child env. No process is started.
   */
  static async prepare(options: ReferenceAdapterOptions = {}): Promise<ReferenceBackend> {
    const repoRoot = resolveRepoRoot(options.repoRoot);
    // Validate everything that can fail BEFORE creating the temp DATA_DIR so
    // a rejected prepare never leaks an adapter-owned directory.
    const port = options.port ?? (await allocateCompatFreePort());
    validateCompatPort(port);
    const readyTimeoutMs = options.readyTimeoutMs ?? DEFAULT_COMPAT_READY_TIMEOUT_MS;
    const probeTimeoutMs = options.probeTimeoutMs ?? DEFAULT_COMPAT_PROBE_TIMEOUT_MS;
    const shutdownGraceMs = options.shutdownGraceMs ?? DEFAULT_COMPAT_SHUTDOWN_GRACE_MS;
    const logLineCap = options.logLines ?? DEFAULT_COMPAT_LOG_LINES;
    for (const [label, value] of [
      ["readyTimeoutMs", readyTimeoutMs],
      ["probeTimeoutMs", probeTimeoutMs],
      ["shutdownGraceMs", shutdownGraceMs],
      ["logLines", logLineCap],
    ] as const) {
      if (!Number.isFinite(value) || value <= 0) {
        throw new ReferenceAdapterError(
          "not-prepared",
          `adapter option ${label} must be a finite positive count`
        );
      }
    }
    const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), DATA_DIR_PREFIX));
    const secrets = resolveSecrets(options.secrets);
    const childEnv = buildReferenceChildEnv(process.env, { port, dataDir, secrets });
    const spawnCommand: CompatSpawnCommand = options.spawnCommand ?? {
      command: process.execPath,
      args: [...COMPAT_REFERENCE_COMMAND],
    };
    return new ReferenceBackend({
      port,
      dataDir,
      repoRoot,
      secrets,
      childEnv,
      readyTimeoutMs: Math.floor(readyTimeoutMs),
      probeTimeoutMs: Math.floor(probeTimeoutMs),
      shutdownGraceMs: Math.floor(shutdownGraceMs),
      logLineCap: Math.floor(logLineCap),
      keepDataDir: options.keepDataDir ?? false,
      spawnCommand,
      revision: probeRevision(repoRoot),
    });
  }

  /** Current lifecycle state (explicit; transitions validated per method). */
  getState(): ReferenceAdapterState {
    return this.state;
  }

  /** Usable base URL — only once `ready` (contract requires readiness). */
  baseUrl(): string {
    if (this.state !== "ready") {
      throw new ReferenceAdapterError(
        "not-ready",
        `adapter base URL requires the ready state (current: ${this.state})`
      );
    }
    return `http://127.0.0.1:${this.port}`;
  }

  /** Start the owned child process. Exactly once per instance. */
  start(): void {
    if (this.state === "ready" || this.state === "starting") {
      throw new ReferenceAdapterError(
        "already-started",
        `adapter is already ${this.state}; a fresh instance is required for another backend`
      );
    }
    if (this.state !== "prepared") {
      throw new ReferenceAdapterError(
        "invalid-transition",
        `adapter cannot start from state ${this.state}`
      );
    }
    let child: ChildProcess;
    try {
      child = spawn(this.spawnCommand.command, this.spawnCommand.args, {
        cwd: this.repoRoot,
        env: this.childEnv,
        stdio: ["ignore", "pipe", "pipe"],
      });
    } catch (error) {
      this.state = "failed";
      throw new ReferenceAdapterError(
        "spawn-failed",
        `reference backend spawn failed (${error instanceof Error ? error.name : "unknown-error"})`
      );
    }
    if (child.pid === undefined) {
      this.state = "failed";
      throw new ReferenceAdapterError("spawn-failed", "reference backend spawn returned no PID");
    }
    this.child = child;
    this.pid = child.pid;
    child.on("exit", (code, signal) => {
      this.exitInfo = { code, signal };
    });
    child.on("error", (error) => {
      this.spawnError = error instanceof Error ? error.name : "unknown-error";
      if (this.exitInfo === null) this.exitInfo = { code: null, signal: null };
    });
    child.stdout?.on("data", (chunk: Buffer) => this.pushLines(this.stdoutLines, String(chunk)));
    child.stderr?.on("data", (chunk: Buffer) => this.pushLines(this.stderrLines, String(chunk)));
    this.state = "starting";
  }

  /**
   * Bounded two-phase readiness: liveness (`GET /api/health`) then readiness
   * (`GET /api/health/ping`). Distinguishes early child exit from deadline
   * expiry. Requires `start()` first.
   */
  async waitReady(): Promise<string> {
    if (this.state === "prepared") {
      throw new ReferenceAdapterError(
        "not-started",
        "adapter readiness requires start() before waitReady()"
      );
    }
    if (this.state === "ready") return this.baseUrl();
    if (this.state !== "starting") {
      throw new ReferenceAdapterError(
        "invalid-transition",
        `adapter cannot wait for readiness from state ${this.state}`
      );
    }
    const startedAt = Date.now();
    let live = false;
    for (;;) {
      if (this.exitInfo !== null || this.child?.exitCode !== null) {
        this.state = "failed";
        const exit = this.exitInfo ?? { code: null as number | null, signal: null };
        throw new ReferenceAdapterError(
          "early-exit",
          `reference backend exited before ready (code=${String(exit.code)}, signal=${String(exit.signal)}; spawn-error=${this.spawnError ?? "none"})` +
            `\n--- stdout tail ---\n${this.redactedTail(this.stdoutLines)}` +
            `\n--- stderr tail ---\n${this.redactedTail(this.stderrLines)}`
        );
      }
      const elapsed = Date.now() - startedAt;
      const remaining = this.readyTimeoutMs - elapsed;
      if (remaining <= 0) {
        this.state = "failed";
        throw new ReferenceAdapterError(
          "readiness-timeout",
          `reference backend not ready within ${this.readyTimeoutMs} ms` +
            `\n--- stdout tail ---\n${this.redactedTail(this.stdoutLines)}` +
            `\n--- stderr tail ---\n${this.redactedTail(this.stderrLines)}`
        );
      }
      const probeTimeout = Math.min(this.probeTimeoutMs, remaining);
      try {
        const path = live ? COMPAT_READINESS_PATH : COMPAT_LIVENESS_PATH;
        const response = await fetch(`http://127.0.0.1:${this.port}${path}`, {
          signal: AbortSignal.timeout(probeTimeout),
        });
        if (response.ok) {
          try {
            await response.arrayBuffer();
          } catch {
            // draining the probe body is best-effort; status is the signal.
          }
          if (live) {
            this.state = "ready";
            return this.baseUrl();
          }
          live = true;
        }
      } catch {
        // Unreachable mid-startup is the normal pre-ready condition.
      }
      await sleep(Math.min(READINESS_POLL_MS, remaining));
    }
  }

  /**
   * Bounded aliveness check: owned-process existence plus a single liveness
   * probe when `ready`. Never throws for lifecycle reasons; never polls in
   * the background. Post-stop reports `alive: false`.
   */
  async health(): Promise<ReferenceAdapterHealth> {
    const processAlive =
      this.pid !== null && this.child?.exitCode === null && isProcessAlive(this.pid);
    if (this.state !== "ready" || !processAlive) {
      return {
        alive: false,
        pid: this.pid,
        state: this.state,
        exit: this.exitInfo,
        probe: "unprobed",
      };
    }
    try {
      const response = await fetch(`http://127.0.0.1:${this.port}${COMPAT_LIVENESS_PATH}`, {
        signal: AbortSignal.timeout(Math.min(this.probeTimeoutMs, DEFAULT_COMPAT_PROBE_TIMEOUT_MS)),
      });
      try {
        await response.arrayBuffer();
      } catch {
        // draining the probe body is best-effort; status is the signal.
      }
      return {
        alive: response.ok,
        pid: this.pid,
        state: this.state,
        exit: this.exitInfo,
        probe: response.ok ? "ok" : "unreachable",
      };
    } catch {
      return {
        alive: false,
        pid: this.pid,
        state: this.state,
        exit: this.exitInfo,
        probe: "unreachable",
      };
    }
  }

  /**
   * Non-secret metadata for future harness artifacts. Fixed key allowlist —
   * secret env values, API keys, cookies, and CSRF tokens can never appear.
   */
  describe(): ReferenceAdapterDescription {
    const description: ReferenceAdapterDescription = {
      id: "reference",
      state: this.state,
      baseUrl: this.state === "ready" ? `http://127.0.0.1:${this.port}` : null,
      pid: this.pid,
      dataDir: this.dataDir,
      mode: "dev",
    };
    if (this.revision !== undefined) description.revision = this.revision;
    return description;
  }

  /**
   * Provision a real client API key through the product flow
   * (login → CSRF → create-key) against the isolated backend only. Requires
   * `ready`. The key is returned in memory for future `resolvedHeaders` use;
   * it is never logged or persisted. Failures carry fixed templates with
   * statuses only — response bodies may carry key material, so they are never
   * copied into diagnostics.
   */
  async provisionApiKey(label = "compat-harness"): Promise<string> {
    if (this.state !== "ready") {
      throw new ReferenceAdapterError(
        "not-ready",
        `API key provisioning requires the ready state (current: ${this.state})`
      );
    }
    const origin = `http://127.0.0.1:${this.port}`;
    const jar: string[] = [];
    const provisionFetch = async (
      targetPath: string,
      init: { method: string; headers?: Record<string, string>; body?: string },
      step: string
    ): Promise<{ status: number; text: string; headers: Headers }> => {
      let response: Response;
      try {
        response = await fetch(`${origin}${targetPath}`, {
          method: init.method,
          headers: {
            ...(init.body !== undefined
              ? {
                  "content-type": "application/json",
                  "content-length": String(Buffer.byteLength(init.body)),
                }
              : {}),
            ...(init.headers ?? {}),
          },
          body: init.body,
          signal: AbortSignal.timeout(PROVISION_TIMEOUT_MS),
        });
      } catch (error) {
        throw new ReferenceAdapterError(
          "provision-failed",
          `provisioning ${step} request failed (${error instanceof Error ? error.name : "unknown-error"})`
        );
      }
      let text = "";
      try {
        text = await response.text();
      } catch {
        text = "";
      }
      if (!response.ok) {
        throw new ReferenceAdapterError(
          "provision-failed",
          `provisioning ${step} answered HTTP ${response.status}`
        );
      }
      return { status: response.status, text, headers: response.headers };
    };

    const login = await provisionFetch(
      "/api/auth/login",
      { method: "POST", body: JSON.stringify({ password: this.secrets.initialPassword }) },
      "login"
    );
    const setCookie = readSetCookie(login.headers);
    for (const entry of setCookie) {
      const pair = entry.split(";")[0]?.trim();
      if (pair !== undefined && pair.includes("=")) jar.push(pair);
    }
    const csrf = await provisionFetch(
      "/api/auth/csrf",
      { method: "GET", headers: jar.length > 0 ? { cookie: jar.join("; ") } : {} },
      "csrf"
    );
    let csrfToken: string | null = null;
    try {
      csrfToken = (JSON.parse(csrf.text) as { token?: unknown }).token as string | null;
    } catch {
      csrfToken = null;
    }
    if (typeof csrfToken !== "string" || csrfToken.length === 0) {
      throw new ReferenceAdapterError("provision-failed", "provisioning csrf issued no token");
    }
    const created = await provisionFetch(
      "/api/keys",
      {
        method: "POST",
        headers: {
          ...(jar.length > 0 ? { cookie: jar.join("; ") } : {}),
          "x-omniroute-csrf": csrfToken,
        },
        body: JSON.stringify({ name: label }),
      },
      "create-key"
    );
    let apiKey: string | null = null;
    try {
      apiKey = (JSON.parse(created.text) as { key?: unknown }).key as string | null;
    } catch {
      apiKey = null;
    }
    if (typeof apiKey !== "string" || apiKey.length === 0) {
      throw new ReferenceAdapterError(
        "provision-failed",
        "provisioning create-key returned no key"
      );
    }
    return apiKey;
  }

  /**
   * Bounded shutdown, safe from cleanup paths: SIGTERM the owned child only,
   * wait the grace period, then SIGKILL the owned child plus
   * starttime-verified descendants. Verifies termination, then removes the
   * adapter-owned temp DATA_DIR unless `keepDataDir` is set (or ownership is
   * uncertain, in which case the path is left and reported). Idempotent:
   * repeated calls return the same summary. Never signals PIDs the adapter
   * did not spawn, never touches the operator port.
   */
  async stop(): Promise<ReferenceAdapterStopSummary> {
    if (this.stopPromise !== null) return this.stopPromise;
    this.stopPromise = this.stopOnce();
    return this.stopPromise;
  }

  private async stopOnce(): Promise<ReferenceAdapterStopSummary> {
    if (this.state !== "stopped" && this.state !== "failed") this.state = "stopping";
    const child = this.child;
    const pid = this.pid;
    try {
      if (child !== null && pid !== null && child.exitCode === null && isProcessAlive(pid)) {
        const descendants = compatDescendantsOf(pid);
        try {
          child.kill("SIGTERM");
        } catch {
          // already gone; verification below confirms.
        }
        const termStart = Date.now();
        while (Date.now() - termStart < this.shutdownGraceMs) {
          if (child.exitCode !== null || !isProcessAlive(pid)) break;
          await sleep(100);
        }
        if (child.exitCode === null && isProcessAlive(pid)) {
          for (const descendant of descendants) {
            try {
              const current = readProcStat(descendant.pid);
              if (current !== null && current.starttime === descendant.starttime) {
                process.kill(descendant.pid, "SIGKILL");
              }
            } catch {
              // already gone.
            }
          }
          try {
            child.kill("SIGKILL");
          } catch {
            // already gone.
          }
          const killStart = Date.now();
          while (Date.now() - killStart < FORCE_KILL_SETTLE_MS) {
            if (child.exitCode !== null || !isProcessAlive(pid)) break;
            await sleep(100);
          }
        }
      }
    } finally {
      const summary = this.finishStop();
      this.state = "stopped";
      this.stopSummary = summary;
      return summary;
    }
  }

  private finishStop(): ReferenceAdapterStopSummary {
    const leftoverPids: number[] = [];
    if (this.pid !== null) {
      for (const descendant of compatDescendantsOf(this.pid)) leftoverPids.push(descendant.pid);
      if (isProcessAlive(this.pid)) leftoverPids.push(this.pid);
    }
    let dataDirRemoved = false;
    let dataDirRemoval: ReferenceAdapterStopSummary["dataDirRemoval"] = "removed";
    if (this.keepDataDir) {
      dataDirRemoval = "kept-by-option";
    } else if (!isOwnedDataDir(this.dataDir)) {
      dataDirRemoval = "skipped-unowned";
    } else {
      try {
        fs.rmSync(this.dataDir, { recursive: true, force: true });
        dataDirRemoved = true;
      } catch {
        dataDirRemoval = "remove-failed";
      }
    }
    return { pid: this.pid, exit: this.exitInfo, dataDirRemoved, dataDirRemoval, leftoverPids };
  }

  private pushLines(ring: string[], chunk: string): void {
    for (const line of chunk.split(/\r?\n/)) {
      if (line.length === 0) continue;
      ring.push(line);
      if (ring.length > this.logLineCap) ring.splice(0, ring.length - this.logLineCap);
    }
  }

  /** Redact every generated secret from a diagnostic tail before exposure. */
  private redactedTail(ring: string[]): string {
    return redactSecrets(ring.slice(-COMPAT_DIAGNOSTIC_TAIL_LINES).join("\n"), this.secrets);
  }

  /** Current ring sizes (for bounded-log tests; counts only, never content). */
  logSizes(): { stdout: number; stderr: number; cap: number } {
    return {
      stdout: this.stdoutLines.length,
      stderr: this.stderrLines.length,
      cap: this.logLineCap,
    };
  }
}

/** Adapter-owned temp dirs live under the OS temp dir with our prefix. */
function isOwnedDataDir(dataDir: string): boolean {
  const tmp = os.tmpdir();
  return (
    dataDir.startsWith(`${tmp}${path.sep}`) && path.basename(dataDir).startsWith(DATA_DIR_PREFIX)
  );
}

/** Split `set-cookie` where the runtime exposes it (undici-only API). */
function readSetCookie(headers: Headers): string[] {
  const candidate = (headers as unknown as { getSetCookie?: unknown }).getSetCookie;
  if (typeof candidate !== "function") return [];
  try {
    const values = (candidate as () => unknown).call(headers);
    return Array.isArray(values) ? values.map(String) : [];
  } catch {
    return [];
  }
}

/**
 * Replace every generated secret occurrence with `[redacted]`. Secrets
 * shorter than 8 chars are ignored so redaction can never blank ordinary
 * diagnostic text.
 */
export function redactSecrets(text: string, secrets: ResolvedAdapterSecrets): string {
  let out = text;
  for (const secret of [secrets.jwtSecret, secrets.apiKeySecret, secrets.initialPassword]) {
    if (secret.length >= 8) out = out.split(secret).join("[redacted]");
  }
  return out;
}
