# OmniRoute native backend

> Task 011 foundation. This directory holds the first production C for
> OmniRoute-in-C: a minimal executable skeleton with deterministic
> startup/shutdown and basic memory instrumentation. It is **not** an HTTP
> server yet — no port is bound, no network is contacted, no database is
> opened. User-facing backend functionality is NOT STARTED (`FEATURE_PARITY.md`
> stays uniformly `NOT STARTED` until vertical slices land with harness proof).

Design sources: `docs/native-backend/MIGRATION_PLAN.md` §1 (this layout),
`docs/native-backend/MEMORY_MODEL.md` (metric semantics), `docs/native-backend/ARCHITECTURE.md`.

## Language baseline

**ISO C11, strict** (`CMAKE_C_STANDARD 11`, `REQUIRED`, `EXTENSIONS OFF`).
C11 is the newest standard guaranteed by GCC, Clang, and the older Apple
Clang behind the iOS 12 SDK — portability first. No C++, no Rust, no
handwritten assembly. Linux-only code stays behind explicit `#ifdefs`
until the `platform/` split happens with iOS work.

## Build system

**CMake ≥ 3.21, Linux-first** (named by the Task 001 migration design).
Out-of-tree builds only (`native/build*`, gitignored). No dependency
download during configure/build: the skeleton links libc only.

| Build                   | Configure command                                                            |
| ----------------------- | ---------------------------------------------------------------------------- |
| Debug, GCC              | `cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Debug`          |
| Debug, Clang            | `CC=clang cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Debug` |
| Release                 | same with `-DCMAKE_BUILD_TYPE=Release` (add `-DCMAKE_C_COMPILER=gcc/clang`)  |
| Sanitizers (Debug only) | add `-DOMNIROUTE_SANITIZE=ON` (ASan + UBSan; refused for Release)            |

Then `cmake --build <dir>` and `ctest --test-dir <dir> --output-on-failure`.
Repo shortcut (Debug, default compiler): `npm run test:native`.

Warnings (`-Werror` on the target): `-Wall -Wextra -Wpedantic -Wshadow
-Wstrict-prototypes -Wold-style-definition -Wconversion`, supported
identically by GCC and Clang. Release is explicit `-O2` — no `-ffast-math`,
no `-march=native`, no LTO.

## Layout

```text
native/
  README.md                  # this file (build matrix, behavior, baseline)
  CMakeLists.txt             # Linux-first build described above
  include/omniroute/         # version.h, exit_code.h, meminfo.h, arena.h, bytebuf.h, listener.h, poller.h, accepted.h
  src/
    main.c                   # entry point, tiny CLI, lifecycle
    meminfo.c                # Linux /proc/self/status RSS hook (+ stub elsewhere)
    arena.c                  # bounded arena allocator (Task 012)
    bytebuf.c                # bounded reusable byte buffer (Task 013)
    listener.c               # loopback TCP listener lifecycle (Task 014)
    poller.c                 # bounded readiness wait (Task 015)
    accepted.c               # accepted-socket owner + bounded accept4 drain (Task 016)
  tests/
    CMakeLists.txt           # CTest cases (CLI, meminfo, units, network-boundary gate)
    check_network_boundary.sh  # review gate: socket setup confined to src/listener.c,
                              # readiness wait to src/poller.c, accept path plus
                              # accepted-FD lifecycle to src/accepted.c; deferred
                              # layers (loop/IO/threads/TLS) banned everywhere in
                              # production sources, heap allocation banned in
                              # src/accepted.c
```

`compat/` holds the TypeScript harness stages (Tasks 006–010); the C
harness client arrives with later slices. No placeholder layers are created
beyond what this task needs.

## Executable behavior

Binary: `omniroute-native`. Runs independently of Node/V8.

```text
$ omniroute-native
omniroute-native 0.1.0 ready (release, c11)

$ omniroute-native --version
omniroute-native 0.1.0

$ omniroute-native --meminfo
rss_kb: 1748
peak_rss_kb: 1748

$ omniroute-native --bogus
omniroute-native: unknown option '--bogus'
Usage: omniroute-native [--help] [--version] [--meminfo]
```

