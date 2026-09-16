# OmniRoute Native C Backend — Migration Plan (Task 001 Design)

> Audit/design only — no C code is written in Task 001. The TypeScript
> backend remains the reference implementation until parity is proven slice
> by slice. Frontend behavior must not change.
>
> Constraints (from brief): full parity mandatory; subsystem-by-subsystem
> migration; native backend eventually runs without Node/V8; memory beats
> simplicity; streaming + bounded memory preferred; iOS 12 arm64 must stay
> possible; Linux stays the first-class dev/test platform; no hand-written
> asm; SIMD/intrinsics only after profiling.

## 0. Guiding decisions

1. **Strangler, not rewrite.** The C backend grows beside the TS backend
   behind a compatibility harness (§5). Slices cut over only when the
   harness + existing tests pass. `origin/main` never resets to upstream;
   upstream stays the behavioral reference (fork model).
2. **Vertical slices, not horizontal layers.** Each slice is a user-visible
   route family with storage, auth, and tests — never "all JSON parsing"
   alone. First slice: `GET /v1/models` (§6).
3. **Byte budgets before data structures.** Every slice declares idle RSS,
   per-request, per-stream, and cache caps up front (MEMORY_MODEL §5) and
   the harness enforces them.
4. **Generated parity, not hand parity.** Provider/model/strategy tables are
   generated from the TS registry at build time (§4.5) so 358 providers
   cannot silently shrink to a "lite" subset.
5. **iOS 12 veto at design review.** Any new dependency (TLS, SQLite, JSON,
   regex, crypto) must compile for arm64 + iOS 12 SDK and run under ~1 GiB
   shared RAM, or it does not land (§8).

---

## 1. Proposed `native/` structure (adjusted from brief by repo evidence)

The brief's skeleton is sound, but the audit suggests four changes:
(a) split `api/` into `api_v1/` (OpenAI-compatible, hot path) vs `api_mgmt/`
(management, cold path) so the hot path stays lean; (b) add `compat/`
(harness + generated tables) and `codegen/` (registry generators) as
first-class dirs; (c) add `admission/` (backpressure is a cross-cutting
runtime, not an HTTP detail); (d) keep `vendor/` minimal and pinned —
most TS optionals (ONNX, Playwright, sharp, Qdrant server) are **not**
ported, only interfaced or stubbed.

```
native/
  README.md                  # build matrix, budgets, slice status (points here)
  CMakeLists.txt             # Linux-first build; iOS toolchain file under platform/
  include/omniroute/         # public headers (arena, buf, http, json, sse, db, cfg)
  src/
    core/                    # arena, slab/pool, buf/ring, slice/str, err, log, time, rand
    http/                    # server, router, cors, headers, query, body-limits, keepalive
    admission/               # ingest budgets, concurrency caps, shed (503/429), metrics
    json/                    # streaming-capable minimal parser + serializer (no DOM dup)
    api_v1/                  # hot path: models, chat/completions, responses, embeddings…
    api_mgmt/                # cold path: keys/combos/providers/settings/usage/… (slice order §6)
    providers/               # static metadata (generated) + per-family dispatch helpers
    routing/                 # strategies, combo DAG, auto-score, fusion, pipeline, quotas
    auth/                    # api-key validation, scopes, session/JWT verify, CORS policy data
    crypto/                  # AES-256-GCM (SQLite-compat), SHA-256, scrypt-verify, CSPRNG
    storage/                 # SQLite wrapper ( WAL, pragmas, migrations runner, domain DAOs)
    streaming/               # SSE framer/parser, WS bridge, responses transformer, heartbeat
    compression/             # lite/standard core only at first; rtk/onnx explicitly later
    mcp/                     # server core + transports (stdio/HTTP-SSE) + tool dispatch
    a2a/                     # tasks, 6 skills, agent-card JSON
    memory/                  # FTS5 access + embedding-client + qdrant-client (no server)
    platform/
      linux/                 # epoll/kqueue-abstracted poller impl, cgroup/PSI probes, FD utils
      ios/                   # iOS 12 shims (mmap limits, no fork, keychain notes, bg rules)
      posix/                 # shared poller/time/signal/tempfile
  codegen/                   # TS-registry → C table generators (run at build, checked in)
  compat/                    # harness client + comparators + corpus + expected vectors
  tests/                     # C unit tests (one dir per slice) + harness cases
  bench/                     # RSS/HWM/threads/FD micro-benches mirroring MEMORY_MODEL §4
  vendor/                    # ONLY: sqlite amalgamation + (later) single-header deps w/ licenses
  platform-toolchain/
    ios12-arm64.cmake        # iOS 12 SDK, arm64, min-version, stack-size, bitcode-off notes
```

