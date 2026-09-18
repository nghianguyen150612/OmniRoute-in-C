# OmniRoute Compatibility Harness — Contract and Architecture

> Task 005 — DESIGN ONLY. No harness runner is implemented here, no C code is
> written, no build system is chosen, and no request is ever sent to a native
> backend (none exists). This document specifies the data model precisely
> enough that later tasks can implement the harness incrementally without
> redesigning it.
>
> Provenance labels used throughout:
>
> - **[existing]** — behavior or infrastructure present in this checkout,
>   cited by path (and line where verified).
> - **[verified-contract]** — Task 003/004 contract facts from
>   `contracts/v1-models.contract.json` and
>   `contracts/v1-models-compatibility-surface.contract.json`.
> - **[design-decision]** — a choice made by this task; implementable as specified.
> - **[future-requirement]** — something a later stage must satisfy; not claimed
>   to exist today.
>
> Relationship to `MIGRATION_PLAN.md` §5: that section remains the promotion
> contract (topology, comparison dimensions, resource capture, promote/rollback
> rule). This document specifies the **data model and component boundaries**
> needed to implement it. No change to `MIGRATION_PLAN.md` or
> `FEATURE_PARITY.md` was required by this task.

Machine-readable companions (specification artifacts only, not runtime code):

- `contracts/compat-scenario.schema.json` — scenario file schema (§4).
- `contracts/compat-observation.schema.json` — per-backend observation schema (§5).
- `contracts/compat-result.schema.json` — comparison result schema (§13).
- `contracts/compat-models-examples.json` — six Models scenarios in scenario
  form (§14).

---

## 1. Reusable infrastructure inventory [existing]

Surveyed before designing anything new. Verdict per capability: **reuse** (use
as-is), **adapt** (borrow the pattern, new harness-owned copy), or **separate**
(leave alone; harness must not depend on it).

### 1.1 Process management and isolated server startup

| Capability                                                                                             | Existing artifact                                                                                                                                 | Verdict                                                                                                  |
| ------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| Spawn with direct PID tracking (never name-match)                                                      | `scripts/perf/baseline/reference-backend-baseline.mjs:479-496` (Task 002)                                                                         | **reuse pattern** — PID-from-`spawn()` is the only safe identity under parallel sessions                 |
| Fresh isolated state per run (`mkdtemp` `DATA_DIR`, removed on success)                                | `reference-backend-baseline.mjs:437-439,762-769`; `tests/_setup/isolateDataDir.ts:29-41`                                                          | **reuse pattern** — per-run temp `DATA_DIR` + exit cleanup                                               |
| Random per-run secrets, operator secrets blanked                                                       | `reference-backend-baseline.mjs:457-474` (`JWT_SECRET`/`API_KEY_SECRET` random, `OMNIROUTE_API_KEY`/`ROUTER_API_KEY` blanked)                     | **reuse pattern** — harness must never inherit operator credentials                                      |
| Two-phase readiness: liveness then readiness, finite deadline, premature-exit detection at every phase | `reference-backend-baseline.mjs:390-414` (`GET /api/health` → `GET /api/health/ping`, `--ready-timeout-s`)                                        | **reuse pattern** — distinct liveness/readiness probes with a deadline                                   |
| Exit-aware wait with bounded log tails on failure                                                      | `tests/integration/resilience-http-e2e.test.ts:235-279` (`waitForServer`: 120 s deadline, `AbortSignal.timeout(5_000)` polls, last-40-line tails) | **reuse pattern** — bounded log ring (200 lines cap at `resilience-http-e2e.test.ts:213-222`)            |
| `SIGTERM` → grace → `SIGKILL`, descendant-scoped only                                                  | `reference-backend-baseline.mjs:698-760` (starttime-verified descendants, orphan check); `resilience-http-e2e.test.ts:281-292` (5 s grace)        | **reuse pattern** — never broad name-match kills; see §12                                                |
| Free-port allocation                                                                                   | `tests/e2e/helpers/mockUpstreamServer.ts:19-37` (`net.createServer` + `listen(0)`); same helper inlined in `resilience-http-e2e.test.ts:26-44`    | **reuse** `mockUpstreamServer.ts` shape for mock upstreams; harness allocates backend ports the same way |
| E2E boot env isolation                                                                                 | `resilience-http-e2e.test.ts:183-208` (`DATA_DIR`, `PORT`, `REQUIRE_API_KEY=false`, `INITIAL_PASSWORD=""`, background-service disable flags)      | **adapt** — reference-adapter env block follows this shape                                               |

The Task 002 baseline tool is **not** the compatibility harness and must not
become it **[design-decision]**: it measures one backend's RSS over minutes
(300 s idle waits) and records aggregates; the harness compares two backends'
observable behavior per scenario in seconds. What transfers is the
process-management discipline (PID tracking, secret hygiene, failure taxonomy,
descendant-scoped cleanup), not the tool itself.

### 1.2 Credentials and auth setup

| Capability                                                                                                      | Existing artifact                                                                                | Verdict                                                                                                   |
| --------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------- |
| Real key provisioning through the product flow (login → CSRF → create-key), key kept in memory, never persisted | `reference-backend-baseline.mjs:328-388` (`provisionApiKey`; CSRF header `x-omniroute-csrf`)     | **reuse pattern** — scenarios reference provisioned credentials; fixtures never store key material        |
| Dashboard session minting for unit-level requests                                                               | `tests/helpers/managementSession.ts` (`jose` `SignJWT`, `auth_token` cookie)                     | **separate** — in-process unit helper; the harness drives real HTTP and provisions like the baseline tool |
| Seeded-DB + open-mode boot for deterministic tests                                                              | `resilience-http-e2e.test.ts:460-542` (seed via domain modules, `closeDbInstance()`, then spawn) | **adapt** — fixture seeding follows seed-then-close-then-spawn ordering                                   |

### 1.3 Request capture, comparison, and fixtures