CLI precedence: `--help` > `--version` > normal run. Unknown options and
positional arguments exit `2` with a one-line stderr diagnostic. Diagnostics
go to stderr; CLI output goes to stdout; no timestamps; no secrets.

Exit codes (`include/omniroute/exit_code.h`): `0` success, `1` runtime
failure, `2` invalid usage.

## Memory diagnostics

`--meminfo` reports the kernel's `VmRSS` (whole-process resident set,
shared mappings included) and `VmHWM` (lifetime high-water mark) from
`/proc/self/status` — the same semantics as the Task 002 reference
tooling. Unavailable platforms print `unavailable` (never confused with
zero). No threads, no polling, no dependencies, no heap allocation in the
measurement path. This is process-level observation, not allocator
accounting (no allocator exists yet by design).

## Arena allocator (Task 012)

Bounded bump allocator for future request-scoped allocations
(`include/omniroute/arena.h`, `src/arena.c`, unit-tested by
`tests/test_arena.c` — 73 checks via CTest `arena-unit`).

- **Capacity**: explicit and finite at init; exhaustion returns NULL. No
  growth, no heap fallback, no hidden block chains.
- **Backing**: borrowed caller buffer (`omni_arena_init_borrowed`) or one
  owned allocation made at init and released at destroy
  (`omni_arena_init_owned`). Steady-state allocation never calls malloc.
- **Lifetime**: allocations stay valid until reset/destroy. No individual
  free. Reset rewinds `used` to zero, keeps backing and capacity, does not
  clear contents (no erasure guarantee). Destroy frees owned backing
  exactly once, never borrowed storage; NULL, inert, and repeated destroy
  are safe no-ops.
- **Alignment**: explicit power-of-two parameter, computed from the absolute
  address (odd caller buffers still yield aligned pointers); invalid
  alignment returns NULL. Default: `OMNI_ARENA_ALIGN_DEFAULT`
  (`_Alignof(max_align_t)`). All padding/offset arithmetic is
  subtraction-first and overflow-safe (SIZE_MAX requests fail on bounds
  alone; no giant buffer is ever attempted).
- **Zero-size**: returns the aligned current position, consumes nothing.
- **Accounting**: `capacity` / `used` / `remaining` / lifetime-max
  `high_water`, all local to the arena. Failed allocations change nothing.
- **Threads**: none — single-owner, externally synchronized by contract.

`main` does not instantiate an arena (separate static lib linked only into
the test executable), so the Task 011 RSS baseline below is unaffected by
this task — and no arena-efficiency claim is drawn from it.

## Byte buffer (Task 013)

Bounded reusable byte staging for future socket receive staging and
incremental parsing (`include/omniroute/bytebuf.h`, `src/bytebuf.c`,
unit-tested by `tests/test_bytebuf.c` — 202 checks via CTest
`bytebuf-unit`, including a 400-op deterministic invariant stress test).

- **Hard cap**: explicit finite capacity at init (zero capacity rejected,
  symmetric with the arena). No growth path exists — no realloc, no chunks,
  no spill, no heap fallback. Exhaustion fails cleanly with state unchanged.
- **State**: linear `read`/`write` offsets, invariant
  `read <= write <= capacity` (linear, not a ring: MEMORY_MODEL §2.5 and
  MIGRATION_PLAN §1 mention ring/chain buffers only as future sketches, so
  the auditable linear + explicit-compact form stands until a consumer
  proves otherwise). Consumed prefix, readable region, contiguous free tail.
- **Backing**: borrowed caller storage or one owned allocation at init
  (`init_borrowed` / `init_owned`); steady-state operations never allocate.
  Destroy frees owned backing exactly once, never borrowed storage.
- **Views**: zero-copy readable (`read_ptr` + length; NULL when empty) and
  writable tail (`write_ptr` + length; NULL when full) for a future socket
  read placed directly into storage — no I/O here. Views are borrowed and
  invalidate on append/commit/consume/compact/reset/destroy.