What is deliberately **absent** from `native/` v1: dashboard (preserved TS/Next
frontend talks to C over the same HTTP contract), Electron, browser pool,
ONNX/LLMLingua, sharp/ffmpeg, Qdrant server, Redis (stub the cache interface;
file/SQLite-backed fallback), cloud tunnels/MITM/TProxy (documented stubs
returning 501 `not_supported_on_this_build` with the TS error shape).

---

## 2. C backend design principles (investigated, not yet implemented)

### 2.1 Ownership model

- **Request-scoped arenas.** One arena per request (plus one per stream leg);
  freed on completion/cancel. No per-request `malloc` churn for headers,
  JSON slices, translator scratch. Global state never borrows from arenas.
- **Slab/pool allocators** for fixed-size hot objects (headers, frames,
  connections, timers). Pools sized by config, never grown silently.
- **Bounded buffers + reuse.** Fixed ring/chain buffers per direction
  (16–64 KiB, mirroring the 16k HWM); connection buffers pooled, not
  per-request allocated. Large payloads spill to tempfiles past threshold
  (align with the 8 MiB ingest floor) instead of growing RAM.
- **Zero-copy/slice parsing where practical.** HTTP + JSON parsers emit
  slices into the owned body buffer; translator works on slices; only log
  paths and persistence make bounded copies.
- **Streaming both directions.** Upstream→C→downstream pumped chunk by chunk;
  SSE frames flushed on chunk boundaries; Responses transform applied
  incrementally (snapshot path handled standalone, per the PII learning).
- **mmap for suitable read-only data** (generated provider tables,
  compression rules, i18n if ever needed server-side) with private mapping
  semantics, size 0/disabled on iOS defeature builds.
- **Strict cache budgets.** One global table: per-cache byte cap + idle TTL
  - `release_when_idle`; heavy caches (catalog, embeddings) explicitly
    releasable. No unbounded `Map` growth: every map has count + byte caps.
- **Bounded pools, low threads.** Fixed accept + worker pool (CPUs, min 2),
  configurable stacks, bounded upstream connection pool, bounded concurrent
  streams × buffer. No per-request threads, no `fork` on iOS.
- **Lazy init + idle release.** Compression engines, embedding clients,
  vector/Qdrant clients, media paths init on first use and release after
  idle timeout. Core boots without them (needed for the <8 MiB idle target).
- **SQLite discipline.** Small `cache_size` (e.g. `-2048`), `mmap_size` 0 on
  iOS, `synchronous=NORMAL`, `busy_timeout` small, checkpoint-on-idle,
  single writer discipline, prepared-statement cache with cap.
- **No duplicated JSON trees.** One owned parse per request; fan-out (combo
  targets, fusion panel) borrows or re-serializes from slices; token
  estimation counts from the same pass.

### 2.2 What we explicitly defer

Hand-written assembly (never per brief); SIMD/intrinsics (only with a
profiled hot path + A/B numbers); HTTP/3; Qdrant-server-in-C; ONNX-in-C;
browser automation; JS/WASM extension surface.

---

## 3. Compatibility strategy (how TS and C coexist)

1. **Same port contract, different port.** C binds `20129` (configurable)
   while TS keeps `20128`. The harness sends each case to both (§5).
2. **Shared `DATA_DIR` discipline — never shared live.** Each backend gets
   its own `DATA_DIR` copy (seeded from the same fixture) so SQLite WAL
   writers never contend. Persistence effects are compared by dumping
   (`sqlite3 .dump` of listed tables + artifact files), not by sharing files.
3. **Frontend untouched.** Dashboard keeps calling the TS backend until a
   slice is promoted; promotion = reverse-proxy route (path-prefix) to C
   with instant fallback. No dashboard code changes per slice.