| Capability                                                                                                                 | Existing artifact                                                                                                                        | Verdict                                                                                                                                       |
| -------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| Stable JSON comparison (recursive key sort)                                                                                | `tests/helpers/goldenSnapshot.ts:7-21` (`stable()`); `UPDATE_GOLDEN=1` regen + `GoldenMismatchError` with expected/actual                | **adapt** — JSON key ordering is insignificant by construction (§8); golden files are a separate (single-backend) mechanism, not parity proof |
| Request-body capture on mocks (hits, bodies, timing, queued planned responses, `delayMs`)                                  | `tests/e2e/helpers/mockUpstreamServer.ts:65-175` (`MockUpstreamServer`: per-token default + queue, `/v1/models` stub, 404 for unhandled) | **reuse** for future mock-upstream needs (slice 3+); not needed for read-only slices                                                          |
| Controllable upstream byte stream (push/close/error/cancel)                                                                | `tests/helpers/fakeUpstreamStream.ts`                                                                                                    | **separate** — in-process unit seam; harness observes real sockets                                                                            |
| SSE pipeline invariants ([DONE] consumed, error→rejection, cancel propagation, watchdog cleanup, snapshot non-duplication) | `tests/integration/sse-correctness.test.ts`; `tests/integration/heap-growth.test.ts` (50 warmup + 500 streams, growth < 20 MB)           | **adapt** — these invariants shape the streaming observation design (§9); the tests themselves stay where they are                            |
| SSE consumer pattern (buffer split on `\n\n`, `data:` prefix, terminal states)                                             | `tests/e2e/protocol-clients.test.ts:43-87` (`consumeA2AStream`: completed/failed/cancelled)                                              | **adapt** — event-framing logic, not the transport                                                                                            |
| Error-shape discipline (`buildErrorBody` / `sanitizeErrorMessage`, never raw stack)                                        | `open-sse/utils/error.ts:364-394`; tests assert bodies never contain `at /`                                                              | **reuse as oracle** — error-structure comparison (§8) leans on the `{error:{message,type,code}}` triple                                       |
| Upstream header denylist with sync contract (sanitize + Zod + tests)                                                       | `src/shared/constants/upstreamHeaders.ts`                                                                                                | **separate** — product surface, not harness code; cited only to keep header policy aligned                                                    |
| CORS two-layer model (route-static + pipeline overlay)                                                                     | `src/shared/utils/cors.ts`; `src/server/cors/origins.ts`; `src/server/authz/pipeline.ts:359-365`                                         | **existing** behavior the header policy (§7) must encode, not abstract away                                                                   |

### 1.4 Timeouts, WS, DB inspection

| Capability                                                                                                   | Existing artifact                                                                                                               | Verdict                                                                                                        |
| ------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| Bounded waits via `AbortSignal.timeout(ms)` (5 s health, 10 s API, 20 s chat, 30 s ecosystem default)        | `resilience-http-e2e.test.ts:260,410,418,432`; `tests/e2e/protocol-clients.test.ts:8,25`                                        | **reuse pattern** — no wait is unbounded (§12)                                                                 |
| WS surfaces (`ws@8.21.3`, `/v1/ws` bridge, responses-WS proxy, injectable `__setXWebSocketForTesting` seams) | `src/app/api/v1/ws/route.ts`; `scripts/dev/responses-ws-proxy.mjs`; codex/copilot/uc executor tests                             | **separate** — WS comparison is a late stage (§15); unit seams stay unit seams                                 |
| DB-state inspection via domain modules + `closeDbInstance()` discipline                                      | `resilience-http-e2e.test.ts:21-24,532,549`; PII learning: tests must `resetDbInstance()` and close handles or the runner hangs | **adapt** — persistence observations read through domain modules or `sqlite3`, never raw SQL in fixtures (§10) |

No existing abstraction covers dual-backend differential comparison, so a new
harness-owned model is justified **[design-decision]**. Nothing above is
duplicated: the harness reuses patterns and a few helpers, and references the
rest as oracles.

---

## 2. Harness responsibilities

### 2.1 Must [design-decision]

The future harness MUST eventually be able to send equivalent scenarios to the
reference TypeScript/Node backend and the native C backend, and compare
observable behavior across these dimensions:

1. HTTP status (exact).
2. Selected headers (per-header policy, §7).
3. Response body bytes (exact or hashed above a size cap, §12).
4. JSON structure (modal comparison, §8).
5. Error structure (`{error:{message,type,code}}` triple + no-stack-leak on both).
6. Authentication behavior (same accept/reject + same error shape per credential class).
7. Redirects (status + `Location`/`Refresh`, never followed by default).
8. Method handling (per-method status/body/headers, including transport-guard methods).
9. SSE framing (byte capture + parsed event sequence, §9).
10. Streaming event sequence and ordering (ordered compare; normalization only where the contract documents nondeterminism).
11. Cancellation behavior (abort mid-stream → close, no further frames, equal persistence effects).
12. WebSocket behavior where applicable (late stage; handshake + frame sequence).
13. Persistence/database effects (constrained semantic observations, §10).
14. Post-response effects where required (bounded settle window, then re-observe).
15. Timing **only** where timing itself is part of a contract (`Retry-After`
    values, keepalive intervals, `reset after` messaging) — wall-clock is
    otherwise informational, never a pass/fail signal.

### 2.2 Extension points (designed now, built later) [design-decision]

Each dimension above is a comparator behind a registry keyed by scenario-declared
`capabilities` (e.g. `http`, `json`, `redirect`, `sse`, `cancel`, `ws`,
`persistence`, `timing-contract`). A scenario runs only the comparators its
capabilities require; adding a dimension means adding a comparator + observation
fields, never changing the pipeline shape (§3) or the scenario envelope (§4).

### 2.3 Non-goals [design-decision]

- Proving parity by passing: the harness compares exactly the dimensions each
  scenario encodes — silence on a dimension is not agreement.
- Performance benchmarking or RSS accounting (owned by the Task 002 baseline
  tooling and `MEMORY_MODEL.md` procedures).
- Load/chaos/fuzz generation (existing suites own those).
- Re-implementing Next.js semantics in test code: expectations encode
  **observable** behavior from Tasks 003/004, never framework internals.

---

## 3. Pipeline stages [design-decision]

Six separated stages; a scenario file is declarative input to stage 1 and never
contains executable behavior.

```text
scenario file (declarative, §4)
  │  1. LOAD: parse + validate against compat-scenario.schema.json
  ▼
prepared run plan (ports, temp dirs, credential refs resolved — no secrets on disk)
  │  2. EXECUTE per backend via adapter (§11): seed → start → ready → request(s) → settle → stop
  ▼
observation per backend (§5): status/headers/body/stream/persistence/failure — raw, unnormalized
  │  3. CAPTURE: bounded (§12), artifacts spilled to files, hashed
  ▼
normalized observation pair (applied rules recorded per field)
  │  4. NORMALIZE with explicit scenario/field-aware rules (§6)
  ▼
result (§13): verdict + mismatch paths + artifacts
  │  5. COMPARE with declared policies (§7, §8)
  ▼
  │  6. REPORT: terminal summary + result JSON + artifact directory
```

Stage contracts:

1. **Load** is pure: file bytes in, validated plan out. Unknown fields are
   rejected (schema `additionalProperties: false` at the top level) so typos
   fail loudly; a namespaced `extensions` object carries forward-compatible
   extras.
2. **Execute** is backend-agnostic: generic code drives the adapter interface
   (§11) and a capability-scoped HTTP client. No Next.js imports, no provider
   imports, no `open-sse` imports in generic code.
3. **Capture** records bytes before interpretation: headers as received
   (lowercased names, repeated values preserved in order), body as bytes with
   hash, stream as raw chunks plus parsed events.
4. **Normalize** transforms copies, never originals; every applied rule is
   recorded (`normalizedBy: ["request-id", ...]`) so reports distinguish
   "equal after normalization" from "equal as received".
5. **Compare** is total: every declared expectation yields zero or more
   mismatches with exact paths; undeclared dimensions yield nothing.
6. **Report** is bounded: terminal output is a summary; full bodies live in
   artifacts (§13).

---

## 4. Scenario schema [design-decision]

Full machine-readable form: `contracts/compat-scenario.schema.json`.
Prose reference below; the JSON Schema file is normative on field shapes.