- **Mutation**: `commit` advances the write side after external fill;
  `append` copies caller bytes tail-only (no implicit compaction —
  `consume` a prefix then `compact` first); `consume` advances the read
  side (full consume canonicalizes to `read == write == 0`); `compact`
  memmoves unread bytes to the start (the only O(n) op, always explicit).
  Append overlap is safe by construction (memmove snapshot semantics).
- **Bytes, not strings**: no NUL appended, no text assumption; embedded
  zeros round-trip. `reset` reuses backing without clearing (no erasure
  guarantee). `high_water` tracks peak readable across reset.
- **Safety**: subtraction-first bounds throughout (near-`SIZE_MAX` inputs
  fail on bounds alone); NULL/inert/destroyed inputs fail safe or no-op.
- **Threads**: none — single-owner, externally synchronized by contract.

`main` does not instantiate a buffer (separate static lib linked only into
`test_bytebuf`), so the Task 011 RSS baseline below is unaffected by this
task — and no buffer-efficiency claim is drawn from it.

## TCP listener (Task 014)

First networking primitive: loopback-IPv4 TCP listen + descriptor
lifecycle only (`include/omniroute/listener.h`, `src/listener.c`,
unit-tested by `tests/test_listener.c` — 52 checks via CTest
`listener-unit`, loopback-only, self-cleaning).

- **Scope**: IPv4 `127.0.0.0/8` bind enforced in code (`0.0.0.0` and
  non-loopback rejected); IPv6 deferred. Explicit or ephemeral (`0`) port;
  actual port always read back from kernel socket state. Tests use
  ephemeral ports only — operator port `20128` is never bound.
- **Creation**: TCP stream socket with atomic `SOCK_NONBLOCK |
SOCK_CLOEXEC`, verified after creation (fails closed); `SO_REUSEADDR`
  only — no `REUSEPORT`, keepalive, `NODELAY`, or buffer tuning.
  Bounded backlog `OMNI_LISTENER_BACKLOG` (16; kernel may clamp).
- **Ownership**: success publishes exactly one owned FD; destroy closes it
  exactly once (single close, never retried — see header). Invalid
  sentinel is `-1` (FD `0` is valid; proven by a fork-isolated FD-0
  lifecycle test). Borrowed-FD accessor for future event-loop
  registration; NULL/inert/repeated destroy are safe no-ops.
- **Errors**: status enum + errno captured before cleanup; bounded EINTR
  retries on the one-shot setup calls; ordinary bind conflicts fail
  without aborting, leaking nothing.
- **Not this task**: no accept path, no connection objects, no payload
  input/output, no event loop (epoll/poll/select), no HTTP, no threads,
  no signal changes.
- **Boundary gate**: `tests/check_network_boundary.sh` (CTest
  `network-source-boundary`) confines socket/FD tokens to
  `src/listener.c` — arena, bytebuf, meminfo, and main stay socket-free —
  and bans accept/loop/IO/thread/TLS tokens in all production sources.

`main` never binds a listener (separate static lib linked only into
`test_listener`), so the Task 011 baseline below still describes a
socket-free executable — and no listener-overhead claim is drawn from it.

## Bounded readiness (Task 015)

Portable readiness observation over borrowed descriptors
(`include/omniroute/poller.h`, `src/poller.c`, unit-tested by
`tests/test_poller.c` — 130 checks via CTest `poller-unit`, plus one
loopback-listener integration proof).

- **Backend**: POSIX readiness wait first — small, auditable, portable —
  not epoll/select/io_uring/kqueue. A later task may replace or augment
  the mechanism after profiling; the registration contract stays.
- **Bounds**: explicit finite capacity at init (zero rejected); dense live
  prefix, no growth, no realloc; beyond capacity registration fails with
  state unchanged. Caller-provided parallel arrays or exactly one owned
  block at init (arena/bytebuf dual-model symmetry); steady-state
  add/remove/update/wait/reset/destroy never allocate.
- **Borrowed FDs**: the poller never closes, dups, or reconfigures a
  descriptor. Owners keep lifetime control; the owner must remove a
  registration before closing/recycling its FD (descriptor numbers can be
  reused). Destroy/reset/remove close nothing — proven by fd-census and
  bystander checks, including listener-usable-after-poller-destroy.