4. **No Node in the C process.** C links only libc + SQLite (+ later a
   minimal TLS lib). Anything needing Node semantics (Next rewrites, RSC)
   stays in TS; C implements the _observable_ contract (status/headers/body/
   framing), not Next internals.
5. **Reference stays green.** TS suites (`test:unit`, `test:vitest`,
   `test:e2e` as applicable) must pass before and after each slice; new
   harness vectors become permanent regression tests on both sides.

---

## 4. Work breakdown (slices in promotion order)

Each slice = parser/handler + auth + storage + errors + streaming + tests +
budgets + harness vectors. Suggested order minimizes risk while proving the
plumbing early:

| #   | Slice                                                                                                              | Why this order                                                                     | Proves                               |
| --- | ------------------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------- | ------------------------------------ |
| 0   | Build + `core/` + `http/` skeleton + `/healthz`-equivalent + harness                                               | nothing to compare without it                                                      | ports, RSS/HWM harness, CI           |
| 1   | `GET /v1/models` (+ aliases, HEAD, error shape)                                                                    | read-only, no upstream, no streaming; exercises codegen tables end to end          | codegen parity, catalog cache budget |
| 2   | Auth + errors (`extractApiKey` semantics, 401/403/415/404 shapes, CORS subset)                                     | every later slice depends on identical rejects                                     | auth parity, `buildErrorBody` parity |
| 3   | `POST /v1/chat/completions` non-stream via **one** pinned mock upstream                                            | smallest streaming-free write path with persistence (`call_logs`, `usage_history`) | arena + SQLite + usage accounting    |
| 4   | SSE streaming for the same path (keepalive, terminal frame, abort)                                                 | hardest framing risk, isolated to one path                                         | pump buffers, failure boundary       |
| 5   | Responses API transform (both modes)                                                                               | reuses §4 plumbing + transform stream                                              | event order, snapshot handling       |
| 6   | Storage slices (keys/combos/settings/providers/usage/files/batches)                                                | management parity without upstreams                                                | migration runner, DAO parity         |
| 7   | Routing (priority → round-robin → weighted → … → auto → fusion/pipeline) + breaker/cooldown/lockout + quotas       | needs §3–§6; biggest logic surface                                                 | strategy-by-strategy vectors         |
| 8   | Remaining `v1` (messages, completions, embeddings, images, media, search, relay, vscode tokens)                    | per-family, mostly independent                                                     | per-family harness                   |
| 9   | MCP (stdio + StreamableHTTP + SSE) + scopes + audit                                                                | needs §2–§5; 110 tools land in tool-family batches                                 | tool vectors, session sweep          |
| 10  | A2A + skills + compression + memory/Qdrant-client + guardrails/webhooks/log-export                                 | largest optional surface; each independently shippable                             | per-feature vectors                  |
| 11  | Ops parity (backup/restore, tunnels/MITM stubs, version-manager, tunnels 501s) + promotion proxy + cutover runbook | last: only after user paths are green                                              | cutover without frontend change      |

### 4.5 Codegen (prevents "lite" drift)

- `native/codegen/` generates `providers.c/h`, `models.c/h`,
  `strategies.c/h` from `src/shared/constants/providers/*` +
  `open-sse/config/providers/registry/*` + `routingStrategies.ts`.
- The generator runs at build; outputs are checked in for review; CI fails
  when TS registry changes without regenerated C tables (`check:codegen`
  gate to be added in Task 002).
- Counts (providers, models, aliases, strategies) are asserted in C unit
  tests against the TS counts recorded in ARCHITECTURE §0.

---

## 5. Compatibility harness (minimum contract)