```json
{
  "schema": "compat-scenario/v1",
  "id": "models-get-authenticated",
  "description": "Authenticated GET /v1/models returns the catalog envelope",
  "http": {
    "method": "GET",
    "path": "/v1/models",
    "query": {},
    "headers": { "accept": "application/json" },
    "body": null,
    "followRedirects": false
  },
  "auth": { "kind": "provisioned-api-key" },
  "environment": { "requireApiKey": false },
  "seed": { "fixture": "fresh-install", "tables": ["settings"] },
  "compare": {
    "status": 200,
    "headers": { "content-type": "exact", "x-request-id": "presence" },
    "json": { "mode": "shape", "requiredPaths": ["object", "data"] },
    "capabilities": ["http", "json", "auth"]
  },
  "persistence": { "observations": [] },
  "timeout": "api",
  "tags": ["models", "http", "read-only"]
}
```

### 4.1 Field reference

| Field                   | Required | Meaning                                                                                                                                                                                                                                                                                                           |
| ----------------------- | -------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `schema`                | yes      | `compat-scenario/v1`; loader rejects anything else                                                                                                                                                                                                                                                                |
| `id`                    | yes      | kebab-case, unique per corpus; also the observation/result join key                                                                                                                                                                                                                                               |
| `description`           | yes      | one sentence; what observable behavior this pins                                                                                                                                                                                                                                                                  |
| `http.method`           | yes      | literal method token (`GET`, `HEAD`, `OPTIONS`, `POST`, `TRACE`, …); case-sensitive, sent as-is                                                                                                                                                                                                                   |
| `http.path`             | yes      | public path exactly as a client sends it (`/v1/models`, `/models`, `/models/`, case variants); never an internal route path                                                                                                                                                                                       |
| `http.query`            | no       | string→string map; serialized in insertion order (order is insignificant to the server; representative queries pin one order)                                                                                                                                                                                     |
| `http.headers`          | no       | string→string map; ordinary request headers only — pipeline-trusted internal headers can never be set here                                                                                                                                                                                                        |
| `http.body`             | no       | `null`, UTF-8 string, or `{ "artifact": "<repo-relative fixture>" }` for larger bodies                                                                                                                                                                                                                            |
| `http.followRedirects`  | no       | default `false`; redirects are compared, not followed (dimension 7 below)                                                                                                                                                                                                                                         |
| `auth`                  | no       | credential setup; default `{ "kind": "none" }`. Kinds: `none`, `provisioned-api-key` (harness mints via the product flow, §1.2), `invalid-key` (literal non-secret value), `malformed-scheme` (e.g. bare `Bearer`), `session` (later stage). The resolved credential is runtime state, never written to artifacts |
| `environment`           | no       | abstract config requirements (`requireApiKey`, named fixtures for settings); the adapter maps these to real backend env/flags. Abstract names keep scenarios backend-agnostic                                                                                                                                     |
| `seed`                  | no       | named fixture + table allowlist for initial persistent state; fixtures live in the harness corpus, seeded identically per backend before start                                                                                                                                                                    |
| `compare`               | yes      | per-dimension expectations: `status`, `headers` (per-header policy, §7), `json` (mode + options, §8), `error` (triple, §8), `redirect` (location/refresh, §7), `stream`/`cancel`/`persistence` (later stages)                                                                                                     |
| `persistence`           | no       | constrained observations (§10); absent means "no DB effect asserted" (collection may still record for diagnostics)                                                                                                                                                                                                |
| `timeout`               | no       | one of `health`, `api`, `chat`, `stream`, `bulk` — named categories mapped to milliseconds by harness config (§12), never inline numbers                                                                                                                                                                          |
| `tags` / `capabilities` | no       | `tags` are free-form selectors; `capabilities` select comparators (§2.2)                                                                                                                                                                                                                                          |
| `extensions`            | no       | namespaced forward-compat object; comparators ignore unknown namespaces                                                                                                                                                                                                                                           |

### 4.2 Scenario rules

- Declarative only: no functions, no templates, no script hooks. Anything
  requiring logic becomes a loader/adapter feature with its own schema version.
- No secrets: valid credentials are always provisioned at runtime; only
  known-invalid literals (e.g. `Bearer definitely-not-a-key`) may appear inline.
- No environment specifics: no ports, no temp paths, no hostnames. `Location`
  expectations use the path-preserving form the reference emits
  ([verified-contract] `/v1/models?limit=1…`, not absolute URLs).
- No Node specifics: no Next.js internals, no `after()` semantics, no module
  paths. `environment` uses abstract keys; only adapters know env vars.
- Every field optional except `schema`, `id`, `description`, `http.method`,
  `http.path`, `compare`. Minimal scenarios (e.g. a 404 probe) stay minimal.

---

## 5. Observation schema [design-decision]

Full machine-readable form: `contracts/compat-observation.schema.json`. The
harness captures one observation per (scenario, backend). Capture precedes
interpretation: normalization (§6) works on copies.

```json
{
  "schema": "compat-observation/v1",
  "scenarioId": "models-get-authenticated",
  "backend": { "id": "reference", "revision": "51febc7a0", "mode": "dev" },
  "outcome": "http-response",
  "status": 200,
  "headers": [{ "name": "content-type", "value": "application/json" }],
  "body": {
    "bytes": 214005,
    "sha256": "…",
    "truncated": false,
    "artifact": "obs/<id>/reference/body.bin"
  },
  "json": { "object": "list", "data": [] },
  "redirect": null,
  "stream": null,
  "failure": null,
  "persistence": { "observations": [] },
  "timing": { "wallMs": 1932, "ttfbMs": 1932 }
}
```

### 5.1 Outcome taxonomy — five distinct terminal states, never conflated

| `outcome`            | Meaning                                                            | `status`              | Body/headers                                               |
| -------------------- | ------------------------------------------------------------------ | --------------------- | ---------------------------------------------------------- |
| `http-response`      | bytes received, response completed (any status, including 4xx/5xx) | present (exact `u16`) | captured                                                   |
| `timeout`            | no complete response within the scenario timeout                   | **absent**            | partial capture flagged `truncated: true` when bytes exist |
| `connection-refused` | TCP connect failed (backend not listening)                         | absent                | none                                                       |
| `connection-reset`   | connection dropped mid-response (incl. premature disconnect)       | absent                | partial bytes retained + flagged                           |
| `backend-exit`       | backend process died (or was already dead) during the scenario     | absent                | exit info in `failure`                                     |
| `harness-error`      | fixture/adapter/capture bug (bad seed, lost artifact, over-cap)    | absent                | diagnostic in `failure`                                    |

A missing response is NEVER recorded as status `0` or any synthetic status
**[design-decision]**: `status` is present if and only if
`outcome == "http-response"`. Comparators treat `timeout` vs `http-response`
with a slow status as different verdicts (the former is a liveness failure,
the latter a contract result).

### 5.2 Field notes

- `backend.id` is `reference` or `native`; `revision`/`mode` are informational
  (image/rev of what ran) and never compared.
