# OmniRoute Memory Model (Reference Implementation → C Budgets)

> Task 001. What the reference TypeScript backend likely costs in RAM, where
> to look, and how to obtain **real** baseline measurements. No benchmark
> results are invented here — every number below is either a checked-in
> constant or explicitly marked **[unmeasured — run the procedure]**.
>
> Target context: iPad mini 2 / A7 / iOS 12.5.7 has ~1 GiB RAM shared with
> the OS. Idle RSS and peak RSS are first-class architecture constraints for
> the C backend. Linux x86_64/arm64 is the measurement platform.

## 1. Summary of likely RSS contributors (ranked by evidence)

| Rank | Source                                                                                          | Why it matters                                                                                                                                                                                       | Key paths / constants                                                                                                                                                                                                     | Confidence                                                         |
| ---- | ----------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------ |
| 1    | Node/V8 baseline + `--max-old-space-size=8192`                                                  | Dev/test scripts allow an 8 GiB heap; heap ≠ RSS but caps worst case. Prod calibrates via `OMNIROUTE_MEMORY_MB` (~35% RAM, clamp 512–4096; Docker 1024)                                              | `package.json` dev/test scripts, `scripts/build/runtime-env.mjs:39-130`, `scripts/build/build-next-isolated.mjs:200-213` (#6409 comment)                                                                                  | confirmed constant, **unmeasured** RSS                             |
| 2    | Module loading (699 routes, Next 16, React 19, dashboard)                                       | Every route + dashboard chunk is importable; Turbopack/webpack build memory anecdote 15.4+17.2 GB RSS concurrent (`docs/ops/RUNNER_BOX.md:66`)                                                       | `src/app/**/route.ts`, `src/app/(dashboard)/**`, `next.config.mjs`                                                                                                                                                        | confirmed scale, **unmeasured** per-route cost                     |
| 3    | Provider/model static registries (256 registry dirs, 358 providers)                             | Largest static data in the repo; lazy `Proxy` mitigates startup but full `/v1/models` aggregation still walks it                                                                                     | `open-sse/config/providers/registry/*`, `open-sse/config/constants.ts:82 PROVIDERS Proxy`, `src/shared/constants/providers.ts`                                                                                            | confirmed size, **unmeasured** bytes                               |
| 4    | Request-body buffering + JSON duplication (729 msgs / 86 tools / 3.05 MiB incident shape)       | The #7847 incident shape is the canonical large-Codex request; clones (`cloneLogPayload`, `cloneBoundedForLog`, `structuredClone × targets`, `JSON.stringify` estimate) multiply it per combo target | `scripts/perf/request-body-heap.ts`, `scripts/perf/agentPayloadCorpus.ts` (`INCIDENT_SHAPE={729,86,527}`), `src/sse/handlers/chat.ts`, `open-sse/utils/requestLogger.ts`, `open-sse/services/combo.ts:attemptBody`        | confirmed code paths, **unmeasured** on this machine               |
| 5    | SSE/WebSocket streaming buffers                                                                 | Per-stream buffers + 16k HWM transform + 3s keepalive + failure-boundary collector; 500-stream heap-growth gate exists                                                                               | `open-sse/transformer/responsesTransformer.ts:575`, `open-sse/utils/stream*.ts`, `tests/integration/heap-growth.test.ts` (50 warmup + 500 streams, growth <20 MB)                                                         | confirmed mechanism, gate threshold known                          |
| 6    | Caches (readCache TTLs, quota/auth LRU+Redis, semantic/reasoning, catalog, embedding, tiktoken) | Many small TTL/LRU maps; individually bounded but collectively long-lived; tiktoken WASM is a fixed cost once loaded                                                                                 | `src/lib/db/readCache.ts:74`, `src/lib/db/apiKeys.ts` (60s + Redis 1h), `open-sse/services/{quotaCache,reasoningCache,signatureCache}.ts`, `src/shared/utils/tiktokenCounter.ts`, `src/app/api/v1/models/catalogCache.ts` | confirmed, **unmeasured** total                                    |
| 7    | SQLite page cache + WAL + mmap + FTS5 + sqlite-vec                                              | `cache_size=-<setting>`, `mmap_size=256MiB` default, WAL file, FTS5 index, optional native vector index                                                                                              | `src/lib/db/core.ts:1306-1352`, `optimizationSettings.ts`, `walMaintenance.ts`, mig `022/178` FTS5                                                                                                                        | confirmed settings, **unmeasured** RSS effect                      |
| 8    | Compression intermediates + worker pool                                                         | `OMNI_COMPRESSION_WORKERS=2`, 120s timeout, per-engine buffers, memo map, ONNX/LLMLingua optionals                                                                                                   | `open-sse/services/compression/compressionWorkerPool.ts:104`, `resultMemo.ts`, `engines/llmlingua/*`                                                                                                                      | confirmed, **unmeasured**                                          |
| 9    | Browser automation + media natives                                                              | Playwright Chromium (`packages/browser-pool`, `browserPool.ts:172`), `playwright-core` persistent contexts, `sharp@0.35`, `ffmpeg` child, `onnxruntime-node`, `@huggingface/transformers`            | `packages/browser-pool/**`, `open-sse/vendor/codex-chatgpt-web/**`, `open-sse/utils/imageNormalize.ts`, `scripts/perf/video-bridge-*.ts`                                                                                  | confirmed presence, **unmeasured** (expected dominant when active) |
| 10   | Child processes / workers / tunnels / MITM                                                      | Per-child RSS outside Node heap: service installers (`npm/node`), cloudflared/tailscale/ngrok, MITM server, TProxy native                                                                            | `src/lib/services/installers/*`, `src/lib/{tailscaleTunnel,cloudflaredTunnel}.ts`, `src/mitm/manager.ts:614`, `src/mitm/tproxy/native/transparent.c`                                                                      | confirmed, **unmeasured**                                          |
| 11   | Long-lived Maps/Sets/arrays + timers/queues                                                     | Breaker map, cooldown/lockout maps, rate-limit windows, MCP sessions, dedup map, quota fetchers, spend batch writer, sweeps (60s/5m/6h)                                                              | `src/shared/utils/circuitBreaker.ts`, `open-sse/services/{providerCooldownTracker,accountFallback,rateLimitManager,requestDedup}.ts`, `mcp-server/httpTransport.ts:34`, `src/instrumentation-node.ts` schedulers          | confirmed, **unmeasured**                                          |
| 12   | i18n + dashboard assets                                                                         | 66 locales src/i18n/messages/*.json, Mermaid/Monaco/Next dashboard weight                                                                                                                            | src/i18n/messages/, .size-limit.json (bin 15 KB, mcp 5 KB — code only, not runtime)                                                                                                                                       | confirmed files, **unmeasured** runtime                            |

What is **not** a significant steady-state cost (evidence): `docs/ops/VM_DEPLOYMENT_GUIDE.md:432-435` reports idle `200–400 MB RSS` at `OMNIROUTE_MEMORY_MB=512` — **[cited, not re-measured here]**.

---

## 2. Subsystem-by-subsystem memory notes

### 2.1 Node/V8 baseline

- All dev/test scripts pin `--max-old-space-size=8192`; prod derives the heap
  from `OMNIROUTE_MEMORY_MB` (`runtime-env.mjs`). Heap limit ≠ RSS: native
  addons, WASM, Buffers, and child RSS sit outside the V8 heap
  (`build-next-isolated.mjs` #6409 comment).
- Admission sheds load at `heapUsed/heap_size_limit >= 0.75`
  (`chatBodyAdmission.ts:155-165`, `OMNIROUTE_CHAT_ADMISSION_HEAP_SHED_RATIO`)
  and returns 503 `heap_pressure` past `max(85% ceiling, 400 MB)`
  (`heapPressure.ts`). Ingest budget = 25% ceiling / 8× amplification, clamp
  8 MiB–2 GiB (`admissionBudget.ts:32-110`).
- C implication: no V8 heap at all; replace percentage-of-heap guards with
  byte-budget guards (arena size, ingest cap, concurrent-body cap).

### 2.2 Module loading

- 699 route modules + dashboard + `open-sse/*` are all resolvable by the Next
  build. The proxy cold-import anecdote (#10627, `src/proxy.ts:14-21`) shows a
  single cold `import("@/lib/db/settings")` (native SQLite load) could stall
  every proxied request — hence boot-time warming.
- C implication: static provider metadata (no per-request parse), lazy
  subsystem init, optional heavy state unloadable when idle.

### 2.3 Static registries / provider metadata

- 256 `open-sse/config/providers/registry/*` dirs + 10-section
  `src/shared/constants/providers/*` + image/video/audio/search/rerank/
  upscale registries + 66 i18n JSON files. `PROVIDERS` is a lazy Proxy
  (`constants.ts:82`) to cut startup, but `/v1/models` aggregation
  (`catalog*.ts`) still walks the union plus live-catalog state.
- C implication: compile-time/static tables (generated from the TS registry),
  `mmap` read-only blobs where the OS supports it, no per-request registry
  copies.

### 2.4 Request body buffering / JSON duplication

- Route reads `request.json()` **once** (`chat/completions/route.ts`,
  #4380/#7862 comment); deep validation runs over the parsed object.
- Duplication risks: `cloneLogPayload` (chat.ts), `cloneBoundedForLog`
  (requestLogger.ts), `structuredClone × TARGETS` (combo `attemptBody`),
  `JSON.stringify` for token estimation, per-target `handleSingleModel`
  bodies, fusion panel copies (≤40 stripped copies + judge body).
- The repo's own regression tool is `bench:heap-body`
  (`scripts/perf/request-body-heap.ts`, `--expose-gc`, `--json`,
  `--max-retained-mib`) driven by the deterministic LCG corpus
  (`agentPayloadCorpus.ts`, guarded by `heap-benchmark-corpus.test.ts`).
- C implication: slice-based parsing, single owned body per request, borrow
  for fan-out, bounded log clones, spill >threshold bodies to temp files.

### 2.5 Response / SSE / WS buffering

- Non-stream legs collect the full upstream body before translation
  (`nonStreamingProviderLeg.ts`); streaming legs buffer per-chunk plus the
  failure-boundary collector (`streamPayloadCollector.ts`) and the
  Chat→Responses 16k-HWM transform.
- WS bridge holds sockets + proxy mappings (`ws@8.21.3`).
- Leak gate: `tests/integration/heap-growth.test.ts` (50 warmup + 500
  `createSSEStream(passthrough)`, `gc+sleep+gc`, assert growth <20 MB);
  run via `test:heap`.
- C implication: pump upstream→downstream with a fixed ring/chain buffer,
  flush on chunk boundaries, no full-response retention except when the API
  demands it (non-stream JSON), cap concurrent streams × buffer.

### 2.6 Caches

- Bounded individually (5s/30s/5s readCache TTLs, 60s auth LRU + 1h Redis,
  session sweep 60s/idle 5m, quota fetcher cleanup intervals, reasoning-cache
  sweep, compression `resultMemo`, catalog/embedding caches), but count is
  high — audit before assuming "small".
- C implication: one cache-budget table with per-cache byte caps + idle TTL +
  explicit `release_when_idle` for heavy caches (embeddings, catalog).

### 2.7 SQLite page/cache behavior

- Pragmas: `busy_timeout=2000`, `journal_mode=WAL`, `synchronous=NORMAL`,
  `cache_size=-<setting>` (negative = KiB), `temp_store=MEMORY`,
  `mmap_size=256MiB` default (stored setting, mig 046).
- Maintenance: WAL `TRUNCATE` 6h / `PASSIVE` 5m / 256 MB guard
  (`OMNIROUTE_WAL_*`), 6h `VACUUM`, stale purge, `VACUUM INTO` backups,
  6h DB health scheduler.
- FTS5 (`memories` + index, mig 015/022/023/178) and optional `sqlite-vec`
  add index memory beyond page cache.
- C implication: small `cache_size`, small/zero `mmap_size` on 1 GiB target,
  `synchronous=NORMAL` keep, checkpoint on idle, measure WAL-SHM with `ls -l`
  - `pmap`.

### 2.8 Compression / embeddings / vector search

- Worker pool (`OMNI_COMPRESSION_WORKERS=2`, 120s timeout, 60s idle) +
  `compressionWorker.js/ts` + LLMLingua ONNX workers
  (`@huggingface/transformers` + `onnxruntime-node` + `@atjsh/llmlingua-2`).
- Embedding paths: remote, custom provider, local transformers, static potion;
  rerank listings; Qdrant (`omniroute_memory`, Cosine, int8/binary
  quantization, rescore) via `docker-compose --profile memory`.
- C implication: compression and embeddings must be lazy-loaded, worker count
  0/1 on iOS, ONNX optional and off by default; Qdrant client only (no server
  on device).

### 2.9 Browser automation / media / child processes / workers

- `packages/browser-pool` (`chromium.launch`), `playwright-core`
  `launch/launchPersistentContext` (codex/chatgpt-web adapters, obscura/conol
  logins — lazy `import(playwright)`), `sharp` image normalize,
  `ffmpeg execFile` (video-bridge FU07 eval measures `maxRssKiB` via
  `/usr/bin/time`), service children (`runNpm`), tunnels, MITM server,
  TProxy native addon, `wreq-js` Rust TLS natives.
- These dominate peak RSS whenever active — but are all optional/lazy.
- C implication: none of these belong in the always-resident core. Gate
  behind explicit enable flags; on iOS default off.

### 2.10 Retained request state / timers / queues

- Per-request: admission lease, translator state, executor instance, combo
  trace (`X-OmniRoute-Combo-Trace`), routing-event sinks, OTel queue,
  `after()` models-refresh, spend-batch entries, audit/webhook deliveries.
- Global: breaker/cooldown/lockout/rate-limit/dedup/quota maps, MCP sessions,
  skill/plugin registries, scheduler timers (all `unref()`d).
- C implication: request-scoped arenas freed on completion/cancel; global
  maps with count + byte caps + idle eviction; no unbounded queues.

---

## 3. Existing memory/performance tooling (use it, don't reinvent it)

| Tool                       | Command                                                                                                                            | What it actually measures                                                                                  | Memory relevance                            |
| -------------------------- | ---------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- | ------------------------------------------- |
| request-body heap bench    | `npm run bench:heap-body` (`node --expose-gc --import tsx/esm scripts/perf/request-body-heap.ts [--json] [--max-retained-mib N]`)  | V8 retained heap after ×4 `gc()` per clone mechanism on the #7847 corpus                                   | Direct: per-target body-copy cost           |
| deterministic corpus       | `scripts/perf/agentPayloadCorpus.ts` (+ `tests/unit/heap-benchmark-corpus.test.ts`)                                                | LCG corpus, `INCIDENT_SHAPE={729 msgs, 86 tools, 527 …}`                                                   | Repro for the above                         |
| routing-events bench       | `npm run bench:routing-events` (`scripts/perf/routing-events-bench.ts`)                                                            | `perf_hooks` µs/op + ops/s for factor calc + event dispatch + OTel enqueue                                 | Event-overhead ceiling                      |
| compression bench          | `npm run bench:compression` (`bun scripts/compression/benchmark.ts`)                                                               | token savings/retention per engine on fixed corpus                                                         | Quality/cost, **not** RAM                   |
| compression budget gate    | `npm run check:compression-budget` (+ `compression-budget-baseline.json`, 2% tolerance)                                            | tokens/task vs baseline                                                                                    | Guards against prompt bloat (indirect RAM)  |
| heap leak gate             | `npm run test:heap` (`tests/integration/heap-growth.test.ts`)                                                                      | heap growth over 500 SSE streams (<20 MB)                                                                  | Direct: stream-path leaks                   |
| video-bridge benches       | `scripts/perf/video-bridge-{bench,fu07-eval,contact-sheet-eval,promotion-eval}.ts`                                                 | `memoryUsage/maxRSS/cpu/latency`, `ffmpeg` child `maxRssKiB` via `/usr/bin/time`                           | Media-path peak RSS                         |
| bundle/size ratchets       | `npm run check:bundle-size` (+ `.size-limit.json`), `check-file-size.mjs`                                                          | gzip bytes vs `quality-baseline.json`                                                                      | Code size, not RSS                          |
| router/compression evals   | `npm run eval:router*`, `npm run eval:compression`                                                                                 | AIQ/cost/latency, quality/cost/save (spend-gated)                                                          | Regression context                          |
| pressure probes            | `open-sse/utils/{heapPressure,resourcePressure*}.ts`, `chatBodyAdmission.ts`                                                       | live `heapUsed/limit`, cgroup/PSI                                                                          | Runtime guard behavior                      |
| `wtfnode@0.10.1`           | devDependency, no wired script                                                                                                     | open handles on demand                                                                                     | Leak debugging                              |
| reference-backend baseline | `npm run bench:reference-baseline` (`node scripts/perf/baseline/reference-backend-baseline.mjs --runs 3 --port 21138 --out <dir>`) | External `/proc` RSS/peak-RSS/threads/FDs across startup → idle → 5-min idle → `GET /v1/models` → recovery | Direct: the Task 002 measured baseline (§7) |

Not present: `clinic`, `autocannon`, `k6` configs, Lighthouse (only nightly
mention in `docs/ops/QUALITY_GATE_PLAYBOOK.md:85,159` and unrelated string
hits). `bench:highwatermark`: **confirmed dangling (Task 002)** — the
`package.json:99` target references a `benchmark-highwatermark.ts` file under
`scripts/perf/` which does not exist; the script line was added without its
file in upstream commit `e2e330a05` (package.json-only change, no file under
`scripts/perf/` in that commit), it is not generated, and no same-purpose
file exists under another name (other `highWaterMark` hits are stream buffer
options). Pre-existing upstream omission; do not cite it as runnable.
(Full verification commands in §7.8.)

---

## 4. Baseline measurement procedures (Linux; no invented results)

Prerequisites: `npm install`, `cp .env.example .env` + `JWT_SECRET` /
`API_KEY_SECRET` per AGENTS.md, free port 20128, `curl`, `python3` (harness),
`/usr/bin/time -v` (peak RSS), `node --expose-gc` where noted. All
**[unmeasured — run the procedure]** unless stated.

### 4.0 Start the reference server reproducibly

```bash
npm install
PORT=20128 npm run build          # or npm run dev for dev-mode numbers (state which)
PORT=20128 npm start &
SRV=$!
sleep 5 && curl -sf http://localhost:20128/api/health
```

Record: `node --version`, `git rev-parse --short HEAD`, dev vs standalone,
`OMNIROUTE_MEMORY_MB`, `DATA_DIR` size (`du -sh ~/.omniroute`).

### 4.1 Startup / idle RSS

```bash
PID=$(pgrep -f "run-next|run-standalone|server-ws" | head -1)
sleep 2;  ps -o pid,rss,vsz,etime,cmd -p "$PID"      # startup RSS (KiB)
sleep 300; ps -o pid,rss,vsz,etime,cmd -p "$PID"     # idle RSS after 5 min
cat /proc/$PID/status | grep -E 'VmRSS|VmHWM|Threads|FDSize'
ls -l /proc/$PID/fd | wc -l                          # open FDs
cat /proc/$PID/status | grep Threads                  # thread count
```

Also capture V8 view: `node -e 'console.log(process.memoryUsage())'` is
per-process — prefer the server's own `/api/monitoring/health` +
`resourcePressure` log line (`heapUsed/heapTotal/rss/external/arrayBuffers`)
over a second node process.

### 4.2 `/v1/models` (catalog aggregation, no upstream spend)

```bash
/usr/bin/time -v curl -sf http://localhost:20128/v1/models \
  -H "Authorization: Bearer $KEY" -o /tmp/models.json
ls -l /tmp/models.json; node -p "require('/tmp/models.json').data.length"
ps -o pid,rss -p "$PID"   # delta vs §4.1
```

### 4.3 Normal completion (non-stream, mocked or cheap upstream)

Prefer a local/mock upstream to avoid spend; otherwise use the cheapest
configured model. Record wall ms, response bytes, RSS delta, `call_logs` row.

```bash
/usr/bin/time -v curl -sf http://localhost:20128/v1/chat/completions \
  -H "Authorization: Bearer $KEY" -H 'Content-Type: application/json' \
  -d '{"model":"<cheap-or-mock>","messages":[{"role":"user","content":"ping"}]}' \
  -o /tmp/normal.json
```

### 4.4 Streaming completion (SSE framing + ordering)

```bash
curl -sN http://localhost:20128/v1/chat/completions \
  -H "Authorization: Bearer $KEY" -H 'Content-Type: application/json' \
  -d '{"model":"<cheap-or-mock>","messages":[{"role":"user","content":"count to 5"}],"stream":true}' \
  -o /tmp/stream.sse --write-out '%{time_starttransfer} %{size_download}\n'
grep -c "^data:" /tmp/stream.sse; tail -3 /tmp/stream.sse  # terminal frame?
ps -o pid,rss -p "$PID"
```

### 4.5 `/v1/responses` (both modes)

Repeat §4.3/§4.4 against `/v1/responses` with a Responses-shaped body;
verify event sequence contains `response.completed` and no duplicated
snapshot tail (see PII learning §6).

### 4.6 Large Codex-style request (the #7847 shape)

Use the checked-in corpus instead of hand-crafting:

```bash
node --expose-gc --import tsx/esm scripts/perf/request-body-heap.ts --json | tee /tmp/heap-body.json
node --expose-gc --import tsx/esm scripts/perf/request-body-heap.ts --max-retained-mib 64
```

Then send the generated corpus body (see `agentPayloadCorpus.ts` export)
to `/v1/chat/completions` (non-stream) under `/usr/bin/time -v` and record
peak RSS + 413/503 behavior.

### 4.7 Compression

```bash
npm run bench:compression          # quality baseline (bun)
curl -sf http://localhost:20128/api/compression/compare \
  -H "Authorization: Bearer $KEY" -H 'Content-Type: application/json' \
  -d @tests/fixtures/compression-sample.json -o /tmp/compress.json
/usr/bin/time -v curl -sf ...      # peak RSS with OMNI_COMPRESSION_WORKERS=2 vs 0
```

### 4.8 MCP + A2A

```bash
npm run test:vitest               # MCP/autoCombo/cache suites (blocking in CI)
npm run test:protocols:e2e        # MCP+A2A protocol E2E (advisory #10049)
# Live: POST /api/mcp/stream (StreamableHTTP) + GET /api/mcp/sse, watch RSS delta per tool call
curl -sf http://localhost:20128/api/a2a/status -H "Authorization: Bearer $KEY"
```

### 4.9 Post-request recovery / peak RSS / threads / FDs

```bash
sleep 60; ps -o pid,rss,vsz -p "$PID"              # recovery vs §4.1
cat /proc/$PID/status | grep -E 'VmHWM|Threads'    # peak RSS + threads
ls /proc/$PID/fd | wc -l                           # FDs (sockets, WAL, children)
ls -lh ~/.omniroute/storage.sqlite*                # DB + WAL + SHM sizes
node --expose-gc --import tsx/esm --test tests/integration/heap-growth.test.ts
```

### 4.10 Reporting format (paste into PRs / Task 002 planning)

```
build: <dev|standalone>  rev: <sha>  node: <v>  OMNIROUTE_MEMORY_MB: <n>
startup RSS: …  idle RSS: …  idle+5m RSS: …
/v1/models: wall …  bytes …  RSSΔ …
normal: wall …  bytes …  RSSΔ …  call_logs+1? …
stream: TTFB …  bytes …  frames …  terminal? …  RSSΔ …
responses: …  large-request: peak …  status …
compression: …  mcp: …  a2a: …
recovery RSS: …  VmHWM: …  Threads: …  FDs: …
```

---

## 5. C backend budget guidance (design targets, not measurements)

- Resident core (router + HTTP + SQLite + one active request, no providers
  loaded beyond static tables): aim **< 8 MiB idle RSS** on Linux; stretch
  **< 4 MiB** for the iOS target before caches warm. **[design target]**.
- Per-request arena: cap ingest (e.g. 8 MiB default, matching the TS clamp
  floor) + bounded stream buffers (e.g. 16–64 KiB per direction, mirroring
  the 16k HWM) + explicit spill-to-tempfile above threshold.
  - Arena primitive implemented (Task 012, `native/src/arena.c`,
    `omni_arena_*`): explicit finite capacity, clean NULL failure on
    exhaustion, no growth, no heap fallback, address-based alignment,
    reset-to-reuse, borrowed-or-single-owned backing. Request wiring comes
    with the first slice; until then this is the primitive only.
  - Byte-buffer primitive implemented (Task 013, `native/src/bytebuf.c`,
    `omni_bytebuf_*`): bounded reusable staging for future socket receive
    and incremental parsing — linear read/write offsets, hard cap with no
    growth path, tail-only append (explicit compact, never implicit),
    zero-copy read/write views, commit/consume/compact/reset, borrowed-or-
    single-owned backing, lifetime high-water. Task 017 adds
    `native/src/recv.c` as the allocation-free consumer: one nonblocking
    `recv()` writes directly into the writable tail and commits the exact
    positive count; bounded drain, EOF, would-block, buffer-full, and fatal
    receive outcomes remain explicit. HTTP/parser wiring is still later.
- Caches: global byte budget (e.g. single-digit MiB default on iOS, higher
  on Linux via config), per-cache caps, idle eviction; heavy caches
  (catalog, embeddings) releasable.
- SQLite: `cache_size` small (e.g. `-2048` ≈ 2 MiB), `mmap_size` 0 on iOS,
  WAL checkpoint on idle; measure with `pmap` + DB file sizes.
- Concurrency: low fixed thread count (e.g. accept + worker pool sized to
  CPUs, min 2), small configurable stacks, bounded connection pool, bounded
  stream count × buffer.
- Verify all of the above with the §4 procedures against the C backend once
  the first slice lands (MIGRATION_PLAN §5 harness records RSS/HWM/threads/
  FDs per case).

## 6. Prior art already in the repo (read before designing)

- PII learnings (`AGENTS.md`): bounded non-overlapping regexes (ReDoS),
  snapshot-vs-delta SSE handling (sanitize final snapshots standalone to
  avoid duplication), `resetDbInstance()` + handle cleanup in tests (or the
  Node runner hangs).
- Heap-guard calibration (#3052), #7847 body-clone incident, #6409 heap≠RSS,
  #10353 heap-limit conflict, #10627 cold-import stall, #9045 streaming
  backup export (`resourceUsage().maxRSS` assertion example).

---

## 7. Measured baseline (Task 002, 2026-09-17) **[measured]**

> Provenance rule for this section: every number in §7.1–§7.5 is a **measured
> value** from 3 clean runs of `npm run bench:reference-baseline` on the
> machine in §7.2. Everything else in this document keeps its original
> provenance — §1–§2 constants are **source-derived expectations**, §5 budgets
> are **design targets (estimates)**, and §7.6 lists **unmeasured workloads**.
> Do not quote §5 budget numbers as measurements, and do not quote §7 numbers
> as guarantees for other machines, modes, or revisions.

### 7.1 Reproduction

```bash
npm run bench:reference-baseline -- --runs 3 --port 21138 --out /tmp/omniroute-baseline/<stamp>
```

What the tool does (external measurement only — no production code is
touched, nothing is injected into the server):

1. Spawns `node --max-old-space-size=8192 scripts/dev/run-next.mjs dev`
   (the same command `npm run dev` uses) with a fresh per-run `DATA_DIR`,
   random per-run `JWT_SECRET` / `API_KEY_SECRET` / `INITIAL_PASSWORD`,
   and operator secrets blanked. The server PID is tracked directly from
   `spawn()` — never matched by process name.
2. Waits deterministically: liveness `GET /api/health` → readiness
   `GET /api/health/ping`, with a finite deadline; premature server exit is
   detected at every phase and reported as `measurement_failure` (distinct
   from HTTP `request_failure`). Missing samples are recorded as `null`,
   never zero.
3. Samples Linux `/proc/<pid>/status` (`VmRSS`, `VmHWM`, `Threads`) and
   `/proc/<pid>/fd` at each phase; polls VmRSS every 50 ms while
   `GET /v1/models` is in flight for the workload peak. Descendant PIDs are
   discovered via `/proc/*/stat` and reported **separately** as an explicitly
   labeled aggregate (shared pages double-count, so it is an upper bound).
4. Provisions a real client API key through the product's own flow
   (initial-password login → dashboard session → `GET /api/auth/csrf` →
   `POST /api/keys`); no credentials are fabricated and no key material is
   written to artifacts. A `503 catalog_build_timeout` (see
   `src/app/api/v1/models/catalogCache.ts`) is retried once after the
   advertised `Retry-After`, as a representative client would.
5. Cleans up with `SIGTERM` → `SIGKILL` on the spawned PID and its
   starttime-verified descendants only, then verifies no leftovers remain.

Timings per run: readiness → 60 s stabilize → 300 s real idle (not
extrapolated) → models workload → 60 s recovery. About 8–10 minutes per run.

### 7.2 Environment (recorded by the tool, `summary.json:meta`)

- git commit: `b6c226515479c1407f40e87375dc5f93ccc3374b`
- OmniRoute version: `3.8.51` (`package.json`)
- OS / kernel / arch: Linux `7.2.4-zen2-1-zen`, x64, 8 CPUs, ~16 GiB RAM
- Node `v26.8.2`, npm `12.0.2`
- Run mode: **dev** (`scripts/dev/run-next.mjs dev`,
  `--max-old-space-size=8192`) — not production standalone
- `OMNIROUTE_MEMORY_MB`: unset; `NODE_OPTIONS`: unset
- No secrets recorded.

### 7.3 Metric semantics

- `RSS` = `VmRSS` of the server PID: whole-process resident set including
  shared mappings. **Not** V8 heap — do not compare with `bench:heap-body`
  or `heap-growth` numbers.
- `peak RSS` = `VmHWM` of the server PID: kernel lifetime high-water mark.
  It includes the first-boot Turbopack compile (§7.5), so it is a boot-peak
  bound, not a steady-state bound.
- `workload peak` = max VmRSS polled at 50 ms while the models request (plus
  a documented single retry, if any) is in flight.
- `threads` = `Threads:` count; `fds` = entries in `/proc/<pid>/fd`.
- `tree aggregate` = sum of descendant VmRSS, labeled `AGGREGATE`.

### 7.4 Results (MiB; individual samples run-1 / run-2 / run-3)

| Phase                                    | Run 1  | Run 2  | Run 3  | Min    | Median | Max    |
| ---------------------------------------- | ------ | ------ | ------ | ------ | ------ | ------ |
| Startup, 2 s after spawn (pre-readiness) | 101.6  | 340.0  | 93.9   | 93.9   | 101.6  | 340.0  |
| Startup, at readiness                    | 967.1  | 873.2  | 885.1  | 873.2  | 885.1  | 967.1  |
| Initial idle (ready + 60 s)              | 838.8  | 789.4  | 746.3  | 746.3  | 789.4  | 838.8  |
| Idle after 5 min (real 300 s wait)       | 422.5  | 788.4  | 715.1  | 422.5  | 715.1  | 788.4  |
| Pre-`/v1/models`                         | 422.5  | 788.4  | 715.1  | 422.5  | 715.1  | 788.4  |
| `/v1/models` workload peak               | 729.1  | 962.4  | 876.2  | 729.1  | 876.2  | 962.4  |
| Post-`/v1/models`                        | 729.2  | 963.4  | 856.2  | 729.2  | 856.2  | 963.4  |
| Recovery (models + 60 s)                 | 671.1  | 963.5  | 855.8  | 671.1  | 855.8  | 963.5  |
| Lifetime VmHWM (boot compile included)   | 2086.5 | 2050.0 | 1998.2 | 1998.2 | 2050.0 | 2086.5 |

`/v1/models` request detail (all `200 OK`, authenticated with a provisioned
key, body `214663` bytes each run): wall `1932 / 2453 / 3935` ms
(min / median / max). TTFB ≈ wall (single JSON body). No `503` retry was
needed on any run.

Derived deltas (informational, n=3 — no claim of significance):

- Request cost (post − pre): `+306.7 / +175.0 / +141.1` MiB.
- 60 s retention (recovery − pre): `+248.6 / +175.1 / +140.7` MiB — the
  catalog response memory is still resident 60 s later in all runs.
- 60 s release (recovery − post): `−58.1 / ~0 / ~0` MiB.

Outliers are kept, not hidden: run-1 released ~416 MiB during the 5-minute
idle (838.8 → 422.5) while runs 2–3 stayed flat; run-2 sampled 340 MiB at
2 s after spawn (warm Turbopack disk cache → faster boot, liveness in
176 ms vs 2079 ms / 908 ms). With n=3, treat the idle-5-min spread
(422–788 MiB) as idle-GC timing variance, not as two populations.

### 7.5 Threads, FDs, children, DATA_DIR

- Threads at recovery: `22 / 22 / 22`. FDs at recovery: `47 / 47 / 47`.
  (Boot transient: 24–25 threads / 53–55 FDs at readiness, settling to 22/47.)
- Exactly one child process in every sample: `esbuild` (Next dev
  transpiler service), 3–13 MiB RSS. Tree aggregate is 3–13 MiB in all
  samples — negligible next to the ~700–990 MiB main process, and always
  reported separately, never mixed into the main-process numbers.
- Fresh per-run `DATA_DIR` settled at ~6.1 MiB on disk (all runs).

### 7.6 Workloads NOT MEASURED (and why)

- Normal/streaming chat completion, `/v1/responses`, large Codex-shape
  request over HTTP, compression over HTTP, MCP/A2A over HTTP: all need a
  configured (and usually billable) upstream provider. Fabricating provider
  responses to obtain completion measurements is explicitly out of scope —
  that would be provider integration testing, not a memory baseline.
- V8-heap side of the large-request shape is covered separately by
  `npm run bench:heap-body` (heap, not RSS — not comparable to §7.4).
- Production standalone mode (`npm start`): not measured. The §7 numbers
  are dev-mode numbers (Turbopack + `esbuild` child resident); expect
  standalone to differ.

### 7.7 Relation to the cited `200–400 MB` statement

`docs/ops/VM_DEPLOYMENT_GUIDE.md:432-435` reports idle `200–400 MB` RSS at
`OMNIROUTE_MEMORY_MB=512` — that provenance is unchanged: it is a
**cited production-standalone deployment note, not a Task 002 measurement**.
It is not contradicted by §7.4 (dev mode, unset memory cap, 8 GiB heap
allowance, dev compiler resident) and it is not confirmed by §7.4 either.
A standalone-mode baseline on comparable hardware is future work.

### 7.8 Status of the previously reported `bench:highwatermark` issue

Confirmed precisely in Task 002: the `bench:highwatermark` target in
`package.json` references a benchmark file under `scripts/perf/` which has
never existed on disk — upstream commit `e2e330a05` added the script line
(package.json-only, +1 line) without its file; no commit ever created it;
it is not generated (no generator references it); no same-purpose file
exists under another name (other `highWaterMark` hits are SSE/stream
buffer-size options). Pre-existing upstream omission, unrelated to this
task. Verify with:

```bash
grep -n "bench:highwatermark" package.json
ls scripts/perf/benchmark-highwatermark.ts
git show e2e330a05 -- package.json
git log --all --oneline -S 'benchmark-highwatermark'
```

The new `bench:reference-baseline` target does not replace it (different
purpose: server-process RSS baseline vs the unknown intent of the dangling
target), so the dangling target is left untouched — not recreated, not
removed.

---

_Next: MIGRATION_PLAN.md (incremental path + harness)._

(End of file - total 501 lines)