Location: `native/compat/` (client + comparators + corpus; exact files are
Task 002's job — this section fixes the **contract**).

### 5.1 Topology

```
corpus/*.http.json  ──▶  harness ──▶ TS :20128 (DATA_DIR=ts.db) ──▶ record A
                        └─────────▶ C  :20129 (DATA_DIR=c.db)  ──▶ record B
                                      comparators(A,B) → PASS/FAIL + diff
                                      RSS/HWM/threads/FDs sampled per case (§5.4)
```

Fixture seeding: identical SQL fixture → copy to `ts.db` / `c.db` before
each case group; migrations under test run per-backend from the same
`from_version`.

### 5.2 Corpus (seed from existing suites, not hand-waved)

- Route-shape cases from `tests/unit/**` + `tests/e2e/**` request fixtures.
- #7847 large body from `scripts/perf/agentPayloadCorpus.ts`.
- SSE vectors: recorded `stream.sse` files with frame boundaries preserved.
- Auth vectors: valid/invalid/expired/scoped keys, banned, env-key, session
  cookie, CSRF on unsafe management paths, token-in-path (vscode).
- Failure vectors: unknown path (JSON 404), 415 content-type, retired model,
  unknown provider, breaker-open, cooldown-hit, lockout-hit (seeded state).

### 5.3 Comparison dimensions (all required per case)

| #   | Dimension             | PASS condition                                                                                                                                                                                                                                  |
| --- | --------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | HTTP status           | exact equality (including 415/404/429/503 shed paths)                                                                                                                                                                                           |
| 2   | Relevant headers      | `content-type`, SSE headers (`content-type: text/event-stream`, `cache-control`, `connection`), `X-OmniRoute-Combo-Trace` presence/shape, CORS `vary`/`allow-origin` policy outcome, compression echo; order-insensitive except SSE frame order |
| 3   | JSON structure        | deep-shape equality ignoring volatile fields listed in `ignore.json` (`request_id`, timestamps, `created`, usage wall-times); types exact (`null` vs missing matters)                                                                           |
| 4   | Error structure       | `error.{message,type,code}` triple equal; no `at /` stack leakage in either body (Hard Rule #12 test)                                                                                                                                           |
| 5   | Auth behavior         | same accept/reject + same `WWW-Authenticate`/JSON error per credential class; scoped-key allow/deny identical                                                                                                                                   |
| 6   | Streaming/SSE framing | byte-identical event sequence after normalization (strip keepalive `:ping`/comment frames listed in `ignore.json`); `data: [DONE]` + terminal frame present on both                                                                             |
| 7   | Event ordering        | ordered compare (no set-compare); fusion panel order normalized only where TS documents nondeterminism                                                                                                                                          |
| 8   | Cancellation          | abort mid-stream on both → connection closes, no further frames, persistence effects equal (see 9)                                                                                                                                              |
| 9   | Persistence effects   | `sqlite3 .dump` of touched tables + artifact files identical modulo `ignore.json` (ids, timestamps); no extra/missing rows                                                                                                                      |

### 5.4 Resource capture per case (feeds MEMORY_MODEL §4.10)

`rss_before/after`, `VmHWM`, `threads`, `fds`, wall ms, TTFB (stream),
bytes in/out, DB bytes before/after. Stored beside each case result;
budgets in `native/bench/budgets.json` fail the case on exceed.

### 5.5 Promote / rollback rule

Promote a slice's route-prefix to C only when: harness PASS on the slice's
full vector set on Linux x86_64 **and** arm64, TS suites still green,
budgets met, and the design review records no new iOS 12 blocker. Rollback
= flip the prefix back to TS (one config line, no deploy).

---

## 6. Proposed first native vertical slice (concrete)

**Slice 1: `GET /v1/models` (+ `/models` alias, `HEAD`, JSON-404 neighbor).**

- Touches: `core/` (arena, buf, err, log), `http/` (router, headers),
  `providers/` codegen tables, `storage/` read-only open + `models` DAO,
  `auth/` (public-readonly path: no key required — verify against TS!),
  `api_v1/models.c`.
- Deliberately excludes: upstreams, streaming, writes, JWT, OAuth, workers.
- Harness vectors: list (empty/populated DB), `?provider=` filters if TS
  supports them (verify — do not assume), single-model get, unknown model
  (exact TS status+body), HEAD, wrong-method, CORS preflight, unknown path
  JSON-404, `Accept` variants.
- Budgets: idle RSS target MEMORY_MODEL §5; per-request arena ≤ catalog
  row-set estimate; catalog cache capped; no `malloc` after warmup on repeat
  `GET` (assert via bench).
- Done when: §5 comparators PASS + counts asserted + budgets met + C unit
  tests for the slice live in `native/tests/`.

---

## 7. Risks

### 7.1 Architectural risks

1. **Next.js semantics leak into the contract.** Rewrites (`/v1/*`,
   `/codex/*`, case-insensitive proxy matcher GHSA-jvqc-mp9f-q936),
   `after()` background refresh, RSC streaming, and `NextResponse.next`
   header-stamping have no C equivalent — the C server must reproduce
   _observable_ behavior from scratch. Mitigation: harness vectors for every
   alias/method/header case; never assume Express-like routing.
2. **Translator breadth.** 10 formats × request/response × tool-call edge
   cases (orphaned results, ID coercion, system-hoist, reasoning-replay) is
   the largest correctness surface after routing. Mitigation: port behind
   `tests/translator/**` vectors; slice 8, not earlier.
3. **Resilience triple.** Breaker (provider) vs cooldown (connection) vs
   lockout (model) interact per attempt; mis-scoping either over-blocks
   (outage) or under-blocks (retry storm). Mitigation: seeded-state harness
   vectors per mechanism + the three-layer diagram
   (`docs/diagrams/resilience-3layers.mmd`) as the spec.
4. **Streaming failure boundary.** The "never a hung stream" guarantee
   (terminal frame + abort cleanup) must hold while buffers stay bounded.
   Mitigation: slice 4 proves it on one path before any other stream ships.
5. **SQLite concurrency.** TS uses synchronous better-sqlite3 + WAL +
   2s busy timeout on a single writer discipline; a naive threaded C port
   deadlocks or busy-loops. Mitigation: single-writer queue, small
   `busy_timeout`, checkpoint-on-idle, `WD40`: write-discipline tests first.
6. **Secret handling.** AES-256-GCM + scrypt compat must decrypt existing
   DBs byte-identically; any deviation bricks credentials. Mitigation: port
   `encryption.ts` against known-answer vectors before any credential slice.

### 7.2 Feature-parity risks

1. **Silent "lite" drift** (the existential risk): 358 providers → subset,
   20 strategies → 5, 110 MCP tools → 45. Mitigation: codegen + count
   assertions + FEATURE_PARITY checklist gate per PR.
2. **Long-tail route families** (vscode tokens, bifrost, muse-code, provider
   plugin manifest, session leases, video-bridge drilldown) are easy to miss
   because each is small. Mitigation: coverage checklist (FEATURE_PARITY end)
   enforced in review; harness corpus seeded from the 699-route enumeration.
3. **Error-shape drift.** Dozens of handlers share `buildErrorBody`, but
   codes/messages differ per path; tests asserting `!includes("at /")` exist
   for a reason. Mitigation: §5.3 dimension 4 on every vector.
4. **Auth scoping.** Scoped keys, env-key synthesis, token-in-path, session
   vs key vs OIDC paths interact. Mitigation: auth slice (§4 #2) before any
   promotion; matrix of credential classes in corpus.
5. **Background effects.** `after()` refresh, spend batching, audit/webhook
   deliveries, MCP session sweeps change observable state after the response.
   Mitigation: persistence compare includes post-settle dump (settle delay
   in harness config).

### 7.3 iOS 12 compatibility risks

1. **No Node/V8 — but also no modern-libc comforts.** iOS 12 SDK + arm64 +
   old libSystem: verify `clock_gettime`, `pthread` options, `mmap` flags,
   `fcntl` FD handling, and TLS APIs available at that deployment target.
   Anything using newer `dispatch`/`Network.framework` APIs needs a fallback
   or is out.
2. **TLS stack choice is the critical path.** TS relies on Node TLS +
   `wreq-js` Rust fingerprinting + per-provider TLS clients. On iOS the
   realistic options are SecureTransport (old, limited) vs bundled
   mbedTLS/BearSSL-style C TLS (size + audit cost) — none chosen in Task 001
   by design. Upstream fetch stays a **[blocking decision for Task 002]**;
   slices 0–2 must run without it, slice 3 forces the choice.
3. **No `fork`/`exec`, no JIT, limited background.** Spawn paths (services,
   tunnels, MITM, ffmpeg, browser pool) are structurally unavailable — they
   stay 501-stubbed on iOS. WASM/JIT optionals (tiktoken WASM, ONNX) need
   ahead-of-time or CPU-fallback evaluation per library.
4. **1 GiB shared RAM.** The <4 MiB idle target + small SQLite
   (`cache_size`, `mmap_size=0`) + workers 0/1 + no browser/ONNX/media
   residents are mandatory, not aspirational. Any slice exceeding budgets on
   Linux x86_64 has no chance on A7.
5. **Native-dep audit.** `better-sqlite3`, `sharp`, `onnxruntime-node`,
   `keytar`, `wreq-js` Rust natives, TProxy C addon, Playwright Chromium —
   none ship to iOS. The C port must not reintroduce them as build deps;
   SQLite amalgamation + chosen TLS + chosen crypto are the only natives.
6. **Keychain vs `STORAGE_ENCRYPTION_KEY`.** Credential-encryption secret
   storage differs on iOS (Keychain) vs Linux (env/file). Design the secret
   provider interface now (§2 `crypto/` + platform shims), implement per-OS
   later.

### 7.4 Schedule risks

Upstream moves fast (release-train model + parallel-cycle freezes per
AGENTS.md Hard Rule #21). Mitigation: pin the audited rev in each slice PR,
re-run codegen diff + harness on rebase, and never merge instruction-surface
files without operator approval (Review-focus rule).

---

## 8. iOS 12 feasibility notes (no code yet)

- Build: dedicated `platform-toolchain/ios12-arm64.cmake` (SDK pin,
  `-miphoneos-version-min=12.0`, arm64 only, bitcode off, stack sizes
  explicit, `-Os`, LTO optional, warnings-as-errors for the new code).
- Runtime: single binary, no embedded Node, no JIT, no `fork`; tempfiles in
  the app sandbox; Keychain-backed secret provider; background-fetch-safe
  idle (no timers holding the app awake — all intervals `unref`-equivalent).
- Defeature matrix for iOS builds (compile-time flags, all default-off on
  iOS — proposed C build options, not env vars): OMNI_C_EMBEDDED_SERVICES,
  OMNI_C_TUNNELS, OMNI_C_MITM, OMNI_C_BROWSER, OMNI_C_ONNX,
  OMNI_C_MEDIA_FFMPEG, OMNI_C_QDRANT_SRV (client only), OMNI_C_REDIS
  (stubbed cache).
- Validation: every slice's C unit tests + harness must also run for the
  Linux→iOS cross-compile (build-break = slice-break), with budgets checked
  against the iOS column in `budgets.json`.

---

## 9. Test/verification plan per slice (maps to repo gates)

- Keep green: `npm run lint`, `typecheck:core`, `test:unit` (node native),
  `test:vitest` (MCP/autoCombo/cache), focused single-file runs
  (`node --import tsx/esm --test …`), `check:docs-all` for doc edits,
  `check:cycles`, coverage ratchet (60/60/60/60 floor).
- New: `native/tests/*` (C unit), `native/compat` harness (§5),
  `native/bench/*` (RSS/HWM/threads/FDs), `check:codegen` (Task 002).
- Bug-fix protocol (Hard Rule #18) applies to harness failures too: failing
  vector → fix → green vector as the permanent guard; no "worked locally"
  promotions.

---

## 10. Open questions (runtime measurement, not source reading)

1. Real RSS numbers for MEMORY_MODEL §4 on this machine (startup/idle+5m/
   models/normal/stream/responses/large/compression/MCP/A2A/recovery/HWM/
   threads/FDs) — **[unmeasured]**.
2. Exact `fetch()` dispatch site(s) per executor family (audit stopped at
   `getExecutor→execute`; pin during slice 3).
3. Full dashboard per-page fetch contracts (page-tree read during slice 6).
4. TLS library choice for C upstream fetch (blocks slice 3; spike in
   Task 002 with handshake interop + iOS 12 build proof).
5. Crypto compat vectors for AES-256-GCM/scrypt (`encryption.ts`) — extract
   known-answer pairs during slice 2.
6. Whether `/v1/models` requires auth in this deployment (route says
   public-readonly relax; verify live before slice 1 asserts it).
7. `bench:highwatermark` target missing — restore or drop before citing it.
8. Counts that drift: re-run ARCHITECTURE §0 commands per slice PR.

---

_Files created in Task 001: `docs/native-backend/ARCHITECTURE.md`,
`docs/native-backend/FEATURE_PARITY.md`, `docs/native-backend/MEMORY_MODEL.md`,
`docs/native-backend/MIGRATION_PLAN.md` (this file). No production code
changed; no C code written._