- `headers`: array of `{name, value}` in received order; names lowercased at
  capture (representation hygiene, not normalization — HTTP names are
  case-insensitive [existing] per undici merging behavior). Repeated headers
  keep all values in order; `set-cookie` values are captured but excluded from
  default comparison (extension point, §7).
- `body`: `bytes` (observed length), `sha256` (of full bytes when fully
  captured), `truncated` + `capName` when a bound fired, `artifact` path for
  bytes above the inline threshold (§12). Small bodies may additionally inline
  `textPreview` (bounded, §13).
- `json`: parsed value when content-type is JSON and the body was fully
  captured under the parse cap; otherwise `null` with `jsonParse: {skipped,
reason}`. Parse failures on a JSON content-type are recorded, not thrown.
- `redirect`: `{location, refresh}` captured verbatim when status is
  3xx (or whenever those headers are present); comparators apply §7 rules.
- `stream`: null until the SSE stage; shape reserved by §9
  (`eventCount`, `terminal`, `truncated`, raw + parsed artifact refs).
- `failure`: `{kind, detail}` for the four non-response outcomes; `detail` is
  bounded and never contains key material.
- `persistence` / `postResponse`: constrained observations (§10) plus an
  optional `settledAfterMs` record; absent when the scenario declares none.
- `timing`: informational only (`wallMs`, `ttfbMs`); compared only when the
  scenario declares the `timing-contract` capability (dimension 15).

---

## 6. Normalization policy [design-decision]

Normalization exists solely to prevent false parity failures on values that
are nondeterministic **by contract**. It is explicit, scenario/field-aware,
and recorded per application (`normalizedBy`).

### 6.1 Allowed normalization rules (closed catalog; additions need a schema bump)

| Rule id            | Applies to                                                                   | Transformation                                                                                                                                   |
| ------------------ | ---------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| `header-name-case` | all header names                                                             | lowercase at capture (§5.2); always on, not scenario-declared                                                                                    |
| `header-order`     | header lists                                                                 | order-insignificant compare (multiset per name); always on                                                                                       |
| `json-key-order`   | JSON objects                                                                 | key-order-insignificant deep compare (cf. `goldenSnapshot.stable()` [existing]); always on                                                       |
| `request-id`       | `x-request-id` (and pipeline `AUTHZ_HEADER_REQUEST_ID` projections)          | presence required, value ignored; both backends must emit the header                                                                             |
| `date-headers`     | `date`, `last-modified`, `expires` (when not contract-pinned)                | value ignored; presence policy per scenario                                                                                                      |
| `catalog-created`  | `data[].created` + envelope timestamps on catalog responses                  | value ignored — [verified-contract] one timestamp shared per response; wall-clock differs across backends by construction                        |
| `dynamic-host`     | absolute URLs embedding the probe host/port, if any contract ever emits them | rewrite to `{host}` placeholder; Task 004 redirect `Location`s are path-only so this rule is currently unused — reserved, not applied by default |
| `next-internals`   | headers only a Next.js runtime can emit (chunked-framing extras, build ids)  | ignored — the C backend has no Next internals by design (`MIGRATION_PLAN.md` §3.4); the scenario must still pin every contract header explicitly |
| `keepalive-frames` | SSE comment/`ping` frames listed in the scenario                             | dropped before event-sequence compare (§9)                                                                                                       |

### 6.2 Forbidden normalizations (harness must fail closed)

- Status codes, error `type`/`code`, required-field presence.
- Array ordering, unless the governing contract explicitly documents order
  freedom AND the scenario declares the unordered path with its sort key (§8).
- SSE event ordering and terminal-event presence.
- Header presence/absence where the contract pins it (e.g. `content-length`
  absent on HEAD, `allow` absent on Models 405s [verified-contract]).
- Authentication outcomes: a 401-vs-200 difference is a mismatch, never
  normalized.
- Deleting a field globally "because it is inconvenient": every ignored path
  is named in the scenario's `compare` block and appears in the report.

Principle: precise comparison by default; each normalization narrows the
comparison and must be auditable in the result (§13).

---

## 7. Header comparison policy [design-decision]

### 7.1 Representation and matching

Headers compare as case-insensitive-name multisets. Two observations agree on
a header when the policy for that name holds over the full value multiset.
Value ordering within one name is insignificant (multiset), except
`set-cookie`, which is out of default scope.

### 7.2 Per-header policies

| Policy                        | Meaning                                                                                                   |
| ----------------------------- | --------------------------------------------------------------------------------------------------------- |
| `exact`                       | value multiset must be byte-equal                                                                         |
| `presence`                    | header must exist (≥1 value); values ignored                                                              |
| `absent`                      | header must not exist                                                                                     |
| `ignore`                      | header excluded; scenario must name it (recorded as normalization)                                        |
| `prefix:<s>` / `contains:<s>` | value(s) must start with / contain `s` (for HTML 404 content-type `text/html`, future build fingerprints) |

Scenarios declare policy per header; there is no global "compare all" or
"ignore all". Undeclared headers are not compared (but remain in the
observation for diagnostics).

### 7.3 Named-header rules derived from Tasks 003/004

| Header                                      | Models rule ([verified-contract])                                                                                                                                                                                                               |
| ------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `content-type`                              | `exact`. Values differ meaningfully per surface: `application/json` (GET/JSON 404s), `application/json` on HEAD (no body), absent on 204/405/308, `application/json; charset=utf-8` (TRACE guard), `text/html; charset=utf-8` (root HTML 404)   |
| `content-length`                            | `exact` where bodies are byte-compared; `absent` on HEAD (transport guard suppresses bytes), OPTIONS 204, 405s, 308s                                                                                                                            |
| `location` / `refresh`                      | `exact` on trailing-slash 308s (query-preserving, path-preserving per alias: `/v1/models?…` vs `/models?…`); `absent` elsewhere                                                                                                                 |
| `allow`                                     | `exact`: absent on normal Models 405s; `GET, POST, OPTIONS` on the TRACE-guard 405; pipeline OPTIONS 204 carries `access-control-allow-methods: GET, POST, PUT, DELETE, PATCH, OPTIONS` (global list, not the route-local `GET, HEAD, OPTIONS`) |
| `access-control-allow-origin`               | conditional by scenario: echoed origin for credentialed/preflight requests on the relaxed surface; absent for credential-less GET ([verified-contract] fail-closed). The scenario fixes request headers so the expectation is deterministic     |
| `access-control-allow-methods` / `-headers` | `exact` on pipeline preflights (global method list; requested-headers echo or the `pipelineCors.standardAllowHeaders` default from the surface contract)                                                                                        |
| `vary`                                      | `exact` per surface: `Origin` (204 preflight, no `Accept-Encoding` added); GET carries `Origin, Accept-Encoding` plus Next internals (see `next-internals`)                                                                                     |
| `cache-control`                             | `exact` where pinned: `no-store` (TRACE guard), `no-cache, must-revalidate` (HTML 404 page renders)                                                                                                                                             |
| `connection`                                | `exact: close` on HEAD (transport guard); otherwise undeclared                                                                                                                                                                                  |
| `x-request-id`                              | `presence` (+ `request-id` normalization); both backends must emit it                                                                                                                                                                           |
| `x-omniroute-route-class`                   | `exact: CLIENT_API` on matched Models-path responses; `absent` on raw 308s and the root HTML 404                                                                                                                                                |
| `x-model-catalog-version`                   | `exact` in shape (`model-metadata-v1:<sync\|static>`) on GET catalog responses; absent on HEAD/OPTIONS/405/404-neighbor responses. The suffix follows seeded state, so identical seeds make it comparable — never normalized away               |
| `x-omniroute-catalog` / `retry-after`       | pinned only on the `503 catalog_build_timeout` path (not live-triggered in Tasks 003/004; scenario-gated for later)                                                                                                                             |
| `content-type` on 204/308/405               | `absent` (verified dev-runtime behavior; production-standalone framing is an open question, §16)                                                                                                                                                |

