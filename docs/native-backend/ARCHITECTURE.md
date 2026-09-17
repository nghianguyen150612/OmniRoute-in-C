# OmniRoute Backend Architecture (Reference Implementation Audit)

> Task 001 — repository audit for the native C backend.
> Reference implementation: TypeScript / Node / Next.js 16, version `3.8.51`
> (`package.json:2-4`). This document describes **confirmed behavior** traced to
> source. Claims are tagged **[confirmed]**, **[likely]** (strong evidence, one
> hop not fully read), or **[unknown — needs runtime verification]**.
>
> Audit date: 2026-09-16. Counts below were measured on this checkout; they
> drift as upstream moves. Re-measure with the commands in §0.

## 0. How to re-measure this document

```bash
find src/app/api -name route.ts | wc -l        # 699 on audit date
ls src/app/api/ | wc -l                         # ~103 top-level families
find src/app/api/v1 -name route.ts | wc -l      # 98 on audit date
ls open-sse/executors/ | wc -l                  # 136 entries (files+dirs)
ls open-sse/services/*.ts | wc -l               # 236 top-level .ts files
ls open-sse/config/providers/registry/ | wc -l  # 256 provider dirs
ls src/lib/db/*.ts | wc -l                      # 127 domain modules
ls src/lib/db/migrations/*.sql | wc -l          # 176 (001..179 with gaps)
ls tests/unit/*.test.ts | wc -l                 # 4519 (includes subdirs? see note)
node -p "require('./package.json').version"
```

Note: `tests/unit/*.test.ts` glob without `**` undercounts nested suites;
`ls tests/unit/*.test.ts | wc -l` returned 4519 on this checkout which suggests
the shell expanded recursively via config — **[unknown]** exact nesting; use
`find tests/unit -name '*.test.ts' | wc -l` to re-verify.

---

## 1. Application entry points **[confirmed]**

There is no single `server.js` in the repo root. There are four cooperating
entry layers:

### 1.1 CLI entry — `bin/omniroute.mjs`

- `package.json:6-9` declares `bin: { omniroute: bin/omniroute.mjs }`.
- `bin/omniroute.mjs:97-98,358-363` bootstraps `tsx/esm` + `setupPolyfill` +
  alias resolution, then dispatches to `bin/cli/program.mjs`.
- Subcommands live in `bin/cli/commands/`; `serve.mjs:40-42,51,108-109`
  is the default command. It resolves `APP_DIR` (`dist/server.js` vs app dir),
  defaults `PORT/DASHBOARD_PORT/API_PORT` to `20128`, and spawns
  `server-ws.mjs || server.js` in foreground/daemon/supervised modes
  (`serve.mjs:146-184,253-280`).
- Other CLI surfaces: `bin/reset-password.mjs`, `bin/cli/runtime/
processSupervisor.mjs:77` (`spawn`), `--mcp` stdio mode (see §16).

### 1.2 Dev server — `scripts/dev/run-next.mjs` **[confirmed]**

- `package.json:93` → `dev: node --max-old-space-size=8192
scripts/dev/run-next.mjs dev`; `start: node scripts/dev/run-next.mjs start`.
- `run-next.mjs:49-66,102-138` parses `mode`, calls `bootstrapEnv()` +
  `resolveRuntimePorts()` + `withRuntimePortEnv()`, forces
  `NODE_ENV=development|production`, selects Turbopack vs webpack
  (`OMNIROUTE_USE_TURBOPACK != 0 && !Bun`), then `next({dev, dir, hostname,
port, turbopack|webpack})`.
- It builds its **own `http.Server`** (`run-next.mjs:193-205`):
  `wrapRequestListenerWithHeadResponseGuard(attachRequestStreamGuards +
maybeHandleDisallowedMethod + stampPeerIp + requestHandler)`.
- `server.on("upgrade", ...)` wires `responsesWsProxy → wsBridge →
upgradeHandler` (`run-next.mjs:215-228`).
- Timeouts come from `getMainServerTimeoutConfig()` (`keepAliveTimeout /
headersTimeout`), then `server.listen(dashboardPort, hostname)`.
- Extra edge guards imported at `run-next.mjs:12-22`:
  `http-method-guard.cjs`, `head-response-guard.cjs`, `peer-stamp.mjs`,
  `httpClientAbortGuard.mjs`, `tls-options.mjs`, `responses-ws-proxy.mjs`.
- `prepareWithHeal()` + `installProcessCrashGuard()` (`run-next.mjs:155-181`)
  handle Turbopack cache healing and crash logging.

### 1.3 Production server — Next standalone wrapper **[confirmed]**

- `package.json:108` → `build: node scripts/build/build-next-isolated.mjs`.
  That script (`build-next-isolated.mjs:96-145,267-385`) spawns
  `node_modules/next/dist/bin/next build --turbopack|--webpack` (flag from
  `resolveNextBuildBundlerFlag()`, `OMNIROUTE_USE_TURBOPACK=0` forces webpack),
  with `OMNIROUTE_BUILDING=1`, `NEXT_DIST_DIR=.build/next`, then calls
  `assembleStandalone()`.
- `next.config.mjs:15,223` sets `distDir=.build/next`, `output:"standalone"`
  (skipped only for `OMNIROUTE_BUILD_PROFILE=contributor`).
- `scripts/build/assembleStandalone.mjs` is the single source for the runnable
  artifact: copies `.next/standalone → outDir`, `.next/static`, `public/`,
  `better-sqlite3` natives, `migrations`, `src/mitm/server.cjs`,
  `scripts/dev/run-standalone.mjs → dev/`,
  `standalone-server-ws.mjs → server-ws.mjs`, `peer-stamp.mjs`,
  `responses-ws-proxy.mjs`, etc.
- `Dockerfile:238,269` copies the standalone tree and runs
  `node dev/run-standalone.mjs` with `PORT=20128`, `DATA_DIR=/app/data`.
- `scripts/dev/run-standalone.mjs:14-36` → `bootstrapEnv()` →
  `resolveRuntimePorts()` → spawns `server-ws.mjs || server.js`.
- `scripts/dev/standalone-server-ws.mjs:1-43` wraps `http.createServer` with
  peer-stamp, method/head guards, TLS (`tls-options.mjs`), `responsesWsProxy`,
  WebDAV.

### 1.4 Boot hook — `src/instrumentation*.ts` **[confirmed]**

- `src/instrumentation.ts:18-25` → `register()` imports
  `./instrumentation-node` when `NEXT_RUNTIME == "nodejs"`.
- `src/instrumentation-node.ts:330-419,427-769` → `registerNodejs()`:
  patches `proxyFetch`, calls `registerQuotaFetchers()`,
  `ensureDbReadyForBoot()`, `ensureSecrets()`, `initGracefulShutdown()`,
  `initApiBridgeServer()`, spend batch writer, guardrails, skills, settings
  hydration, schedulers, then `markServerReady()`.
- `src/proxy.ts:14-21` additionally warms `getCachedSettings()` fire-and-
  forget at proxy boot (regression fix #10627: cold native SQLite load on the
  first request path stalled all proxied requests).

**Dependency direction:** `bin/* → scripts/dev/* → next() → src/proxy.ts
(authz) → src/app/api/**/route.ts → open-sse/handlers/* → open-sse/services/*
→ open-sse/executors/* + open-sse/translator/* → upstream fetch`.
`src/instrumentation-node.ts` sits beside this chain and pre-warms DB,
secrets, quota fetchers, and schedulers before traffic arrives.

---

## 2. HTTP server / runtime architecture **[confirmed]**

- Runtime: Node.js `>=22.22.2 <23 || >=24.0.0 <27` (`package.json` engines),
  ESM only (`"type": "module"`). Bun `1.4.0` is a pinned exact devDependency
  used **only** for a small allow-list of gate/generator scripts and the
  `test:bun:db` smoke suite — not a supported server runtime.