- **Identity**: caller-provided opaque 64-bit token per registration,
  round-tripped in every event; duplicate-FD registration rejected.
- **Masks**: project-level interest (readable/writable; empty/unknown
  rejected) and readiness (readable/writable/error/hangup/invalid).
  Error/hangup/invalid surface even when unrequested; urgent-data
  signaling is documented as not yet surfaced.
- **Wait**: caller-owned event array + capacity; capacity must cover every
  live registration or the wait fails without polling — never truncated,
  never overrun (ASan-proven). Events emit in registration order.
  Timeout is int64 milliseconds: 0 probes, positive bounds, negatives
  and values above `INT_MAX` rejected (no infinite wait without a wakeup
  mechanism). Empty poller returns zero events immediately.
- **Interruption**: an interrupted wait returns `INTERRUPTED` with
  nothing consumed — never retried internally (self-signal tested).
- **Stale FDs**: externally closed descriptors surface as invalid
  readiness; nothing is auto-closed or auto-removed.
- **Not this task**: no accept path, no connection objects, no payload
  input/output, no callbacks, no wakeup FD, no threads, no HTTP.
- **Boundary gate**: `check_network_boundary.sh` now also confines the
  readiness-wait token to `src/poller.c`.

`main` never instantiates a poller (separate static lib linked only into
`test_poller`), so the Task 011 baseline below is unaffected by this
task — and no readiness-overhead claim is drawn from it.

## Accepted socket (Task 016)

Minimal accepted-connection FD owner plus a bounded nonblocking drain
(`include/omniroute/accepted.h`, `src/accepted.c`, unit-tested by
`tests/test_accepted.c` — 467 checks via CTest `accepted-unit`,
loopback-only, self-cleaning, zero payload bytes).

- **Scope**: one owned client descriptor per `omni_accepted`, nothing
  else — no byte buffers, no arenas, no parser/HTTP state, no peer
  fields, no threads. The name stays narrow on purpose so a richer
  future connection runtime can arrive under its own name.
- **Ownership**: single accept borrows the listener descriptor and
  publishes ownership only after `accept4` succeeds and the new
  descriptor verifies `O_NONBLOCK` plus `FD_CLOEXEC`. Failed attempts
  publish nothing; the success-but-unverified path closes the new
  descriptor exactly once and leaves the destination inert. Destroy
  closes exactly the owned client descriptor once (single close, never
  retried — same policy as the listener), never the listener or a
  bystander. Invalid sentinel is `-1`; descriptor `0` is valid
  (proven by a fork-isolated FD-0 acceptance test).
- **Mechanism**: Linux `accept4` with atomic `SOCK_NONBLOCK |
SOCK_CLOEXEC`; the POSIX fallback for other platforms is deferred,
  not implemented. No reverse lookup, no payload input/output.
- **Taxonomy**: `OK`, `DRAINED` (would-block is normal control flow),
  `INTERRUPTED` (surfaced, never retried internally),
  `TRANSIENT` (`ECONNABORTED`/`EPROTO` name a failed pending
  handshake; the only per-connection-abort evidence at this layer),
  `CAPACITY`, `ERR_INVALID` (caller-contract violations, checked
  before any wait), `ERR_FATAL`. Every result carries the stop-point
  errno plus the published count.
- **Bounded drain**: caller-owned output plus explicit capacity; stops
  on drained, full, interrupted, transient, or fatal — never exceeds
  capacity, never allocates (heap calls are banned in `src/accepted.c`
  by the boundary gate). Capacity-reached reports distinctly from
  queue-drained so the future runtime revisits listener readiness.
  Partial success keeps its owners: already published entries stay
  live and owned by the caller whatever stops the call. Every output
  slot must already be inert (verified before accepting) so a stray
  live owner can never be overwritten; only the published prefix
  becomes live, the tail is left untouched.
- **Integration proof**: listener → poller READ readiness → bounded
  drain, with the poller borrowing only the listener descriptor
  throughout; an accepted descriptor separately proves it can later
  register as a borrowed writable FD with no payload transferred.