CORS comparison never assumes "every header compared" nor "headers ignored":
the scenario's request headers (Origin / `Access-Control-Request-Method` /
`Access-Control-Request-Headers` / credential presence) determine the exact
expected response headers, per the surface contract.

---

## 8. JSON comparison policy [design-decision]

### 8.1 Modes

| Mode              | Rule                                                                                                                                                                                                     |
| ----------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `exact`           | deep equality modulo `json-key-order` + declared `ignorePaths` (normalized volatile leaves). Types exact: `null` vs missing is a mismatch                                                                |
| `shape`           | required paths exist with the declared JSON types; extra fields allowed; used where catalog content is seed-dependent                                                                                    |
| `subset`          | declared `expected` value is a recursive subset of actual (used for error envelopes inside larger bodies)                                                                                                |
| `ordered-array`   | default for all arrays: length + element-wise compare                                                                                                                                                    |
| `unordered-array` | allowed ONLY when the governing contract documents order freedom AND the scenario names the array path + a unique sort key. No blanket set-compare                                                       |
| `error-triple`    | `{error:{message,type,code}}` compared field-wise; `message` may additionally be `contains`-matched where the contract has dynamic segments (paths, model names), while `type`/`code` are always `exact` |

### 8.2 Models `data`-array rule (preserves Task 003 semantics)

[verified-contract] The catalog `data` array is **ordered**: combo block
first, then provider-grouped by `owned_by` in registry precedence, stable
within group; first-wins dedupe on `(id,type,subtype)`; no global alpha sort.
Therefore:

- The harness MUST compare `data` as an **ordered** array (`ordered-array`).
  Treating it as an unordered set would accept a wrongly-sorted native catalog.
- Full-catalog `exact` compare is legitimate only against identical seeds, with
  `catalog-created` normalization for the per-response timestamp.
- Environment-specific catalog **size is not a parity requirement**:
  scenarios assert structure over the seeded fixture (whose size is fixed by
  the fixture, e.g. fresh-install), using `shape` + ordered id-sequence
  prefixes where full equality would bake one machine's catalog into the corpus
  (explicitly forbidden by the task brief: no live 500+ model catalog in fixtures).
- Specialty rows assert the documented absence: `permission`/`root` missing on
  embedding/image/rerank/audio/moderation/video/music rows is checked by
  `shape` with explicit `absentPaths` — absence is significant here.

### 8.3 Error-structure rule

Every error comparator additionally asserts the Hard-Rule-#12 oracle on **both**
observations: no `at /` stack-trace leakage in the body ([existing] convention
from `open-sse/utils/error.ts` + suites). A leaking body fails the scenario
even when the triple matches.

---

## 9. Future streaming representation [design-decision, not implemented]

No streaming comparison is built in this task. The observation schema reserves
the `stream` slot with this contract:

```json
"stream": {
  "rawArtifact": "obs/<id>/native/stream.raw.bin",
  "rawBytes": 1048576,
  "rawSha256": "…",
  "eventsArtifact": "obs/<id>/native/stream.events.jsonl",
  "eventCount": 128,
  "eventsTruncated": false,
  "terminal": "data: [DONE]",
  "closedCleanly": true,
  "cancelled": false,
  "timeoutMs": null
}
```

- **Raw bytes vs parsed events**: both are retained. Raw chunks preserve
  framing evidence (chunk boundaries may legally differ across backends);
  parsed events are the comparison basis. Comparison is over the **ordered
  parsed event sequence** (type + data per event), never over raw chunking.
- **Event model**: `{index, event (nullable — OpenAI deltas use data-only
frames), data, rawBytes}` one JSON object per line (JSONL). Comment/keepalive
  frames (`:ping`, `OPENAI_*_FRAME` early keepalive [existing]) are recorded
  and then dropped by the `keepalive-frames` rule only when the scenario lists them.
- **Terminal contract**: a complete stream ends with the documented terminal
  frame (`data: [DONE]` for OpenAI-style; `response.completed` sequence for
  Responses API [existing]) followed by clean close. Missing terminal,
  upstream-error mid-stream, or a hung stream are distinct results
  (`connection-reset` / `timeout` outcomes, §5.1) — never "equal because bytes
  matched so far".
- **Ordering**: event order is always significant; fusion panel order is the
  only documented exception and requires its scenario to declare the
  nondeterministic span ([existing] fusion collects panel by quorum+grace).
- **Bounded capture** (low-memory goal): the executor streams to a spill file
  past an in-memory prefix (config `streaming.*`, §12); parsing is incremental
  over the spill file. The full stream is NEVER required in memory. Caps:
  max raw bytes, max events, max per-event bytes; exceeding a cap sets
  `eventsTruncated: true` + outcome stays `http-response` only if the terminal
  frame was already seen — otherwise the scenario errors as `harness-error`
  (budget wrong) or `timeout` (stream never finished), per the scenario's
  declared expectation. Truncated comparisons never pass silently.
- **Cancellation**: a scenario step declares `cancel: {afterEvents: N}` or
  `{afterMs: N}`; the executor aborts the client side and records
  `cancelled: true`, frames-after-cancel (must be none past a grace), close
  behavior, and persistence observations. Both backends must stop producing
  frames and release the connection.
- **Timeout**: per-scenario `timeout: stream` category; a stream that outlives
  it is outcome `timeout` with partial artifacts retained.

---

## 10. Persistence comparison boundary [design-decision]

Future mutating scenarios declare semantic observations — never SQL, never
database-file byte equality (WAL/page layout is nondeterministic by
construction).

```json
"persistence": {
  "tables": ["api_keys"],
  "observations": [
    { "table": "api_keys", "where": { "id": "<seeded-id>" },
      "select": ["last_used_at"], "expect": "changed" },
    { "table": "call_logs", "where": {}, "select": "count", "expect": { "delta": 1 } }
  ],
  "settleMs": 1000
}
```

- **Constrained selector language** (closed, non-executable): `table` (must be
  in the scenario's `tables` allowlist), `where` (equality map over literal
  scalars), `select` (column projection, `"count"`, or `"exists"`),
  optional `orderBy` + `limit` for multi-row reads, `expect` (`"unchanged"`,
  `"changed"`, exact value, or `{delta: N}` for counts). No expressions, no
  joins, no raw SQL in fixture data — ever.