- Framework: `next@16.3.3`, `react@19.2.8` (`package.json:335,348-349`).
  All backend endpoints are **Next.js App Router Route Handlers**
  (`src/app/api/**/route.ts` exporting `GET/POST/PUT/PATCH/DELETE/OPTIONS`).
  There is no `pages/` router and no separate Express/Fastify server; the
  custom `http.Server` in §1 only wraps Next's request/upgrade handlers.
- Default port `20128` (API + dashboard same port). `DATA_DIR` defaults to
  `~/.omniroute/` (`src/lib/dataPaths.ts`), overridable by env.
- No global Next middleware file. Interception is the Next 16 `proxy`
  convention (see §3).
- Dev/standalone servers append `OMNIROUTE_MEMORY_MB`-derived
  `--max-old-space-size` (see `scripts/build/runtime-env.mjs:39-130`,
  `build-next-isolated.mjs:200-213`, `run-standalone.mjs:19`). The comment at
  `build-next-isolated.mjs:200-213` explicitly notes `#6409 heap != RSS`.

---

## 3. Authz pipeline / proxy / CORS **[confirmed]**

### 3.1 `src/proxy.ts` matcher

`src/proxy.ts:23-52` exports `proxy(req) => runAuthzPipeline(req,
{enforce:true})` with a case-insensitive-trick matcher (GHSA-jvqc-mp9f-q936):

```
"/", "/dashboard/:path*", "/home", "/home/:path*",
"/api/:path*", "/:v1seg([vV]1)/:path*", "/:v1seg([vV]1)",
"/:v1betaseg([vV]1[bB][eE][tT][aA])/:path*",
"/:chatseg([cC][hH][aA][tT])/:path*", "/:respseg(...)/:path*",
"/:codexseg([cC][oO][dD][eE][xX])/:path*", "/:modelsseg([mM][oO][dD][eE][lL][sS])"
```

Keep in sync with `next.config.mjs` rewrites and
`src/server/authz/classify.ts` (comment in file).

### 3.2 `src/server/authz/pipeline.ts:263-445`

1. `/ → /dashboard` redirect; 2. `classifyRoute()`; 3. `isDraining()`;
2. `checkBodySize()`; 5. strip trusted headers (AUTHZ_TRUSTED_HEADERS,
   PEER_IP, VIA_PROXY, CLI_TOKEN — header names, not env vars);
3. stamp ROUTE_CLASS, REQUEST_ID, PEER_LOCALITY, TRUSTED_PEER_IP
   (internal request stamps, not env vars); 7. `OPTIONS → 204`;
4. `checkRequestIP`; 9. `POLICIES[routeClass].evaluate()`; 10. CSRF/origin
   check for `MANAGEMENT + dashboard_session + unsafe method`;
5. `NextResponse.next({headers})`.

### 3.3 Classification — `src/server/authz/classify.ts:7-134`

Aliases `/v1*`, `/v1beta`, `/codex`, `/chat/completions`, `/responses`,
`/models` → `/api/v1/*` = `CLIENT_API`. `/api/*` + public API routes =
`PUBLIC`, else `MANAGEMENT`.

### 3.4 Route guard tiers — `src/server/authz/routeGuard.ts:33-326`

