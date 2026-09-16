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

| Tool                     | Command                                                                                                                           | What it actually measures                                                        | Memory relevance                           |
| ------------------------ | --------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------- | ------------------------------------------ |
| request-body heap bench  | `npm run bench:heap-body` (`node --expose-gc --import tsx/esm scripts/perf/request-body-heap.ts [--json] [--max-retained-mib N]`) | V8 retained heap after ×4 `gc()` per clone mechanism on the #7847 corpus         | Direct: per-target body-copy cost          |
| deterministic corpus     | `scripts/perf/agentPayloadCorpus.ts` (+ `tests/unit/heap-benchmark-corpus.test.ts`)                                               | LCG corpus, `INCIDENT_SHAPE={729 msgs, 86 tools, 527 …}`                         | Repro for the above                        |
| routing-events bench     | `npm run bench:routing-events` (`scripts/perf/routing-events-bench.ts`)                                                           | `perf_hooks` µs/op + ops/s for factor calc + event dispatch + OTel enqueue       | Event-overhead ceiling                     |
| compression bench        | `npm run bench:compression` (`bun scripts/compression/benchmark.ts`)                                                              | token savings/retention per engine on fixed corpus                               | Quality/cost, **not** RAM                  |
| compression budget gate  | `npm run check:compression-budget` (+ `compression-budget-baseline.json`, 2% tolerance)                                           | tokens/task vs baseline                                                          | Guards against prompt bloat (indirect RAM) |
| heap leak gate           | `npm run test:heap` (`tests/integration/heap-growth.test.ts`)                                                                     | heap growth over 500 SSE streams (<20 MB)                                        | Direct: stream-path leaks                  |
| video-bridge benches     | `scripts/perf/video-bridge-{bench,fu07-eval,contact-sheet-eval,promotion-eval}.ts`                                                | `memoryUsage/maxRSS/cpu/latency`, `ffmpeg` child `maxRssKiB` via `/usr/bin/time` | Media-path peak RSS                        |
| bundle/size ratchets     | `npm run check:bundle-size` (+ `.size-limit.json`), `check-file-size.mjs`                                                         | gzip bytes vs `quality-baseline.json`                                            | Code size, not RSS                         |
| router/compression evals | `npm run eval:router*`, `npm run eval:compression`                                                                                | AIQ/cost/latency, quality/cost/save (spend-gated)                                | Regression context                         |
| pressure probes          | `open-sse/utils/{heapPressure,resourcePressure*}.ts`, `chatBodyAdmission.ts`                                                      | live `heapUsed/limit`, cgroup/PSI                                                | Runtime guard behavior                     |
| `wtfnode@0.10.1`         | devDependency, no wired script                                                                                                    | open handles on demand                                                           | Leak debugging                             |

Not present: `clinic`, `autocannon`, `k6` configs, Lighthouse (only nightly
mention in `docs/ops/QUALITY_GATE_PLAYBOOK.md:85,159` and unrelated string
hits). `bench:highwatermark` is dangling (target file missing on disk —
verify before use).

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

_Next: MIGRATION_PLAN.md (incremental path + harness)._