- **Collection**: after the response completes AND after `settleMs` (covers
  `after()` background refresh, spend batching, audit/webhook deliveries
  [existing]), the harness reads each backend's `DATA_DIR` copy through domain
  modules or `sqlite3` dumps of the listed tables only, then stops the backend.
  Each backend gets its own `DATA_DIR` copy seeded from the same fixture —
  live writers never share files (`MIGRATION_PLAN.md` §3.2).
- **Nondeterministic columns**: timestamps, generated ids, and counters are
  compared via `changed`/`delta`/normalization rules (§6), never exact — unless
  the contract pins them.
- **Known volatile case** [verified-contract]: a valid database-backed API key
  may update `api_keys.last_used_at` during policy validation (TTL-gated).
  Models scenarios therefore either exclude `last_used_at` from projections or
  assert `changed` explicitly — never exact-match it.
- **Post-response effects**: `settleMs` is scenario-declared and bounded (§12);
  observations re-read after settling are part of the same observation record,
  marked `postSettle: true`.

---

## 11. Backend adapter boundary [design-decision]

A small conceptual interface between generic harness code and a backend
instance. Generic code depends only on this boundary — never on Next.js
internals, provider code, or Node process structure.

```text
Adapter(
  id: "reference" | "native",
  prepare(scenario) -> { env, dataDir, args }   # seed DATA_DIR copy, map abstract env
  start(prepared)   -> handle                     # spawn; return opaque handle (NOT a PID contract)
  waitReady(handle, deadlineMs) -> baseUrl        # liveness → readiness; throw AdapterNotReady (a harness-error cause)
  baseUrl(handle)   -> string
  stop(handle)                                    # SIGTERM → grace → SIGKILL, descendant-scoped (§12)
  health(handle)    -> { alive, exit? }           # process health for outcome classification (§5.1)
  describe()        -> { id, revision, mode }     # identity stamped into observations
)
```

- **Reference adapter** [future-requirement, pattern existing]: spawns the
  documented dev/standalone command with `PORT`/`DATA_DIR`/fresh secrets
  (§1.1), probes `GET /api/health` then `GET /api/health/ping`
  [existing]. Provisioned keys go through login → CSRF → create-key (§1.2).
- **Native adapter** [future-requirement]: same seven operations against the C
  binary when it exists — different command, different readiness endpoint
  (defined by the slice that introduces it), same outcome taxonomy. The native
  backend MUST NOT imitate Node's process structure (no `run-next`, no Turbopack
  child, no Next headers); readiness is "accepting HTTP on the configured port
  and answering its health probe", nothing more specific.
- **Scenario preparation** is adapter-owned: mapping abstract `environment`
  keys to real env/flags, seeding the fixture copy, allocating the port. The
  scenario never names executables, ports, or paths.
- **Health → outcome mapping**: a dead process at request time is
  `backend-exit`, not a timeout; a dead process at startup is `harness-error`
  with adapter diagnostics (mirrors the baseline tool's `measurement_failure`
  vs `request_failure` split [existing]).

---

## 12. Resource and safety constraints [design-decision]

Every wait bounded, every buffer capped, every child reaped. Exact defaults
are set only where repository evidence justifies a number; otherwise the table
names the config point and defers tuning to the implementing stage.

| Limit                                 | Evidence-anchored value or config point                                                                                                                                                                                                                                      |
| ------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Request timeout, `health` category    | config `timeouts.healthMs`; evidence: 5 s health polls [existing]                                                                                                                                                                                                            |
| Request timeout, `api` category       | config `timeouts.apiMs`; evidence: 10 s API probes [existing]                                                                                                                                                                                                                |
| Request timeout, `chat` category      | config `timeouts.chatMs`; evidence: 20 s chat posts [existing]                                                                                                                                                                                                               |
| Streaming timeout, `stream` category  | config `timeouts.streamMs`; evidence: 30 s ecosystem default [existing]; SSE drains use 2–5 s guards in unit tests                                                                                                                                                           |
| Startup/readiness deadline            | config `timeouts.readyMs`; evidence: 120 s E2E wait [existing]; 420 s baseline deadline covers cold Turbopack compile [existing]. Reference adapter default starts from the E2E-proven 120 s; native adapter negotiates its own (expected far lower — measured, not assumed) |
| Shutdown grace                        | `SIGTERM`, grace, then `SIGKILL`; evidence: 5 s E2E grace [existing]; 20 s + 10 s baseline [existing]. Default grace 5 s, configurable                                                                                                                                       |
| Max buffered response body            | config `capture.maxBodyBytes`; evidence informing the range: 64 KiB catalog chunked-stream threshold [verified-contract], 8 MiB ingest floor through 2 GiB clamp ceiling [existing]. Bodies past the cap spill to artifact files with `truncated: true`                      |
| Max inline preview / diagnostic slice | evidence: 200-line log rings [existing], 300-char body heads [existing]. Terminal output never carries full bodies (§13)                                                                                                                                                     |
| JSON parse cap                        | bodies past `capture.maxParseBytes` skip `json` with a recorded reason (raw bytes still compare by hash)                                                                                                                                                                     |
| Streaming capture                     | spill-to-file past an in-memory prefix; caps `streaming.maxRawBytes`, `streaming.maxEvents`, `streaming.maxEventBytes` (§9)                                                                                                                                                  |
| Tempfile lifecycle                    | `mkdtemp` per run under the OS temp dir; removed on success; retained on failure with the path printed (mirrors `--keep-data` [existing])                                                                                                                                    |
| Child-process cleanup                 | signal only spawned PIDs + starttime-verified descendants; post-stop orphan check; never name-match kills [existing]                                                                                                                                                         |
| Settle window                         | scenario `settleMs`, bounded by `timeouts.settleMaxMs` (§10)                                                                                                                                                                                                                 |
| Concurrency                           | scenarios execute sequentially per backend in the first stages (parallel backends allowed: reference and native run simultaneously on distinct ports/`DATA_DIR`s); any future parallelism gets its own bound, never unbounded                                                |

---

## 13. Failure reporting [design-decision]

Full machine-readable form: `contracts/compat-result.schema.json`. A parity
failure must be diagnosable from the artifact directory without rerunning.

```json
{
  "schema": "compat-result/v1",
  "scenarioId": "models-head",
  "verdict": "fail",
  "reference": { "observation": "obs/models-head/reference.json", "outcome": "http-response" },
  "native": { "observation": "obs/models-head/native.json", "outcome": "http-response" },
  "mismatches": [
    {
      "dimension": "headers",
      "path": "headers.content-length",
      "rule": "absent",
      "reference": "absent",
      "native": "\"12\"",
      "normalized": false,
      "artifact": null
    }
  ],
  "artifacts": { "dir": "artifacts/models-head", "result": "artifacts/models-head/result.json" }
}
```

- `verdict`: `pass` (all declared expectations held on both observations),
  `fail` (≥1 mismatch), `error` (any non-`http-response` outcome unless the
  scenario declares that outcome — e.g. a timeout-expectation scenario).