- LOCAL_ONLY_STAR (spawn-capable: `/api/mcp/`, `/api/cli-tools/runtime/`,
  `/api/services/`), ALWAYS_PROTECTED_STAR, `isLocalOnlyPath()`.
  Loopback enforcement runs **before** any auth check, so a leaked JWT via a
  tunnel cannot trigger process spawning (Hard Rules #15/#17).
- Policies: `policies/clientApi.ts`, `policies/management.ts`,
  `policies/public.ts`; helpers `headers.ts`, `peerStamp.ts`,
  `peerContext.ts`, `csrf.ts`, `accessTokenAuth.ts`, `accessScopes.ts`,
  `assertAuth.ts`, `context.ts`, `types.ts`.

### 3.5 CORS — two layers **[confirmed]**

- Central: `src/server/cors/origins.ts:19-215` (`CORS_ALLOW_ALL`,
  `CORS_ALLOWED_ORIGINS` + `CORS_ORIGIN`, `setRuntimeAllowedOrigins()`,
  `resolveAllowedOrigin()`, `applyCorsHeaders(res, req, relaxForTokenAuth)`).
  `pipeline.ts:292-301` relaxes origin for `CLIENT_API` and public-readonly.
- Route-static: `src/shared/utils/cors.ts:11-24` (`CORS_HEADERS` without
  `Allow-Origin`, `handleCorsOptions() → 204`). Each route spreads
  `CORS_HEADERS`; the proxy overlays the real origin.
- Security headers via `next.config.mjs:21-65,487-511` (CSP,
  X-Frame-Options, nosniff, Referrer, Permissions, HSTS, embed mode).

### 3.6 Rewrites — `next.config.mjs:704-783` **[confirmed]**

```
/chat/completions → /api/v1/chat/completions
/responses, /responses/:path* → /api/v1/responses...
/models → /api/v1/models
/v1/v1/:path*, /v1/:path*, /v1 → /api/v1...
/codex/:path* → /api/v1/responses
/v1beta/:path* → /api/v1beta...
/anthropic/:path* → /api/anthropic... (→ catch-all JSON 404, #6405/#6424)
```

Unknown root paths hit the api catch-all route (brace form:
/api/{omnirouteApiCatchAll}) which returns
`application/json` with `error.type === "not_found"` instead of dashboard HTML.

---

## 4. Canonical route pattern **[confirmed]**

Sampled in `src/app/api/v1/chat/completions/route.ts`,
`src/app/api/v1/embeddings/route.ts`,
`src/app/api/v1/images/generations/route.ts`, `src/app/api/combos/route.ts`,
`src/app/api/keys/route.ts`:

```
Route → OPTIONS preflight (CORS_HEADERS / handleCorsOptions)
  → Content-Type guard (415 unless application/json, #6414)
  → admission (chat) / body-size gate (pipeline)
  → request.json() exactly once → permissive Zod shape
  → deep validation in handler → auth (extractApiKey / isValidApiKey /
     enforceApiKeyPolicy / requireManagementAuth)
  → prompt-injection guard → handler delegation (open-sse/handlers/* or
     src/lib/db/*)
Errors via buildErrorBody() / errorResponse() / sanitizeErrorMessage()
  (open-sse/utils/error.ts) — never raw err.stack/message (Hard Rule #12).
```

---

## 5. API route families

### 5.1 Scale

- `src/app/api/`: ~103 top-level families, 699 `route.ts` files.
- `src/app/api/v1/`: 98 `route.ts` files, ~50 sub-families (see §5.2).
- Full directory listing (audit date):

```
a2a acp admin agent-skills analytics assess auth batches cache chaos cli
cli-tools cloud codex combos compliance compression conductor context
conversations copilot cursor-cli dahl db db-backups discovery docs evals
fallback files free-models free-provider-rankings free-tier gamification
github-skills guardrails headroom health init intelligence internal issue-agent
jobs keys local log-export logs mcp memory middleware modality-bridge
model-capability-overrides model-combo-mappings models monitoring network
oauth omniroute [...omnirouteApiCatchAll] openapi playground plugins policies
pricing provider-metrics provider-models provider-nodes providers provider-stats
proxy-fallback quota radar rate-limit rate-limits relay resilience restart
routing search services session-pools sessions settings shutdown skills storage
sync synced-available-models system tags telegram telemetry token-health tools
translator tunnels upstream-proxy usage v1 v1beta version-manager vnc-session
webhooks
```

### 5.2 `/v1` sub-families **[confirmed — directory listing]**

```
accounts agents antigravity api audio auto-combo batches chat classify combos
completions embeddings explain files images issues management me messages models
moderations multimodal-embeddings muse-code music ocr
[...omnirouteCatchAll] provider-plugin-manifest providers quotas
registered-keys relay rerank responses route.ts search segment session-leases
speech-to-text text-to-speech video-bridge videos voices vscode web ws
_helpers _shared
```

Key delegations:

| Route                                                   | File                                                                                                                                                                                                                                                                                               | Delegates to                                                                                                                                |
| ------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `POST /v1/chat/completions`                             | `src/app/api/v1/chat/completions/route.ts:91-283`                                                                                                                                                                                                                                                  | `handleChat()` (`src/sse/handlers/chat.ts`) → `handleChatCore()` (`open-sse/handlers/chatCore.ts`)                                          |
| `POST /v1/responses`, `POST /v1/responses/[...path]`    | `src/app/api/v1/responses/route.ts`                                                                                                                                                                                                                                                                | same `handleChat` + Responses transformer (`open-sse/transformer/responsesTransformer.ts`)                                                  |
| `POST /v1/messages`, `.../count_tokens`                 | `src/app/api/v1/messages/route.ts`                                                                                                                                                                                                                                                                 | `handleChat` (Claude-native)                                                                                                                |
| `POST /v1/embeddings`, `POST /v1/multimodal-embeddings` | `src/app/api/v1/embeddings/route.ts`                                                                                                                                                                                                                                                               | `createEmbeddingResponse()` (`src/lib/embeddings/service.ts`)                                                                               |
| `POST /v1/completions`                                  | `src/app/api/v1/completions/route.ts`                                                                                                                                                                                                                                                              | `handleChat` (legacy completions)                                                                                                           |
| `GET /v1/models`, `GET /v1/models/[...model]`           | `src/app/api/v1/models/route.ts` + `catalog.ts`, `catalogSyncedCoverage.ts`, `catalogProviderMaps.ts`                                                                                                                                                                                              | `getUnifiedModelsResponse()` (+ `after()` background refresh)                                                                               |
| `POST /v1/images/generations\|edits\|upscale`           | `src/app/api/v1/images/generations/route.ts`                                                                                                                                                                                                                                                       | `handleImageGeneration()` (`open-sse/handlers/imageGeneration.ts`), `parseImageModel/getImageProvider` (`open-sse/config/imageRegistry.ts`) |
| videos, music, voices, audio, speech                    | `.../videos`, `.../music`, `.../voices`, `.../audio`, `.../speech-to-text`, `.../text-to-speech/`                                                                                                                                                                                                  | `videoCombo/speechCombo/imageCombo` (`open-sse/services/videoCombo.ts`, `speechCombo.ts`, `imageCombo.ts`)                                  |
| `POST /v1/search`                                       | `src/app/api/v1/search/route.ts`                                                                                                                                                                                                                                                                   | `SEARCH_PROVIDERS` (`open-sse/config/searchRegistry.ts`)                                                                                    |
| files                                                   | `src/app/api/v1/files/**`                                                                                                                                                                                                                                                                          | `createFile/listFiles/formatFileResponse` (`src/lib/db/files.ts`)                                                                           |
| moderations, rerank, ocr, segment, classify, explain    | `.../moderations`, `.../rerank`, `.../ocr`, `.../segment`, `.../classify`, `.../explain/`                                                                                                                                                                                                          | specialty handlers + `getSpecialtyModelsResponse` (`src/app/api/v1/_shared/specialtyCatalog.ts`)                                            |
| `POST /v1/providers/[provider]/...`                     | `.../providers/[provider]/...`                                                                                                                                                                                                                                                                     | pinned-provider `handleSingleModel/handleChat`                                                                                              |
| `POST /v1/relay/chat/completions(+bifrost)`             | `src/app/api/v1/relay/chat/completions/route.ts`                                                                                                                                                                                                                                                   | relay handler                                                                                                                               |
| vscode token paths                                      | `.../vscode/[token]/*`, `.../raw/[token]/*`, `.../combos/[token]/[[...slug]]`                                                                                                                                                                                                                      | token-in-path wrappers around `handleChat/models/combos`                                                                                    |
| agents, batches, combos, quotas, keys, me, issues       | `.../agents`, `.../batches`, `.../combos`, `.../accounts`, `.../quotas`, `.../registered-keys`, `.../me`, `.../issues`, `.../api/chat`, `.../management`, `.../web`, `.../ws`, `.../video-bridge`, `.../auto-combo`, `.../muse-code`, `.../provider-plugin-manifest`, `.../[...omnirouteCatchAll]` | `src/lib/db/*` + `combo.ts` + cloud-agent/batch handlers                                                                                    |

### 5.3 Management / dashboard families (representative)

Management routes follow the same CORS→Zod→auth→DB pattern with
`requireManagementAuth` or scoped-key checks. Notable groups:

- `auth/` (login/logout/status/csrf/oidc), `keys/` (`src/lib/db/apiKeys.ts`),
  `combos/` (`src/lib/db/combos.ts` + `combo.ts:validateComboDAG`),
  `providers/` (56 files), `settings/` (76 files), `usage/` (27 files),
  `models/` + `provider-models/` + `provider-nodes/` + `model-capability-
overrides/` + `model-combo-mappings/`, `oauth/` (16 files),
  `services/` (50 files — embedded-service lifecycle, local-only),
  `cli-tools/` (32 files — local-only), `mcp/` (6 files),
  `tools/` (35 files), `memory/` (9), `compression/` (7), `context/` (14),
  `skills/` (11), `a2a/` (5), `webhooks/` (5), `log-export/` (6),
  `db-backups/` (4), `tunnels/` (9), `telemetry/`, `monitoring/`, `health/`,
  `resilience/`, `rate-limit(s)/`, `quota/` (10), `plugins/` (8),
  `gamification/` (14), `evals/` (4), `guardrails/` (2), `conversations/` (3),
  `files/` + `batches/` (2 each), `sync/` (5), `version-manager/` (6).

Spawn-capable routes (`/api/mcp/`, `/api/cli-tools/runtime/`,
`/api/services/`) are `isLocalOnlyPath()`-gated (Hard Rules #15/#17).

---

## 6. Chat Completions execution path **[confirmed — traced end to end]**

`POST /v1/chat/completions` (`src/app/api/v1/chat/completions/route.ts:91-283`):

1. `await ensureInitialized()` — `initTranslators()` singleton.
2. `Content-Type: application/json` guard → 415 otherwise (#6414).
3. `resolveSessionId(request)` + `admitChatRequest()` — heap-aware admission
   - bounded ingest (see §14); exhaustion returns retryable response.
4. Single `request.json()` → permissive `chatCompletionsRouteShapeSchema`
   (`model?: string|null`, `messages?: unknown[]`, `.passthrough()`).
5. Retirement gates: `assertCommonChatGptWebModelAvailable`,
   `assertRuntimeModelProviderAvailable`.
6. `resolveModelAliasWithSeedFallbackOnBody` (alias → canonical).
7. `createInjectionGuard()` pre-check → 400 on block.
8. Streaming fork: `wantsStreaming = body.stream ||
acceptHeaderForcesStream()` → `handleChat(...)` wrapped with
   `withEarlyStreamKeepalive(OPENAI_*_FRAME)` + `withCompressionHeaderEcho`,
   else plain `handleChat` + compression echo.

`handleChat` (`src/sse/handlers/chat.ts:421-1454`):

- `withChatAdmission(handleChatImplementation)`; `resolveChatRequestBody`,
  `normalizeReasoningRequest`, `detectFormatFromUrl`; `messages[]` → 400.
- Combo branch: `resolveComboTargets/handleComboChat` else
  `handleSingleModelChat`; `executeChatWithBreaker → handleChatCore`.

`handleChatCore` (`open-sse/handlers/chatCore.ts`, 6142 lines split into
`requestSetup`, `memorySkillsInjection`, `providerExecutionPipeline`,
`streamingPipeline`, `nonStreamingProviderLeg` modules):

- cache check → rate limit → combo routing? → `translateRequest()` →
  `getExecutor()` → `executor.execute()` → upstream `fetch()` + retry/backoff
  → response translation → SSE stream or JSON.
- Responses API requests pass through `responsesTransformer.ts`
  `TransformStream` (see §15).

---

## 7. Responses API **[confirmed]**

- Routes: `POST /v1/responses`, `POST /v1/responses/[...path]`
  (`src/app/api/v1/responses/`), plus alias rewrites `/responses*` and
  `/codex/* → /api/v1/responses` (`next.config.mjs:710-733`).
- Same `handleChat` core as Chat Completions; output converted
  Chat→Responses event stream by
  `open-sse/transformer/responsesTransformer.ts:192`
  `createResponsesApiTransformStream()` (`response.created/in_progress/
output_item/delta/completed`, keepalive every 3s, `highWaterMark 16k`).
- Helpers: `open-sse/handlers/chatCore/responsesJsonToSse.ts`,
  `open-sse/utils/responsesStreamHelpers.ts`, `resolveResponsesApiModel`,
  `resolveStreamFlag`.

## 8. Models APIs **[confirmed — Task 003 traced `GET /v1/models` end to end]**

- `GET /v1/models`, alias `GET /models` (rewrites `next.config.mjs:719-741` →
  `/api/v1/models`; proxy class `CLIENT_API` via `src/proxy.ts` +
  `src/server/authz/classify.ts`).
- `src/app/api/v1/models/route.ts:GET` (explicit `HEAD → 200` empty-body probe,
  #6400; `OPTIONS` preflight) → `getUnifiedModelsResponse()` with Next `after()`
  as the background-refresh scheduler (`catalog.ts:192-253`).
- Two auth layers, verified live: (1) pipeline `clientApiPolicy`
  (`src/server/authz/policies/clientApi.ts:57-101`) — anonymous iff
  `REQUIRE_API_KEY=false` (default `"false"`,
  `src/shared/constants/featureFlagDefinitions.ts:17-26`), else `401 AUTH_002
{code,message,correlation_id}`; (2) route `getModelCatalogAuthRejection()`
  (`catalogRequest.ts:12-54`, driven by `isAuthRequired()` in
  `src/shared/utils/apiAuth.ts:406-456` + `settings.requireAuthForModels`) —
  `401 {message,type:"invalid_api_key",code:"invalid_api_key"}`. Malformed
  `Authorization` values fall through to anonymous (`extractApiKey`,
  `src/sse/services/auth.ts:3504-3539`; query `?token=` removed, #3300).
- `resolveCachedCatalogResponse()` (`catalogCache.ts:409-477`): cache key
  `prefix|isCodex|HMAC-fp(key)|configuredOnly|hideAuto|hideNoThink|page`;
  TTL `settings.cache.modelCatalogCacheTtlMs` else 60s; 30s
  stale-while-revalidate via `after()` post-flush; concurrent builds coalesced
  (#6408); 8s cold-build bound → `503 catalog_build_timeout` + `Retry-After`
  (#12627); generation-guarded against post-write joins; errors never served
  stale. Invalidation via `modelCatalogCacheVersion`
  (`src/lib/db/readCache.ts:256-300`).
- Builder `buildUnifiedModelsResponseCore()` (`catalog.ts:282-2074`): settings →
  connections (`getCachedRawProviderConnections`, lazy-decrypt view) + nodes →
  combos (+ nested resolution) → quota-exclusive short-circuit → auto/* loop →
  combo loop → synced map → static `PROVIDER_MODELS` loop → codex-native →
  synced → OpenRouter (only when active w/o synced) → embedding/image/rerank/
  audio/moderation/video/music registries → custom → alias-backed → managed
  fallback → per-key `isModelAllowedForKey` filter → `applyCatalogPostFilters`
  (`catalogResponse.ts`: `configuredOnly`, effort/no-thinking/cc-discovery/
  gateway variants+mirrors, `dedupeExactCatalogIds`) → `finalizeCatalogResponse`
  (per-entry `enrichCatalogModelEntry`, combo rows skip enrichment;
  `sortCatalogModelsProviderGrouped` — combo block first, then registry
  precedence, stable within group; `limit`/`after` page; `models:[]` iff Codex
  client) → `catalogJsonResponse` (`content-type` + `content-length`, chunked
  stream past 64 KiB).
- Contract facts (live-verified, fresh boot): `200` envelope
  `{object:"list",data}`; `id/object/created/owned_by` universal, `permission`
  (`[]`) + `root` absent on specialty rows; `X-Request-Id`,
  `X-Model-Catalog-Version: model-metadata-v1:<sync|static>` on every route
  response; CORS origin echoed iff credentialed request/preflight on this
  relaxed surface, absent for credential-less GET. Post-response work: `after()`
  refresh only (+ fire-and-forget cc-discovery counter). No upstream fetch on
  this path. Full fixture:
  `docs/native-backend/contracts/v1-models.contract.json`.
- `GET /v1/models/[...model]` (`[...model]/route.ts` → `modelById.ts`,
  catch-all join for slashful ids) is a separate route — not covered by Task 003.
- `GET /v1beta/models`, `GET /v1/muse-code/models`,
  `GET /v1/provider-plugin-manifest`, specialty catalog
  (`src/app/api/v1/_shared/specialtyCatalog.ts`).
- Provider-pinned: `GET /v1/providers/[provider]/models|limits`.

## 9. Provider abstraction **[confirmed]**

### 9.1 Declaration — `src/shared/constants/providers.ts` (507 lines)

Re-export hub over `providers/{noauth,oauth,apikey,local,search,audio,
upstream-proxy,cloud-agent,system,web-cookie}.ts`:

- Section arrays (`NOAUTH_PROVIDERS`, `OAUTH_PROVIDERS`, …) merged lazily into
  `AI_PROVIDERS` Proxy via `getOrCreateAiProviders()`.
- Zod-validated at load (`ensureProvidersValidated →
validateProviders(section,name)` from
  `src/shared/validation/providerSchema.ts`).
- Alias maps: `getProviderById / getProviderByAlias / resolveProviderId /
getProviderAlias`, lazy `ALIAS_TO_ID / ID_TO_ALIAS` Proxies.
- Families: `PROVIDER_CONNECTION_FAMILY_ALIASES` (e.g. `alibaba:[alibaba-cn]`,
  `xai:[xai-oauth,xao]`), `getProviderConnectionFamilyIds()`.
- Kind predicates: `FREE_APIKEY_PROVIDER_IDS`, `DUAL_AUTH_PROVIDER_IDS`,
  `IMAGE_ONLY_PROVIDER_IDS`, `AGGREGATOR_PROVIDER_IDS`,
  `ENTERPRISE_CLOUD_PROVIDER_IDS`, `VIDEO_PROVIDER_IDS`, `IDE_PROVIDER_IDS`,
  `EMBEDDING_RERANK_PROVIDER_IDS`, `SELF_HOSTED_CHAT_PROVIDER_IDS`,
  `OPENAI_COMPATIBLE_PREFIX / ANTHROPIC_COMPATIBLE_PREFIX /
CLAUDE_CODE_COMPATIBLE_PREFIX`, `isLocalProvider()`,
  `providerAllowsOptionalApiKey()`, `supportsBulkApiKey()`,
  `USAGE_SUPPORTED_PROVIDERS`.

### 9.2 Execution — `open-sse/executors/` (136 filesystem entries; the

code-derived executor-alias count reported by check-docs-counts is 113 —
different units: files+dirs on disk vs registered aliases) **[confirmed]**

- `registry.ts`: `registerExecutor / registerLazyExecutor /
loadRegisteredExecutor / hasRegisteredExecutor / getRegisteredExecutor /
listExecutorAliases` (Map + lazy loaders; order-stable golden in
  `tests/unit/executor-map-golden.test.ts`).
- `index.ts`: `lazyExecutors: Record<alias, () => Promise<BaseExecutor>>`
  (~100 aliases: `antigravity/agy, github, qoder, kiro, bedrock, codex,
codex-app-server, cursor, trae, glm, azure-openai, pollinations, opencode,
vertex, cliproxyapi, 9router/nr, grok-web, claude-web, gemini-web, ...`);
  `getExecutor(provider)` (+ guards `CHAT_UNSUPPORTED_CLOUD_AGENT_PROVIDERS`,
  `CHAT_UNSUPPORTED_SEARCH_PROVIDERS`), `hasSpecializedExecutor()`;
  re-exports `BaseExecutor`, `DefaultExecutor`.
- `base.ts (+base/)`: `BaseExecutor.execute()` — param strips
  (`providerFieldStrips.ts`), thinking caps, fingerprint/CLI compat
  (`cliFingerprints.ts`), alt formats, fetch-start timeout policy, free-window
  / rate-limit correction.
- `default.ts` + `defaultResolver.ts:getDefaultExecutor(provider)` (cached
  `DefaultExecutor(provider)`; falls back to `PROVIDERS.openai`), plus
  `default/` per-provider defaults and `credential.ts`.
- Fetch shims: `*fetch.ts` (`tavily-, firecrawl-, jina-reader-, tinyfish-,
nimble-, context7-, anysearch-fetch`), `forceResponsesUpstream.ts`,
  `accountRotation.ts`.

### 9.3 Translation — `open-sse/translator/` **[confirmed]**

- `formats.ts`: `OPENAI, OPENAI_RESPONSES, OPENAI_RESPONSE, CLAUDE, GEMINI,
CLOVA, CODEX, ANTIGRAVITY, KIRO, CURSOR`.
- `registry.ts`: `register(from,to,reqFn,resFn)`, `getRequestTranslator()`,
  `getResponseTranslator()` (key `from:to`).
- `bootstrap.ts`: side-effect imports for all `request/*` + `response/*`
  pairs; `index.ts` (931 lines): `translateRequest(...)` (thinking-budget →
  reasoning-directive → role-normalize → system-hoist → hub-and-spoke via
  OpenAI or direct + `filterToOpenAIFormat / prepareClaudeRequest /
normalizeOpenAIResponsesRequest` + tool-schema coercion + reasoning-replay
  cache), `translateResponse(...)`, `needsTranslation()`, `initState()`,
  `initTranslators()`.
- Helpers: `toolCallHelper` (IDs, orphaned results), `claudeHelper`,
  `openaiHelper`, `schemaCoercion`, `geminiHelper`, `strictSystemHoist`;
  `paramSupport.ts`, `webTools.ts`, `deepseekWebTools.ts`, `image/`.

---

## 10. Provider discovery / model registry **[confirmed]**

- Facade: `open-sse/config/providerRegistry.ts` (321 lines) over
  `open-sse/config/providers/{shared.ts,index.ts,registry/*}` (`REGISTRY`,
  256 entries).
- `generateLegacyProviders(): Record<string, LegacyProvider>`
  (format + baseUrl(s) + headers + oauth + chatPath + clientVersion);
  `generateModels(): Record<alias, RegistryModel[]>`;
  `generateAliasMap(): Record<id, alias>`.
- `isLocalProvider(baseUrl)` (`LOCAL_HOSTNAMES + isPrivateHost`);
  `getPassthroughProviders(): Set<string>`; `getRegistryEntry(provider)`
  (ID-or-alias); `getRegisteredProviders(): string[]`.
- Capability: `getRegistryModelThinkingEfforts /
getRegistryThinkingEfforts`, `providerUsesAuthoritativeLiveCatalog()`
  (default true), `getUnsupportedParams(provider, modelId)`,
  `requiresPlainStringContent()`, `getProviderCategory()`, type defs in
  `shared.ts` (812 lines: `RegistryEntry`, `RegistryModel` with
  `aliases/liveCatalogIds/toolCalling/supportsReasoning/
supportsVision/Audio/Video/contextLength/unsupportedParams/timeoutMs`).
- Alias layers exist in **both** `src/shared` and `open-sse/config`
  (`PROVIDER_ID_TO_ALIAS / ALIAS_TO_ID / ID_TO_ALIAS`), plus per-model
  `aliases[]`, `liveCatalogIds[]`, connection-family aliases, and
  `openai-compatible-* / anthropic-compatible-*` wildcards. Retired IDs are
  rejected via `rejectRetiredAutoComboCandidates()` / `modelLifecycle.ts`.
  **[likely]** the two alias layers can drift; retirement lists are the
  enforcement point — verify at runtime with `/v1/models`.

---

## 11. Routing strategies, fallback, combos, quotas **[confirmed]**

Source truth: `src/shared/constants/routingStrategies.ts`:

- `ROUTING_STRATEGY_VALUES` (20): `priority, weighted, round-robin,
context-relay, fill-first, p2c, random, least-used, cost-optimized,
reset-aware, reset-window, headroom, quota-weighted, strict-random, auto,
lkgp, context-optimized, cache-optimized, fusion, pipeline`.
- `INTERNAL_ROUTING_STRATEGY_VALUES = ["quota-share"]` (never UI-exposed).
- `AUTO_ROUTING_STRATEGY_VALUES` (9): `rules, score, cost, eco, latency,
fast, sla-aware, sla, lkgp`.
- `ACCOUNT_FALLBACK_STRATEGY_VALUES` (9): `priority, weighted, fill-first,
round-robin, p2c, random, least-used, cost-optimized, strict-random`.
- `normalizeRoutingStrategy()` aliases (`usage→least-used`,
  `context→context-optimized`, `weekly-reset→reset-window`).

Combo engine: `open-sse/services/combo.ts` (1040 lines) — `handleComboChat()`
(+ trace header `X-OmniRoute-Combo-Trace`), `buildAutoCandidates`,
`resolveTargetTimeoutMsForTarget`, `resolveComboTargets /
resolveComboRuntimeUnits / filterTargetsByRequestCompatibility`
(`combo/comboStructure.ts`), `scoreAutoTargets /
expandAutoComboCandidatePool` (`combo/autoStrategy.ts`), shadow routing,
`validateComboDAG / clampComboDepth / MAX_GLOBAL_ATTEMPTS`, dispatch preludes
(`combo/dispatchPrelude.ts`), attempt loop (`combo/comboAttemptLoop.ts`),
round-robin (`combo/roundRobinCombo.ts`). Each target calls
`handleSingleModel()` wrapping `handleChatCore()` with breaker/cooldown/
lockout gates.

Fusion exception: `open-sse/services/fusion.ts` (491 lines) —
`FUSION_DEFAULTS{minPanel:2, stragglerGraceMs:8000, panelHardTimeoutMs:90000,
maxPanel:40}`; parallel fan-out (non-streaming, tools-stripped panel body) →
`collectPanel(quorum+grace)` → `extractPanelText` → judge prompt
(consensus/contradictions/coverage/blind-spots, anonymized Source N) → judge
model synthesizes the final answer.

### Resilience — three distinct mechanisms (do not conflate)

1. **Provider circuit breaker** — whole provider (e.g. `glm`, `openai`).
   `src/shared/utils/circuitBreaker.ts` (`CLOSED/DEGRADED/OPEN/HALF_OPEN`,
   lazy `HALF_OPEN` on `getStatus/canExecute/getRetryAfterMs` reads);
   chat wiring `src/sse/handlers/chatHelpers.ts`, `src/sse/handlers/chat.ts`;
   status API `src/app/api/monitoring/health/route.ts`; wrappers
   `open-sse/services/accountFallback.ts`; persisted table
   `domain_circuit_breakers`. Only `408,500,502,503,504` trip it. Defaults
   from `PROVIDER_PROFILES` (`open-sse/config/constants.ts`) via
   `DEFAULT_RESILIENCE_SETTINGS.providerBreaker`
   (`src/lib/resilience/settings.ts`). Full table in
   `docs/architecture/RESILIENCE_GUIDE.md`.
2. **Connection cooldown** — one key/account.
   `markAccountUnavailable()` (`src/sse/services/auth.ts`),
   `checkFallbackError()` (`open-sse/services/accountFallback.ts`), skip while
   `rateLimitedUntil > Date.now()`, OAuth base 5s / API-key base 3s with
   `base*2**failureIndex` backoff + upstream `Retry-After` hints,
   `clearAccountError()` on success. Terminal `banned/expired/
credits_exhausted` are sticky, not cooldowns.
3. **Model lockout** — provider+connection+model.
   `open-sse/services/accountFallback.ts` (`lockModel/recordModelLockout…/
isModelLocked`, `MODEL_ACCESS_DENIED_PATTERNS`,
   `CONTEXT_OVERFLOW_PATTERNS`), so one bad model does not disable the whole
   connection.

Quota surfaces: `src/lib/db/{quotaSnapshots,quotaPools,quotaConsumption,
creditBalance}.ts`, `open-sse/services/*QuotaFetcher.ts`,
`quotaCache.ts`, `dailyQuotaReset.ts`, `src/app/api/quota/**`,
`src/app/api/v1/quotas/check`.

---

## 12. Authentication / sessions / OAuth / credential storage **[confirmed]**

- Pipeline auth: `extractApiKey / isValidApiKey`
  (`src/shared/utils/apiAuth.ts:303,331`, JWT fallback),
  `generateApiKeyWithMachineId` (`src/shared/utils/apiKey.ts`),
  Redis cache `auth:api_key:<sha256>` (`src/shared/utils/rateLimiter.ts`),
  `generateRequestId` (`src/shared/utils/requestId.ts`).
- Management password: `getStoredManagementPassword /
hashManagementPassword / verifyManagementPassword` (`bcryptjs`)
  (`src/lib/auth/managementPassword.ts`), `src/server/auth/loginGuard.ts`.
- Dashboard session: `auth_token` cookie, `SignJWT HS256 30d`, verified
  **only** by `verifyDashboardSessionToken()`
  (`src/shared/utils/dashboardSessionToken.ts:31`, `JWT_SECRET`).
  Auto-refresh in pipeline (`refreshDashboardSessionIfNeeded()`).
- Login/OIDC routes: `src/app/api/auth/login|logout|status|csrf|
oidc/login|oidc/callback`.
- Env: `JWT_SECRET`, `API_KEY_SECRET`, `INITIAL_PASSWORD`,
  `OMNIROUTE_API_KEY/ROUTER_API_KEY` (`.env.example`,
  `src/shared/utils/secretsValidator.ts`, `scripts/dev/sync-env.mjs`).
- API-key DB (`src/lib/db/apiKeys.ts`): `validateApiKey /
getApiKeyMetadata / createApiKey / regenerateApiKey /
updateApiKeyPermissions / revokeApiKey / deleteApiKey /
pickApiKeyForInternalUse`, `hashKey = SHA256(key)`, in-memory LRU (60s TTL)
  - Redis 1h. Tables: `api_keys` (+lifecycle columns), `domain_budgets`,
    `domain_cost_history`, `apiKeyGroups.ts`, `apiKeys/{rowParsers,
modelPermissions,permissionsUpdate}.ts`.
- OAuth (`src/lib/oauth/`, ~50 files): constants
  (`constants/oauth.ts:40-444` — Claude/Codex/OpenAI/Cursor/Antigravity/AGY/
  GitHub/GHE-Copilot/GitLab-Duo/Kiro/Qoder/Grok-CLI/XAI/Kimi/Trae/Devin/Zed…,
  `OAUTH_TIMEOUT=300000`); providers in `providers/*.ts`; flows in
  `utils/pkce.ts`, `utils/loopbackTunnel.ts` (`127.0.0.1:56121-56123`),
  `codexDeviceFlow.ts`, `deviceFlowTickets.ts`, `kiroSocialPoll.ts`,
  `pasteCredentials.ts`, `credentialBlob.ts`, `connectionPersistence.ts`;
  public client_id/secret embedded **only** via `resolvePublicCred()`
  (`open-sse/utils/publicCreds.ts`, Hard Rule #11).
- Encryption at rest (`src/lib/db/encryption.ts:181-405`): AES-256-GCM
  `enc:v1:<iv>:<ct>:<tag>`, `scrypt(secret, STATIC_SALT)` primary + legacy
  dynamic-salt fallback, `STORAGE_ENCRYPTION_KEY` (passthrough if unset).
  Fields: `provider_connections.api_key/access_token/refresh_token/id_token`
  (`providers.ts`, `providers/lazyConnectionView.ts`, `syncTokens.ts`),
  `secrets.ts`, `registeredKeys.ts`, `cloudAgent/db.ts`. Never logged
  (`src/mitm/maskSecrets.ts`, `open-sse/utils/credentialPatterns.ts`).

---

## 13. SQLite / database layers **[confirmed]**

- Singleton: `getDbInstance()` (`src/lib/db/core.ts:1035`, `globalThis`
  HMR-safe; `singleton.ts`, `migrationRunner.ts`, `schemaColumns.ts`,
  `caseMapping.ts:rowToCamel()`, `stateReset.ts`, `healthCheck.ts`,
  `recovery.ts`, `probeUtils.ts`).
- Files: `DATA_DIR/storage.sqlite` (`SQLITE_FILE`), `db.json` legacy,
  `db_backups/` (`src/lib/dataPaths.ts`). `DATA_DIR` resolves to
  `~/.omniroute/` by default.
- Base schema (`SCHEMA_SQL`, 17 tables): `provider_connections,
provider_nodes, key_value, combos, api_keys, db_meta, usage_history,
call_logs, proxy_logs, domain_* (fallback_chains/budgets/budget_reset_logs/
cost_history/lockout_state/circuit_breakers), semantic_cache,
quota_snapshots`.
- Migrations: 176 `.sql` files `001_initial_schema.sql →
179_proxy_logs_upstream_status.sql` (numbering has intentional gaps;
  `check:migration-numbering` enforces). Notable: `002_mcp_a2a_tables`,
  `015_memories`, `022_memory_fts5`, `028_files_and_batches`,
  `032_apikey_lifecycle`, `034-043/045/049` compression,
  `058_command_code_auth`, `060_gamification`, `066_api_key_groups`,
  `069_webhook_deliveries`, `080_agent_bridge`, `104_cache_size`,
  `170_log_export_destinations`, `178_memory_fts_au_conditional`.
  Each runs idempotently in a transaction; tracked in
  `_omniroute_migrations`.
- Pragmas (`core.ts:1306-1352`): `busy_timeout=2000`, `journal_mode=WAL`,
  `synchronous=NORMAL`, `cache_size=-<setting>`, `temp_store=MEMORY`,
  `mmap_size=256MiB` (from stored settings, migration 046, best-effort).
  `walMaintenance.ts` (TRUNCATE 6h / PASSIVE 5m / 256MB guard,
  `OMNIROUTE_WAL_*`), `vacuumScheduler.ts` (6h `VACUUM`), `cleanup.ts`,
  `backup.ts` (`VACUUM INTO`), `optimizationSettings.ts`,
  `closeProbeIfSafe()`.
- Drivers (`db/adapters/`): cascade `better-sqlite3 → node:sqlite →
bun:sqlite → sql.js`, `tryOpenSync()` + `openDatabaseAsync()`
  (`driverFactory.ts`), `betterSqliteAdapter / nodeSqliteAdapter /
nodeSqliteShared / bunSqliteAdapter / sqljsAdapter / types.ts`.
- Access rule (Hard Rules #2/#5): routes/handlers never issue raw SQL; all
  ops go through the 127 `src/lib/db/*.ts` domain modules. The old
  `localDb.ts` barrel is removed. See `src/lib/db/AGENTS.md`.
- Cloud/build: `isCloud (globalThis.caches)` or `OMNIROUTE_BUILDING/
NEXT_PHASE` → `:memory:` / no-op stub; local → file-backed WAL.

## 14. Caches, admission, long-lived state, background jobs **[confirmed]**

- Read caches: `src/lib/db/readCache.ts:74` (`getCachedSettings 5s /
getCachedPricing 30s / getCachedConnections 5s`, `TTLCache`),
  `semanticCache.ts`, `reasoningCache.ts` (hybrid mem+SQLite),
  `apiKeys.ts` LRU+Redis, `open-sse/services/reasoningCache.ts:610`
  (interval sweep), `requestDedup.ts`, `signatureCache.ts`, `quotaCache.ts`,
  `providerCooldownTracker.ts`, `rateLimitManager.ts`,
  `src/lib/memory/{cache,embedding/cache}.ts`,
  `src/app/api/v1/models/catalogCache.ts`,
  `open-sse/handlers/chatCore/{semanticCache,streamingSemanticCacheStore}.ts`.
- Admission/backpressure: `src/shared/middleware/chatBodyAdmission.ts`
  (heap probe `heapUsed/heap_size_limit >=
OMNIROUTE_CHAT_ADMISSION_HEAP_SHED_RATIO=0.75`), `admissionBudget.ts`
  (`computeIngestByteBudget` = 25% ceiling / 8x amplification, clamp
  8MiB–2GiB, from `v8.getHeapStatistics().heap_size_limit` /
  `process.constrainedMemory()` / override), `ingestByteAdmission.ts`,
  `withChatAdmission.ts`, wired in `chatHelpers.ts:409`, `chat.ts:1556`,
  `chatCore.ts:508`, `open-sse/services/admission/runtime.ts`.
- Heap guards: `open-sse/utils/heapPressure.ts`
  (`computeHeapPressureThresholdMb` = max(85% ceiling, 400MB) → 503
  `heap_pressure`), `resourcePressure{,Policy,Sampler}.ts` (v8 + cgroup + PSI,
  optional `OMNIROUTE_PRESSURE_SELF_RESTART`).
- Background jobs: WAL maintenance + vacuum + cleanup + backup retention
  (`MAX_DB_BACKUPS`), `backupScheduleJob.ts`, `startDbHealthCheckScheduler()`
  (6h); OAuth auto-refresh daemon (`autoRefreshDaemon.ts`, `tokenRefresh.ts`);
  `rateLimitManager` persist+watchdog timers; `providerCooldownTracker`
  cleanup timer; `dailyQuotaReset`; quota fetchers `_cacheCleanup` intervals;
  `mcp-server/httpTransport.ts:34` session sweep (60s, idle 5m);
  `runtimeHeartbeat.ts:83`; `responsesTransformer.ts:575` keepalive (3s);
  TLS watchdogs; `requestDedup.ts:223`. All long-lived timers use `unref()`
  so the CLI can exit.

---

## 15. SSE / WebSocket / streaming paths **[confirmed]**

- Entry routes: `v1/chat/completions`, `v1/responses(+[...path])`,
  `v1/messages`, `v1/ws` (`ws@8.21.3`), `v1/relay/chat/completions(+bifrost)`,
  `vscode/[token]/…/chat/completions`, plus `scripts/dev/
responses-ws-proxy.mjs` bridging WS↔SSE.
- Core: `open-sse/handlers/chatCore.ts:handleChatCore()` →
  `chatCore/streamingPipeline.ts`, `providerExecutionPipeline.ts`,
  `nonStreamingProviderLeg.ts`, `streamingResponseHeaders.ts`,
  `responsesJsonToSse.ts`, `sseParser.ts` (+`sseParser/geminiResponse.ts`),
  `responseTranslator.ts`, `responseSanitizer*.ts`, `usageExtractor.ts`.
- Utils: `open-sse/utils/{stream,streamHandler,streamHelpers,
streamPayloadCollector,streamTiming,streamFailureBoundary,
streamFailureFinalization,streamErrorFormat,sseHeartbeat,
responsesStreamHelpers,upstreamResponseHeaders,proxyFetch,
proxyDispatcher,tlsClient,fetchStartTimeoutPolicy}.ts`.
- Executors stream upstream bytes through provider-specific `fetch()` with
  retry/backoff; failures are finalized through the stream-failure boundary
  so the client always gets a terminal frame (never a hung stream).
- Transformer: `open-sse/transformer/responsesTransformer.ts:192`
  `createResponsesApiTransformStream()` (Chat→Responses events, keepalive,
  `highWaterMark 16k`).
- **Never swallow SSE errors** (Hard Rule #6); abort signals drive cleanup.

---

## 16. MCP **[confirmed]**

- `open-sse/mcp-server/server.ts:737` → `createMcpServer({blockedProviders})`:
  110 tools (45 canonical + memory/skill/GitHub/pool/gamification/plugin/
  Notion/Obsidian/local-corpus/RTK modules), `omniRouteFetch()`
  (`OMNIROUTE_BASE_URL` + internal auth headers),
  `withScopeEnforcement()`, `compressMcpRegistryMetadata()`,
  `toolCardinality.ts`.
- Transports: `httpTransport.ts:203,297` (`POST /api/mcp/stream +
GET/DELETE`, `GET+POST /api/mcp/sse`, `WebStandardStreamableHTTPServer
Transport`, 404-on-unknown-session #5169), `index.ts` (stdio),
  `bin/omniroute.mjs --mcp`.
- Tools: `tools/{advancedTools,pickFastestModel,memoryTools,skillTools,
agentSkillTools,githubSkillTools,poolTools,gamificationTools,pluginTools,
compressionTools,notionTools,obsidianTools,localCorpusTools}.ts`,
  `toolSearch/{search,catalog,handler,register,signature}.ts`,
  `schemas/{tools,toolSearch,a2a,ccrTools,…}.ts`,
  `scopeEnforcement.ts` (`OMNIROUTE_MCP_ENFORCE_SCOPES`, 33 scopes),
  `httpAuthContext.ts`, `mcpCallerIdentity.ts`, `audit.ts:logToolCall()`
  (table `mcp_tool_audit`), `catalog.ts:getMcpModelsCatalog()`,
  `radarCatalog.ts`, `descriptionCompressor.ts`, `toolCount.ts`.
- Spawn-capable MCP routes are `isLocalOnlyPath()`-gated (Hard Rule #15).

## 17. A2A **[confirmed]**

- `src/lib/a2a/taskManager.ts` (CRUD + `A2ATask`),
  `taskExecution.ts:138,165` (`A2A_SKILL_HANDLERS`, 6 skills:
  `smart-routing/quota-management/provider-discovery/cost-analysis/
health-report/list-capabilities`) + `executeA2ATaskWithState() /
collectMemoryHits()` (1500ms, `OMNIROUTE_A2A_MEMORY_HITS`),
  `authenticate.ts:resolveA2AOwner()` (SHA256 prefix), `streaming.ts`,
  `routingLogger.ts`, `skills/*.ts`, `src/lib/db/a2aTasks.ts`
  (`a2a_tasks` + events), `src/app/.well-known/agent.json/route.ts`
  (Agent Card), `src/lib/skills/a2a.ts`, `mcp-server/schemas/a2a.ts`.

## 18. Compression pipeline **[confirmed]**

- `open-sse/services/compression/` (~40 files): modes
  `off/lite/standard/aggressive/ultra/rtk/stacked`;
  `strategySelector.ts`, `planResolution.ts`, `resolveCompressionPlan.ts`,
  `lite.ts`, `caveman{,Rules}.ts`, `ruleLoader.ts`, `languageDetector.ts`,
  `preservation.ts`, `validation.ts`, `hardBudget.ts`, `progressiveAging.ts`,
  `prefixFreeze.ts`, `liveZone.ts`, `messageContent.ts`,
  `toolResultCompressor.ts`, `summarizer.ts`, `stats.ts`, `resultMemo.ts`,
  `pipelineGuards.ts`, `pipelineEngineBreaker.ts`,
  `compressionWorkerPool.ts` (`OMNI_COMPRESSION_WORKERS=2`, 120s timeout,
  60s idle, `new Worker(compressionWorker.js/ts)`), `outputMode.ts`,
  `outputStyles/`, `riskGate/`, `quantumLock/`, `engines/{rtk,registry,
mcpAccessibility}`, `harness/`, `rules/{en,de,fr,es,it,ja,pt-BR,ru,zh}/
*.json`; `judgeModelClient.ts` (`src/lib/compression/`).
- Persistence: `src/lib/db/{compression,compressionCombos,
compressionRunTelemetry}.ts` (migrations `034-043`), tools
  `mcp-server/tools/compressionTools.ts`, routes `src/app/api/compression/**`
  - `src/app/api/context/**`.

## 19. Memory / search / vector / Qdrant **[confirmed]**

- `src/lib/memory/`: `index/manager/store/backend/sqliteBackend/
genericBackend/obsidianBackend/vectorStore/retrieval(+scoring)/injection/
extraction/summarization/typedDecay/reindex/verify/settings/schemas/types/
cache.ts`; `embedding/`: `index/remote/customProvider/transformersLocal/
staticPotion/providerListings/rerankListings/cache.ts`.
- `qdrant.ts:60-375`: `normalizeQdrantConfig / checkQdrantHealth /
upsertSemanticMemoryPoint / searchSemanticMemory` (`QDRANT_HOST/PORT/
API_KEY/COLLECTION/EMBEDDING_MODEL/VECTOR_SIZE/HNSW_EF_CONSTRUCT`,
  `omniroute_memory` Cosine + int8/binary quantization + rescore);
  `docker-compose.yml --profile memory`.
- SQLite: `015_memories.sql`, `022_memory_fts5.sql`,
  `023_fix_memory_fts_uuid.sql`, `178_memory_fts_au_conditional.sql`
  (`memories` + FTS5); optional `sqlite-vec@0.1.9`
  (`vectorStore.ts:455`); `memoryTools.ts:resolveMemoryOwnerId()`
  (`apiKeyId|mcp`).

## 20. Usage tracking / logging / telemetry **[confirmed]**

- `open-sse/utils/usageTracking.ts:990` (`logUsage / extractUsage` +
  `pickCacheCreationTokens`, `normalizeResponsesUsage`), `usage.ts`
  (per-request cost), `tokenLimitCounter.ts`, `tokenExtractionConfig.ts`,
  `streamTiming.ts`, `generationThroughput.ts`, `requestLogger.ts`,
  `providerRequestLogging.ts`, `diagnostics.ts`.
- DB: `src/lib/db/{usageLogs,usageSummary,usageAnalytics(+sources),
callLogStats,quotaSnapshots,quotaConsumption,proxyLogs,
relayProbeStats}.ts`, `src/lib/usageDb.ts:appendRequestLog()`,
  `src/lib/usage/{tokenAccounting,callLogArtifacts}.ts` (`call_logs` +
  filesystem artifacts `artifact_relpath/sha256`), `quotaSnapshots.ts`.
- Logs: `pino` (`open-sse/utils/logger.ts`), `proxyLogger.ts`, `audit_log`
  (`src/lib/compliance`), `logExport/{registry,destinations}`
  (`170_log_export_destinations.sql`), webhooks (`011_webhooks.sql`,
  `src/lib/webhookDispatcher.ts`, `webhookDeliveries.ts`).

## 21. Files / batches **[confirmed]**

- `src/lib/db/files.ts` + `batches.ts` (migrations
  `028_create_files_and_batches.sql`, `053_remove_status_from_files.sql`),
  `open-sse/services/batchProcessor.ts`,
  `src/app/api/v1/{files,batches}/**/route.ts`,
  `open-sse/utils/secureFileWrite.ts`.

## 22. Native modules already present **[confirmed]**

`package.json` optional deps: `better-sqlite3@13`, `bun:sqlite`,
`node:sqlite`, `sql.js@1.14`, `sqlite-vec@0.1.9`,
`onnxruntime-node@1.24.3`, `@huggingface/transformers`, `keytar@7`,
`sharp@0.35`, `wreq-js@3.2` (TLS fingerprint;
`scripts/build/{build-tproxy-native,wreqJsNative}.mjs`,
`config/release/wreq-js-*`), `@atjsh/llmlingua-2`,
`js-tiktoken/tiktoken`. Plus `src/mitm/tproxy/native/
{transparent.c,binding.gyp}` (node-gyp), `electron/` (desktop),
`packages/browser-pool` (Playwright Chromium).

## 23. Frontend-to-backend contracts **[confirmed]**

- Client APIs: `src/app/api/v1/**` (OpenAI-compatible) + `src/app/api/
{auth,combos,providers,usage,mcp,monitoring/health,resilience,rate-limits,
services/[name]/*}` (management). Validation via Zod
  (`src/shared/validation/schemas/apiV1.ts`); errors via
  `open-sse/utils/error.ts`.
- Dashboard: `src/app/(dashboard)/dashboard/**` (providers/services tabs
  reusing `ServiceStatusCard/LifecycleButtons/LogsPanel`), `src/app/
.well-known/agent.json` (A2A card), `docs/openapi.yaml`
  (`gen-openapi-module.mjs`, `check:openapi-*`),
  `docs/reference/API_REFERENCE.md`.
- Realtime: stamped `x-authz-*`/`x-request-id` headers via
  `NextResponse.next({request:{headers}})`, `TransformStream` SSE, `ws` route
  - `responses-ws-proxy.mjs`. Dashboard and API share port 20128.

## 24. External services and subprocesses **[confirmed]**

- Upstream: 358 providers (`src/shared/constants/providers.ts`,
  `open-sse/config/providerRegistry.ts`); TLS clients
  (`tlsClientBase/grokTlsClient/claudeTlsClient/perplexityTlsClient/…`);
  `networkProxy.ts / socksConnectorWithFamily.ts / proxyDispatcher.ts` +
  `proxy_registry/proxy_assignments` + 1proxy (`oneproxy.ts`).
- Subprocesses (Hard Rule #13 — `execFile`+args, never shell): `src/mitm/
{manager.ts:614 spawn(mitm/server.cjs), systemCommands.ts:execFileText/
execFileWithPassword, cert/install.ts, tproxy/setup.ts,
inspector/systemProxyConfig.ts}`; `src/lib/{tailscaleTunnel,
cloudflaredTunnel}.ts:spawn()`; embedded services
  `src/lib/services/installers/{ninerouter,mux,dario,cliproxy,bifrost}.ts:
runNpm()` (`$DATA_DIR/services/*`, `$DATA_DIR/bin/*`);
  `src/lib/services/bootstrap.ts:SERVICES[]`; `tool-detector.ts:
execFile(--version/which)`; `src/lib/acp/registry.ts:
detectInstalledAgents()`; `src/lib/headroom/{process,detect}.ts`.
- Managed: Qdrant, Redis (`ioredis`, rate-limit/auth cache), Ngrok
  (`@ngrok/ngrok`), Cloud agents
  (`src/lib/cloudAgent/agents/{codex-cloud,devin,jules,cursor-cloud}.ts` +
  `registry.ts`), Notion/Obsidian/GitHub skills, browser pool
  (`packages/browser-pool`, `browserPool.ts:browserBackedChat.ts`).

## 25. Platform-specific behavior **[confirmed]**

- `src/lib/db/core.ts:101-118`: cloud (`globalThis.caches`) or build phase
  (`OMNIROUTE_BUILDING/NEXT_PHASE`) → `:memory:`/stub; local → file WAL.
- `os.platform()/process.platform` branches: `tailscaleTunnel.ts`,
  `services/installers/utils.ts`, `services/portProbe.ts`,
  `skills/containerProvider.ts`, `acp/registry.ts`.
- Deploy: `Dockerfile/Dockerfile.bun/docker-compose*.yml/fly.toml`,
  `electron/` (Win/Mac/Linux), `src/mitm/` (sudo/system-proxy/DNS/TProxy CA
  `rootCa.ts`). Check `engines` before running: Node ESM only.

## 26. Uncertainty log (must resolve by runtime probing, not guessing)

- **[unknown]** Exact upstream `fetch()` dispatch line inside
  `providerExecutionPipeline.ts` (the `handleChatCore` split is 6k+ lines;
  audit traced to `translateRequest→getExecutor→execute` but did not pin the
  single `fetch` call site per executor family).
- **[unknown]** Full `(dashboard)` page tree — only `dashboard/` subdirs were
  listed; per-page data-fetch contracts need per-page reads.
- **[unknown]** `dist/server.js` contents (build artifact, gitignored) and
  Docker-vs-`serve.mjs` port precedence.
- **[likely]** Dual alias layers (`src/shared` vs `open-sse/config`) drift;
  verify via `/v1/models` output, not source diff alone.
- **[likely]** The bench:highwatermark npm target is dangling
  (its benchmark-highwatermark.ts file under scripts/perf is missing on disk —
  verify with ls scripts/perf before citing it).

---

_End of ARCHITECTURE.md — see FEATURE_PARITY.md for the per-capability
matrix, MEMORY_MODEL.md for RSS analysis, MIGRATION_PLAN.md for the C
migration design._