- **Boundary gate**: `check_network_boundary.sh` now confines socket
  setup to `src/listener.c`, descriptor lifecycle to `src/listener.c`
  - `src/accepted.c`, the readiness wait to `src/poller.c`, and the
    accept path to `src/accepted.c` — with heap allocation banned in
    `src/accepted.c` as a negative control.

`main` never accepts (separate static lib linked only into
`test_accepted`), so the Task 011 baseline below is unaffected by this
task — and no accept-overhead claim is drawn from it.

## Initial baseline (Task 011, measured 2026-09-19)

Environment: Linux 7.2.4-zen2 x86_64, 8 CPUs, 16 GiB RAM; GCC 16.2.1,
Clang 22.1.8; Release + Debug builds from this tree.

| Metric                              | Value                                                                                                                                                               |
| ----------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Executable size (Release, GCC)      | 16,640 bytes (`text+data+bss` ≈ 4.8 KiB)                                                                                                                            |
| Executable size (Debug, GCC)        | 22,504 bytes                                                                                                                                                        |
| Startup/current RSS (`--meminfo`)   | ≈ 1,748 kB (one-shot print-and-exit, not a server)                                                                                                                  |
| Peak RSS (`VmHWM`)                  | ≈ 1,748 kB                                                                                                                                                          |
| Explicit heap allocation in sources | init-time only: exactly one owned backing block each in arena / bytebuf / poller owned-init paths; steady-state operations never allocate (libc internals excluded) |

Not a competition with the full Node server (200–400 MB idle RSS at
`OMNIROUTE_MEMORY_MB=512` covers hundreds of routes, providers, and
caches this skeleton does not have). These numbers exist so future slices
can detect regressions from day one.

Task 012 regression: unchanged — 16,640 bytes, RSS 1,748 kB, peak
1,748 kB (`nm` confirms no arena symbols in `omniroute-native`; the arena
lives in a separate static lib linked only into `test_arena`).

Task 013 regression: unchanged — 16,640 bytes, RSS ≈ 1,748 kB, peak
≈ 1,748 kB (`nm` confirms no arena or bytebuf symbols in
`omniroute-native`; the buffer lives in a separate static lib linked only
into `test_bytebuf`). RSS samples 1,748–1,752 kB across runs (one-shot
print-and-exit variance, same qualification as the Task 011 baseline).

Task 014 regression: unchanged — 16,640 bytes, RSS ≈ 1,748 kB, peak
≈ 1,748 kB (`nm` confirms no listener, socket, or bind symbols in
`omniroute-native`; the listener lives in a separate static lib linked
only into `test_listener`, and `main` binds nothing). RSS samples
1,748–1,752 kB across runs (same one-shot variance).

Task 015 regression: unchanged — 16,640 bytes, RSS 1,748 kB, peak
1,748 kB (`nm` confirms no poller, listener, socket, bind, or readiness
symbols in `omniroute-native`; the poller lives in a separate static lib
linked only into `test_poller`, and `main` waits on nothing).

Task 016 regression: unchanged — 16,640 bytes, RSS 1,748 kB, peak
1,748 kB (`nm` confirms no accepted, poller, listener, socket, bind, or
readiness symbols in `omniroute-native`; the accept layer lives in a
separate static lib linked only into `test_accepted`, and `main` accepts
nothing).

## Platform boundary

Linux x86_64/arm64 is the build target; the `/proc` reader is the only
Linux-specific code, isolated in `src/meminfo.c` behind `__linux__` with
an explicit unavailable stub elsewhere. No iOS code, no Objective-C/Swift,
no Apple frameworks yet. The `platform/` + `platform-toolchain/` split
from the migration design happens when iOS work starts.

## Intentionally deferred

Event loop (epoll/io_uring/threads), accept path and connection objects,
socket payload input/output, HTTP, `/health`, `/v1/models`, TLS, SQLite,
crypto, auth, providers, routing, streaming, compression, MCP, A2A,
Objective-C/Swift/assembly. (Arenas, byte buffers, listener lifecycle,
readiness observation, and accepted-socket ownership with its bounded
accept drain are landed primitives now — see above.) Each
remaining item gets its own reviewable task.