- `mismatches[]`: one entry per failed expectation —
  `dimension` (status/headers/json/error/redirect/stream/persistence/timing),
  `path` (exact field path, e.g. `json.error.code`, `headers.allow`,
  `data[0].id`), `rule` (the comparison rule applied), concise
  `reference`/`native` values (bounded strings; full values live in the
  observation artifacts), `normalized` (whether any normalization touched this
  path), and `artifact` pointer for larger diagnostics (diff files, body dumps).
- Size discipline: terminal output is the verdict + mismatch list with short
  values only. Full bodies, event streams, and dumps are files under the run's
  artifact directory, referenced by path — never pasted into logs.
- Both observations are always retained (reference + native), so a failure
  shows what each backend did, under which rule, with normalization visible.

---

## 14. Models mapping — Task 003/004 contracts as scenario sketches

[verified-contract] facts below come from the two contract artifacts; the
sketches show the §4 schema covers them without distortion. Specification
examples only — no execution, no live catalog copied, no model-count parity.

### 14.1 Authenticated `GET /v1/models`

```json
{
  "schema": "compat-scenario/v1",
  "id": "models-get-authenticated",
  "description": "Authenticated GET returns 200 catalog envelope with pinned headers",
  "http": { "method": "GET", "path": "/v1/models", "headers": { "accept": "application/json" } },
  "auth": { "kind": "provisioned-api-key" },
  "environment": { "requireApiKey": false },
  "seed": { "fixture": "fresh-install" },
  "compare": {
    "status": 200,
    "headers": {
      "content-type": "exact",
      "content-length": "exact",
      "x-request-id": "presence",
      "x-model-catalog-version": "exact",
      "x-omniroute-route-class": "exact"
    },
    "json": {
      "mode": "shape",
      "requiredPaths": ["object", "data"],
      "orderedArray": "data",
      "normalizePaths": { "data[].created": "catalog-created" }
    },
    "capabilities": ["http", "json", "auth"]
  },
  "timeout": "api",
  "tags": ["models", "catalog"]
}
```

Not parity requirements: body byte length, `data` length across environments
(fixed only by the shared fixture within a run), `x-request-id` values,
`created` values, wall-clock timing.

### 14.2 `HEAD /v1/models`

`HEAD`, `compare.status: 200`, `content-type: exact`, `content-length: absent`,
`connection: exact("close")`, `x-model-catalog-version: absent`,
client body bytes zero (captured as `body.bytes: 0`), `capabilities:
["http"]`. Auth-required variant without credential expects 401 with body
suppressed — the observation records `http-response` + zero bytes, and the
verdict distinguishes it from a timeout by outcome (§5.1).

### 14.3 `OPTIONS /v1/models`

`OPTIONS` with representative preflight headers, `compare.status: 204`,
`content-type/content-length/allow: absent`,
`access-control-allow-methods: exact` (global list
`GET, POST, PUT, DELETE, PATCH, OPTIONS` — the pipeline value, not the
route-local export), `access-control-allow-headers: exact` (echo or the
surface default), `vary: exact("Origin")`, auth bypassed (no credential
needed even when `requireApiKey: true`).

### 14.4 `/models` alias

Same scenario as §14.1 with `http.path: /models`. Alias-specific assertions:
identical `status`, identical body hash class (equal bodies under identical
seeds — the rewrite preserves method/query/headers), `CLIENT_API` route class
on both. The scenario does not special-case the alias anywhere else: one
scenario shape, two paths, shared expectations — the reuse the schema was
designed for (§3).

### 14.5 Trailing-slash redirect

`GET /v1/models/?limit=1&x=probe` (and the `/models/` twin):
`compare.status: 308`, `location: exact` (query-preserving:
`/v1/models?limit=1&x=probe`), `refresh: exact`, no route-class header, no
auth evaluation, `followRedirects: false`. A followed request would be a new
scenario, not part of this observation.

### 14.6 Neighboring JSON 404

`GET /v1/task004-does-not-exist`: `status: 404`, `error-triple`
(`type: not_found`, `code: unknown_route`, message contains `Unknown API
route`), `CLIENT_API` class, no catalog-version header. The
`/v1/models/<missing>` twin expects `type: invalid_request_error`,
`code: model_not_found` instead — a different scenario, same schema. The
root HTML neighbor (`/models-neighbor-does-not-exist`) asserts `status: 404`,
`content-type: prefix("text/html")`, route-class absent — and explicitly does
NOT assert body bytes (page HTML is not a stable contract).

### 14.7 Schema fit

Every verified predicate in the two contract artifacts maps to a `compare`
entry, a named header policy, a JSON mode, or a declared non-requirement. No
contract fact required executable logic, per-scenario code, or a second
scenario copy per backend. Where the contracts record platform dependence
(case-insensitive filesystems, production-standalone framing), the
corresponding scenarios carry it as a documented constraint, not a universal
assertion (§16).

---

## 15. Staged implementation plan [design-decision]

Deliberately small stages; each is independently reviewable and leaves the
tree green. Later prompts implement one stage at a time — no stage redesigns
the data model.

1. **Scenario loader + validation — IMPLEMENTED (Task 006).** Entry point
   `loadScenarioFile()` in `native/compat/scenarioLoader.ts`; ajv (already a
   direct dependency) compiles the canonical
   `contracts/compat-scenario.schema.json` at runtime, so no second schema
   copy exists. Accepts one scenario object or a scenario-set envelope
   (`{ ..., "scenarios": [...] }`, the shape of
   `contracts/compat-models-examples.json`); unknown top-level fields
   rejected; per-entry diagnostics with stable `(index, path, code)` ordering;
   loaded scenarios deep-frozen. Narrow semantic layer only: set-level
   duplicate-`id` detection, `persistence.tables` allowlist enforcement, and
   the `redirect`-vs-`followRedirects` conflict. Two clarifications from
   implementation (no schema change): envelope metadata outside `scenarios`
   is tolerated and ignored; `http.body` artifact refs are carried through
   shape-checked but unresolved — resolving them against a corpus root
   belongs to the execution stage, which owns corpus roots. Exit criteria met:
   the six §14 examples load clean; malformed fixtures fail with field paths
   (`tests/unit/compat-scenario-loader.test.ts`, 23 tests). No execution.
2. **Observation/result types — IMPLEMENTED (Task 007).** Runtime model in
   `native/compat/observation.ts` (discriminated observation union with
   `status` required on `http-response` and forbidden otherwise by type,
   per-outcome factories, header-name lowercasing at construction with
   repeats/order preserved, canonical-schema validator) and
   `native/compat/result.ts` (`pass`/`fail`/`error` verdict types with
   empty/non-empty mismatch shapes enforced by type, factories,
   canonical-schema validator). Narrow semantic layer only: status-iff-
   `http-response` and the pass/fail-to-mismatch-count relationship — both
   inexpressible in the schemas, both enforced at runtime; every other rule
   stays schema-side. Factories and validators deep-freeze outputs; mutable
   working copies come from explicit clone helpers (normalization boundary).
   Shared ajv/diagnostic machinery factored into private
   `native/compat/schemaCore.ts` with Task 006 behavior preserved
   (its 23 tests pass unchanged). Readings confirmed during implementation
   (no schema change): outcomes with nothing captured use the empty
   representation (`headers: []`, zero-byte body); body metadata already
   covers the bounded-capture strategy. Exit criteria met: JSON round-trip
   and revalidation for response, failure, pass, fail, and error cases plus
   invalid-state guards (`tests/unit/compat-observation-result.test.ts`,
   29 tests). No comparison, no reporting.
3. **Single-backend HTTP executor — IMPLEMENTED (Task 008).** Entry point
   `executeCompatScenario()` in `native/compat/httpExecutor.ts`: loaded
   scenario + caller-provided backend identity/base URL + already-resolved
   request inputs (`resolvedHeaders` merged over scenario headers for later
   auth stages; scenario string bodies sent as UTF-8 bytes) → bounded
   non-streaming fetch → raw frozen observation via the Task 007 factories
   (the result model is never imported). Redirects obey the scenario policy
   (`manual` by default, so raw `308 + Location` is observed verbatim;
   `follow` only when the scenario declares it). Every request carries a
   finite `AbortSignal.timeout` (no unbounded fetch): the scenario category
   maps through `resolveCompatTimeoutMs()` to the §12 evidence anchors
   (`health` 5 s, `api` 10 s, `chat` 20 s, `stream` 30 s) with two explicit
   Stage 3 choices — `bulk` 60 s (no evidence anchor exists) and the `api`
   category when the scenario declares none. Transport failures classify by
   structured error identity (`classifyCompatTransportError()`:
   `TimeoutError` → `timeout` with partial bytes retained, `ECONNREFUSED`
   → `connection-refused`, `UND_ERR_SOCKET`/`ECONNRESET`/`EPIPE` →
   `connection-reset`, anything else → `harness-error`); `backend-exit` is
   never emitted here because the executor holds no process handle, and a
   missing response is never status `0`. Response bodies stream
   incrementally and never retain past `capture.maxBodyBytes` (Stage 3
   default 1 MiB, inside the §12 evidence range); over-cap bodies stay
   `http-response` with `truncated: true` + `capName`, and JSON parses only
   fully-captured bodies under `capture.maxParseBytes`, so malformed JSON
   stays an `http-response` with a recorded reason. `http.body` artifact
   refs are rejected as `harness-error` (resolving them against a corpus
   root belongs to a later corpus layer). Readings confirmed during
   implementation (no schema change): Stage 3 keeps capture in-memory only
   — the §12 "spill to artifact files" applies to later stages that own
   artifact directories, not to this executor, which performs no filesystem
   I/O; undici merges repeated response headers except `set-cookie`
   (kept split, order preserved), adds transport headers the scenario never
   declared, forbids `CONNECT`/`TRACE`/`TRACK` and any `GET`/`HEAD` body
   (those scenarios yield `harness-error`), and exposes `HEAD` bodies as a
   null stream (zero bytes captured without consulting `Content-Length`);
   failure details are fixed secret-free templates that never echo header
   values, bodies, query values, URL userinfo, or thrown error text. Exit
   criteria met: ordinary responses (GET/HEAD/OPTIONS/DELETE/redirect/
   chunked/empty/malformed-JSON/at-cap/over-cap) produce valid frozen
   observations and every transport class stays distinct
   (`tests/unit/compat-http-executor.test.ts`, 35 tests, loopback only).
   No backend lifecycle, no auth provisioning, no comparison.
4. **Reference backend adapter.** `prepare/start/waitReady/stop/health` for the
   Node backend per §11 (env shape §1.1, readiness §1.1, provisioning §1.2,
   cleanup §12). Exit: one-command run of §14 examples against an
   adapter-managed reference backend; orphan check clean.
5. **Status + header comparison.** Comparator for §7 policies + `error-triple`
   checks + no-stack-leak oracle (§8.3). Exit: §14.2–14.6 verdicts (pass) on
   reference-vs-reference runs; injected header/status mutations fail with
   exact paths.
6. **JSON comparison.** Modes of §8.1 + Models `data`-array rule (§8.2) +
   `catalog-created` normalization. Exit: §14.1 passes reference-vs-reference
   on the seeded fixture; reordered-`data` and dropped-required-field
   mutations fail.
7. **Dual-backend execution.** Native adapter stub against the interface (§11)
   - paired runs + result/report writer (§13) + artifact layout. Exit: full
     §14 corpus runs paired (native side reports `harness-error`/unreachable
     until a backend exists — the pairing machinery itself is green).
8. **Persistence observation.** Fixture seeding + constrained selectors +
   settle re-reads (§10). Exit: `last_used_at`-style volatile cases behave per
   §10 on reference-vs-reference.
9. **SSE support.** Bounded capture + event parser + ordered-sequence compare +
   terminal contract (§9). Exit: recorded SSE vectors compare; truncation and
   missing-terminal cases verdict correctly.
10. **Cancellation support.** Client-abort steps + post-cancel frame accounting
    - persistence equality (§9). Exit: abort scenarios verdict on
      reference-vs-reference.
11. **WebSocket support.** Handshake + frame-sequence observation for the
    parity-scoped WS surfaces (scope pinned by the implementing prompt after
    the WS inventory then current — `/v1/ws`, responses bridge, live
    dashboard — is re-verified). Exit: handshake parity scenarios green
    reference-vs-reference.

---

## 16. Unresolved design questions

1. **Production-standalone framing** [verified-contract unresolved]: the 405
   empty-body shape and HTML 404 bytes are verified on the dev server only.
   Scenarios pin dev-server behavior; a standalone-runtime probe (future work)
   may split expectations by `backend.mode`.
2. **Case-insensitive filesystems** [verified-contract unresolved]: mixed-case
   tail resolution stays platform-gated, not a universal assertion.
3. **`last_used_at` TTL conditions** [verified-contract unresolved]: the exact
   validation/update TTL that suppresses the write is not pinned; persistence
   scenarios assert `changed`-class expectations, never exact stamps.
4. **Native readiness endpoint**: defined by the slice that introduces the C
   server (§11 reserves a configurable probe; no assumption of Next paths).
5. **Full-catalog `exact` feasibility**: viable only with byte-identical seeds
   and deterministic build order; the default stays `shape` + ordered
   id-sequence until a stage proves otherwise.
6. **Timing-as-contract inventory**: `Retry-After` values, keepalive intervals,
   and `reset after` messaging need per-surface verification before any
   `timing-contract` scenario is written; wall-clock stays informational.
7. **WS parity scope**: which WS surfaces are contractual (bridge vs live
   dashboard vs provider-internal sockets) is decided at stage 11 with fresh
   source evidence, not here.

---

## Appendix A. What this task did not change (and why)

- `MIGRATION_PLAN.md` §5 needs no edit: the topology, nine comparison
  dimensions, resource-capture, and promote/rollback rule there are consistent
  with this data model (dimensions → `capabilities`, `ignore.json` → explicit
  per-scenario `normalizePaths`/named ignore policies, `.dump` compare →
  constrained selectors in §10).
- `FEATURE_PARITY.md` needs no edit: no parity information changed; Models
  rows remain `NOT STARTED` on the C side, which is accurate — designing the
  harness implements nothing.
- No runtime, build, or production files touched: the commit carrying this
  document plus the four `contracts/` companions is documentation/specification
  only.
