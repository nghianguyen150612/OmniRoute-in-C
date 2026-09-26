# OmniRoute native backend

> Task 011 foundation. This directory holds the first production C for
> OmniRoute-in-C: a minimal executable skeleton with deterministic
> startup/shutdown and basic memory instrumentation. It is **not** an HTTP
> server yet — Task 033 adds only a standalone request-line parser, which is
> not wired into startup; no port is bound, no network is contacted, and no
> database is opened. User-facing backend functionality is NOT STARTED (`FEATURE_PARITY.md`
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
  include/omniroute/         # version.h, exit_code.h, meminfo.h, arena.h, bytebuf.h, listener.h, poller.h, accepted.h, recv.h, send.h, connection.h, registry.h, reactor.h, connection_reactor.h, runtime.h, event_loop.h, connection_io.h, connection_session.h, connection_runtime.h, connection_manager.h, connection_admission.h, listener_admission.h, connection_dispatch.h, connection_policy.h, http_request_line.h
  src/
    main.c                   # entry point, tiny CLI, lifecycle
    meminfo.c                # Linux /proc/self/status RSS hook (+ stub elsewhere)
    arena.c                  # bounded arena allocator (Task 012)
    bytebuf.c                # bounded reusable byte buffer (Task 013)
    listener.c               # loopback TCP listener lifecycle (Task 014)
    poller.c                 # bounded readiness wait (Task 015)
    accepted.c               # accepted-socket owner + bounded accept4 drain (Task 016)
    recv.c                   # bounded nonblocking recv into bytebuf (Task 017)
    send.c                   # bounded nonblocking send from immutable span (Task 018)
    connection.c             # protocol-agnostic connection owner (Task 019)
    registry.c               # bounded connection registry (Task 020)
    reactor.c                # bounded poller-to-callback reactor (Task 021)
    connection_reactor.c     # connection lifecycle/reactor adapter (Task 022)
    runtime.c                # runtime lifecycle coordinator (Task 023)
    event_loop.c             # bounded synchronous runtime event loop (Task 024)
    connection_io.c          # bounded connection I/O state machine (Task 025)
    connection_session.c     # bounded connection/session integration (Task 026)
    connection_runtime.c     # bounded connection runtime binding (Task 027)
    connection_manager.c     # bounded runtime membership (Task 028)
    connection_admission.c   # bounded accept-to-manager admission (Task 029)
    listener_admission.c     # bounded listener-readiness admission bridge (Task 030)
    connection_dispatch.c    # bounded manager/session readiness dispatch (Task 031)
    connection_policy.c      # bounded lifecycle and reactor-interest policy (Task 032)
    http_request_line.c      # bounded zero-copy HTTP request-line parser (Task 033)
  tests/
    CMakeLists.txt           # CTest cases (CLI, meminfo, units, network-boundary gate)
    test_connection_manager.c # bounded manager unit, rollback, ownership, and stress checks
    test_connection_admission.c # bounded admission, rollback, ownership, and stress checks
    test_listener_admission.c # bounded reactor bridge and event-loop integration checks
    test_connection_dispatch.c # managed READ/WRITE dispatch and admission integration checks
    test_http_request_line.c   # bounded parser grammar, limits, and truncation checks
    check_network_boundary.sh  # review gate: socket setup confined to src/listener.c,
                              # readiness wait to src/poller.c, accept path plus
                              # accepted-FD lifecycle to src/accepted.c, receive
                              # to src/recv.c, send to src/send.c, and
                              # connection/reactor binding to
                              # src/connection_reactor.c, event-loop
                              # orchestration to src/event_loop.c, connection I/O to
                              # src/connection_io.c, session coordination to
                              # src/connection_session.c, runtime binding to
                              # src/connection_runtime.c, membership to
                              # src/connection_manager.c, and admission to
                              # src/connection_admission.c and
                              # src/connection_dispatch.c; alternate
                              # loop backends, queues, IO/scatter/TLS are
                              # banned everywhere in production sources, with
                              # heap allocation banned in the bounded native
                              # networking/composition layers
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
  input/output, no event-loop backend (epoll/select), no HTTP, no threads,
  no signal changes.
- **Boundary gate**: `tests/check_network_boundary.sh` (CTest
  `network-source-boundary`) confines socket/FD tokens to
  `src/listener.c` — arena, bytebuf, meminfo, and main stay socket-free —
  and bans alternate-loop/IO/thread/TLS tokens in all production sources.

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

## Bounded socket receive (Task 017)

Allocation-free receive from one accepted nonblocking socket directly into
the writable tail of one existing `omni_bytebuf`
(`include/omniroute/recv.h`, `src/recv.c`, unit-tested by
`tests/test_recv.c` — 285 checks via CTest `recv-unit`, loopback-only,
self-cleaning). The receive layer borrows both objects: `omni_accepted` keeps
sole ownership of the accepted FD, and `omni_bytebuf` keeps sole ownership of
its backing storage. Receive never closes, destroys, resets, compacts,
reallocates, or replaces either resource, and it never registers anything in
the poller.

- **One-shot API**: `omni_recv_once` obtains the byte buffer writable view,
  caps the request at `SSIZE_MAX`, performs at most one `recv(fd, tail, len,
0)`, and commits exactly the positive return count. There is no temporary
  receive buffer or payload copy. Invalid arguments and a zero writable tail
  perform no syscall; a live full buffer returns `BUFFER_FULL` even when a
  consumed prefix is reclaimable.
- **Status model**: `DATA` carries the exact committed count and zero errno;
  `WOULD_BLOCK` carries `EAGAIN`/`EWOULDBLOCK`; `EOF` carries zero errno and
  leaves the accepted owner and buffered bytes intact; `INTERRUPTED` carries
  `EINTR` without an internal retry; `BUFFER_FULL` is normal bounded-buffer
  control flow; `ERR_FATAL` captures every other receive errno without
  closing the FD; `ERR_INVALID` rejects caller-contract violations.
  `ERR_INTERNAL` is an explicit fail-closed guard for the impossible
  successful-recv/failed-commit invariant path.
- **Bounded drain**: `omni_recv_drain` performs at most the caller's finite
  `max_calls` one-shot attempts. A zero limit is invalid. It stops distinctly
  at `WOULD_BLOCK`, `EOF`, `BUFFER_FULL`, `LIMIT_REACHED`, `INTERRUPTED`,
  `ERR_FATAL`, or `ERR_INVALID`, and returns all bytes committed before the
  terminal status; partial success is never rolled back.
- **Buffer policy**: receive never auto-compacts and never auto-resets. The
  caller explicitly chooses `omni_bytebuf_compact` or `reset` after
  `BUFFER_FULL`. Hard capacity, reclaimable-prefix behavior, partial receive,
  and pending-data preservation are therefore observable and deterministic.
- **Payload boundary**: bytes are opaque and binary-safe; no NUL termination,
  HTTP parsing, framing, or encoding assumptions exist. The receive layer
  adds no send/write path of its own, no server loop, no threads, and no
  protocol behavior.
- **Boundary gate**: `check_network_boundary.sh` confines plain `recv()` to
  `src/recv.c`, rejects forbidden receive flags (`MSG_PEEK`, `MSG_WAITALL`,
  `MSG_DONTWAIT`), keeps generic read/write, scatter/gather, HTTP, and later
  transport layers banned, and bans heap calls in `src/accepted.c`,
  `src/recv.c`, and `src/send.c`.

`main` does not link or call `omni_recv` (the receive library is linked only
into `test_recv`), so the Task 011 executable baseline remains unaffected by
receive runtime activity.

## Bounded socket send (Task 018)

Allocation-free output from one accepted nonblocking socket through an
immutable caller-owned byte span (`include/omniroute/send.h`,
`src/send.c`, unit-tested by `tests/test_send.c` — 382 checks via CTest
`send-unit`, loopback-only, self-cleaning). The send layer borrows the
`omni_accepted` owner and the source span: the owner retains sole FD
ownership, the caller retains payload ownership, and the synchronous call
retains no pointer, length, offset, or queue state after returning.

- **One-shot API**: `omni_send_once` performs zero syscalls for invalid input
  and zero-length input, and at most one nonblocking socket `send()` for a
  nonzero span. It uses the Linux-first `MSG_NOSIGNAL` per-call flag; no
  process-wide SIGPIPE disposition is installed. The accepted FD is already
  `O_NONBLOCK`, so the send layer never calls `fcntl`, changes flags, or
  waits internally.
- **Immutable binary span**: the minimal `{ const void *data, size_t length }`
  span needs no NUL terminator and accepts embedded zero and non-ASCII bytes.
  A NULL data pointer is valid only with zero length. The caller advances its
  own logical offset after a partial positive result; the primitive never
  retains the unsent tail.
- **Safe result model**: `PROGRESS` carries the exact positive byte count;
  `COMPLETE` represents a zero-length no-op or a fully completed drain;
  `WOULD_BLOCK` carries `EAGAIN`/`EWOULDBLOCK`; `INTERRUPTED` carries `EINTR`
  with no retry; `PEER_CLOSED` distinguishes `EPIPE`/`ECONNRESET`; and
  `ERR_FATAL` preserves other socket errno values without closing the FD.
  A nonzero request with a zero syscall return is an explicit internal
  zero-progress result, never success. Invalid input is rejected before any
  syscall.
- **Size boundary**: each syscall request is capped at `SSIZE_MAX`. A
  positive `ssize_t` return is checked against that bounded request before
  conversion to `size_t`; no unchecked signed/unsigned narrowing is used.
- **Bounded drain**: `omni_send_drain` advances through the span with local
  byte-pointer arithmetic only and performs no more than finite
  `max_calls` one-shot attempts. It completes without an extra probe call,
  returns `LIMIT_REACHED` when the budget ends with bytes remaining, and
  preserves every byte sent before `WOULD_BLOCK`, `INTERRUPTED`, peer/fatal
  failure, or the limit. A zero call limit is invalid.
- **Composition boundary**: the send layer never updates the poller,
  creates an output queue or output byte buffer, performs shutdown, formats
  HTTP, starts a server loop, or allocates heap storage. The production
  executable remains unchanged; `omni_send` is linked only into
  `test_send`.
- **Integration proof**: tests register the accepted FD for WRITE through the
  borrowed-only poller, observe readiness, send exact binary bytes, verify
  them on the controlled client, and remove the registration explicitly.
  Additional bounded tests force partial sends and WOULD_BLOCK with a
  test-only socket-buffer limit, prove exact offset resumption, isolate
  peer-close SIGPIPE survival in a child, exercise deterministic stream and
  lifecycle stress, preserve a bystander FD, and compare a `/proc/self/fd`
  census.
- **Boundary gate**: `check_network_boundary.sh` authorizes plain `send()`
  only in `src/send.c` and requires visible `MSG_NOSIGNAL`; it continues to
  reject generic socket `read()`/`write()`, scatter/gather, datagram I/O,
  shutdown, DNS, threads, TLS, event-loop backends, and heap calls in the
  accepted/receive/send production layers.

`main` does not link or call `omni_send` (the send library is linked only into
`test_send`), so the Task 011 executable baseline remains unaffected by send
runtime activity.

Task 018 validation record (2026-09-20): the focused `send-unit` test passed
382 checks; full CTest passed all 16 tests in each GCC Debug, GCC Release,
Clang Debug, Clang Release, GCC ASan+UBSan Debug, and Clang ASan+UBSan Debug
build. `sh native/tests/check_network_boundary.sh native` passed, and the
Release executable remained at 16,640 bytes (`size` dec 4,767) with the
send/receive layers absent from its `nm` symbol set. The three measured
`--meminfo` runs reported RSS/HWM pairs of 1,744/1,744, 1,752/1,752, and
1,748/1,748 kB; these are one-shot process measurements, not send-runtime
memory accounting.

## Protocol-agnostic connection lifecycle (Task 019)

The first long-lived native object is now implemented in
`include/omniroute/connection.h` and `src/connection.c`, with
`tests/test_connection.c` covering 69 checks through CTest `connection-unit`.
It is a caller-storage-backed, protocol-agnostic owner: no HTTP, parser,
request, response, TLS, provider, database, output queue, or event loop is
part of this layer.

- **Preparation and adoption**: `omni_connection_make_inert` prepares fresh
  caller-owned struct storage; `omni_connection_init` borrows one finite
  receive backing range and stores an opaque poller token plus a validated
  READ/WRITE interest mask; `omni_connection_from_accepted` then moves one
  live `omni_accepted` owner into the prepared object. Failed preparation or
  adoption does not steal the accepted FD.
- **Explicit state machine**: the only published transitions are
  `INERT -> READY -> OPEN -> CLOSING -> CLOSED`. `READY` owns only its
  connection-local byte-buffer object and metadata; `OPEN` owns the accepted
  FD and permits I/O; `CLOSING` retains the FD but rejects new payload I/O;
  `CLOSED` has released both resources. `omni_connection_destroy` is
  idempotent and destroys the byte-buffer object before the accepted owner,
  which closes the FD exactly once. FD 0 remains valid because liveness is
  explicit rather than numeric.
- **Receive/send composition**: `omni_connection_recv_once` and
  `omni_connection_recv_drain` delegate to Task 017 and write directly into
  the bounded connection-local `omni_bytebuf`. `omni_connection_send_once`
  and `omni_connection_send_drain` delegate to Task 018, borrowing an
  immutable caller span without retaining its pointer, length, or offset.
  EOF, would-block, interruption, and peer/fatal errors remain primitive
  results; the connection never closes itself or changes state implicitly.
- **Poller relationship**: token and interest metadata are views only. The
  connection never calls, registers with, updates, or removes a poller. The
  caller registers `omni_connection_fd` externally and removes that
  registration before destroy.
- **Bounded memory**: connection production code has no heap calls and no
  growth path. The receive backing remains caller-owned and must outlive the
  connection; destroying the connection releases only the embedded borrowed
  byte-buffer object, not the caller's storage. There is no output storage.
- **Integration proof**: the focused suite verifies accepted-owner transfer,
  failed-initialization preservation, binary NUL/high-byte receive and send,
  exact partial-send offset resumption, bounded `LIMIT_REACHED`, EOF and
  reset handling, external WRITE readiness, FD-0 ownership, SIGPIPE-safe
  peer failure, bystander survival, repeated destroy, 64-cycle lifecycle
  stress, 256 KiB byte-stream stress, and `/proc/self/fd` return to baseline.
- **Boundary gate**: the updated gate keeps direct socket setup, accept,
  receive, send, readiness, and descriptor lifecycle in their dedicated
  modules; `connection.c` is authorized only for lifecycle composition and
  primitive delegation. It adds no direct socket call or heap call, and the
  gate continues to reject generic I/O, event-loop backends, threads, TLS,
  HTTP, and output-queue code.

`main` still links only `src/main.c` and `src/meminfo.c`; `omni_connection` is
linked only into `test_connection`, so normal `omniroute-native` startup
remains the Task 011 short-lived executable.

Task 019 validation record (2026-09-20): the focused `connection-unit` test
passed 69 checks with zero failures, and the updated network-boundary gate
passed. The complete compiler/sanitizer matrix and regression sweep are
recorded after the final validation run below.

Final Task 019 validation detail: GCC Debug, GCC Release, Clang Debug, Clang
Release, GCC ASan+UBSan Debug, and Clang ASan+UBSan Debug each built
warning-clean and passed all 17 CTest cases. The focused connection suite was
also repeated five times at 69 checks with zero failures. `npm run
check:docs-all` passed documentation sync, frontmatter, environment sync,
internal links, and fabricated-doc checks; it retained only the repository's
pre-existing soft count/version/date drift warnings. The GCC Release
`omniroute-native` remained 16,640 bytes (`size` dec 4,767), with RSS/HWM
samples of 1,748/1,748, 1,748/1,748, and 1,752/1,752 kB; connection symbols
and native socket-layer symbols remained absent because the connection library
is test-only linked.

## Bounded connection registry (Task 020)

Small membership bookkeeping for a future event loop
(`include/omniroute/registry.h`, `src/registry.c`, unit-tested by
`tests/test_registry.c` — 120 checks via CTest `registry-unit`, no sockets,
no poller waits, no payload bytes). The registry stores borrowed connection
references only and never dereferences them.

- **Storage**: caller-provided fixed slot array plus the caller-owned registry
  object. Capacity is fixed at init, never grows, and has no owned-backing
  form. Zero capacity, NULL slots, oversized capacity beyond the 32-bit handle
  position space, and re-init of a live registry are rejected; all other
  failures leave the registry inert. Steady-state add/remove/find/iterate and
  destroy never allocate.
- **Ownership**: the registry owns slots and handle metadata only. Connections
  stay externally owned from add until after remove or registry destroy.
  Remove forgets one entry without touching its connection; destroy forgets
  every entry without touching any connection and without releasing the caller
  slot array. The caller keeps connections and slots alive until after destroy.
- **Handles**: opaque slot-plus-generation pairs. A handle is valid only while
  the registry is live and its entry is still present with the same generation.
  Removal preserves the stored generation so the next occupant of the same
  position carries a different generation; the old handle then fails lookup
  and fails a second remove. Handles never survive destroy and must be dropped
  by the caller. Raw positions are never exposed as identities; iteration
  publishes handles alongside pointers.
- **Add/remove**: deterministic first-free-position placement; duplicate pointer
  tracking is rejected; beyond capacity addition reports FULL with state
  unchanged. Removal reports NOT_FOUND for unknown positions, generation
  mismatches, the invalid sentinel, and stale handles, with state unchanged.
- **Lookup**: resolves a handle to its borrowed connection or NULL for any
  stale, unknown, or non-live case. Never mutates the registry.
- **Iteration**: live entries visit in increasing slot order with no extra
  storage via a caller cursor. Never mutates the registry, so entries removed
  mid-scan are skipped safely. No callbacks exist at this layer.
- **Memory cost**: registry object holds one borrowed array reference plus two
  counts and a liveness flag (32 bytes on 64-bit). Each slot holds one borrowed
  connection reference plus one generation counter plus one occupancy flag (16
  bytes on 64-bit: 8 pointer + 4 generation + 1 flag + 3 pad). No chains, no
  spare lists, no hidden blocks.
- **Not this task**: no event loop, no polling, no accept loop, no HTTP, no
  routing, no timers, no threads, no descriptor or poller interaction.
- **Boundary gate**: `check_network_boundary.sh` keeps socket, readiness,
  accept, receive, send, and lifecycle tokens in their dedicated modules;
  `registry.c` carries none of them and joins the zero-heap negative control
  with accepted, receive, send, and connection layers.

`main` still links only `src/main.c` and `src/meminfo.c`; `omni_registry` is
linked only into `test_registry`, so normal `omniroute-native` startup
remains the Task 011 short-lived executable.

Task 020 validation record (2026-09-20): the focused `registry-unit` test
passed 120 checks with zero failures, and the updated network-boundary gate
passed. The complete compiler/sanitizer matrix and regression sweep are
recorded after the final validation run below.

Final Task 020 validation detail: GCC Debug, GCC Release, Clang Debug, Clang
Release, GCC ASan+UBSan Debug, and Clang ASan+UBSan Debug each built
warning-clean and passed all 18 CTest cases. The focused registry suite was
also repeated five times at 120 checks with zero failures. `npm run
check:docs-all` passed documentation sync, frontmatter, environment sync,
internal links, and fabricated-doc checks; it retained only the repository's
pre-existing soft count/version/date drift warnings. The GCC Release
`omniroute-native` remained 16,640 bytes (`size` dec 4,767), with RSS/HWM
samples of 1,748/1,748, 1,748/1,748, and 1,748/1,748 kB; registry, connection,
and native socket-layer symbols remained absent because the registry library
is test-only linked.

## Bounded reactor foundation (Task 021)

The bounded reactor foundation connects the existing poller to explicit
application callbacks (`include/omniroute/reactor.h`,
`src/reactor.c`, and `tests/test_reactor.c`):

```
poller_wait() -> reactor_step() -> callback(token, events, context)
```

The public surface is `omni_reactor_init`, `omni_reactor_destroy`,
`omni_reactor_add`, `omni_reactor_remove`, and `omni_reactor_step`, with
`omni_reactor_make_inert` plus count/capacity views for the same explicit
lifecycle convention as the earlier native primitives. Initialization takes
a live poller and caller-provided fixed arrays of
`struct omni_reactor_registration` and `struct omni_poller_event`. The
reactor never allocates, grows, queues, or allocates per event.

**Responsibility and ownership**: the reactor waits and dispatches readiness
only. It never reads or writes payload bytes, accepts, closes, destroys a
connection, changes connection lifecycle, or processes HTTP/JSON/routing/
authentication/provider/TLS protocols. Descriptors, connection objects,
registry entries, poller backing, registration arrays, event arrays, callback
functions, and callback contexts remain externally owned. The reactor does
own the logical registration set: removal and destruction unregister borrowed
descriptors from the poller and clear callback/context fields, but never close
those descriptors or touch a connection/registry entry. The poller itself is
borrowed and is destroyed separately by its caller.

**Callback lifetime**: `omni_reactor_callback` receives the registered opaque
`uint64_t` token, the poller readiness mask (`READ`, `WRITE`, `ERROR`,
`HANGUP`, or `INVALID`), and the exact caller context pointer. The caller must
keep the callback and context valid while the registration exists. Once
remove/destroy returns, the reactor has cleared those fields and no longer
retains either pointer. Callback execution is synchronous and single-owner;
the reactor does not retain a callback result or enqueue follow-up work.

**Bounded execution**: one `omni_reactor_step(timeout_ms)` validates the
timeout, performs at most one `omni_poller_wait`, and dispatches the complete
ready set represented by the fixed event array before returning. `timeout=0`
probes, a positive timeout bounds one wait, an empty reactor returns without
waiting, and negative or over-`INT_MAX` values are rejected. An interrupted
wait returns `OMNI_REACTOR_ERR_INTERRUPTED` with zero callbacks and unchanged
poller registrations. The step never starts an implicit loop; a caller that
wants another step calls it explicitly. If a callback removes a later source,
that source's event is skipped; if it destroys the reactor, dispatch stops.

**Memory usage**: reactor steady state has no heap allocation. For capacity
`N`, the caller reserves
`sizeof(struct omni_reactor) + N * (sizeof(struct
omni_reactor_registration) + sizeof(struct omni_poller_event))` bytes, in
addition to the poller's own fixed backing. The registration array is dense,
and the poller event array is reused for each step. There are no per-event
allocations, unbounded queues, hidden allocators, or dynamic growth paths.

**Integration boundary**: tokens can carry registry identities, but this
foundation stores no registry pointer and performs no registry or connection
lifecycle operation. `main` still links only `src/main.c` and `src/meminfo.c`;
`omni_reactor` is linked only into `test_reactor`, so normal
`omniroute-native` startup remains the Task 011 short-lived executable.

Task 021 validation record (2026-09-20): `reactor-unit` covers lifecycle,
invalid inputs, add/remove and duplicate/full handling, empty/zero/positive
timeouts, read/write/error dispatch, context propagation, interrupted waits,
repeated add/remove, repeated steps, and destroy-without-close. The focused
suite reports 83 checks with zero failures. The complete compiler,
sanitizer, regression, and documentation results are recorded after the
final validation run below.

Final Task 021 validation detail: GCC Debug, GCC Release, Clang Debug, Clang
Release, GCC ASan+UBSan Debug, and Clang ASan+UBSan Debug all built
warning-clean. Each configuration passed all 19 CTest cases: the CLI,
network-boundary, and Tasks 012-020 regression suites plus `reactor-unit`.
The focused reactor executable was repeated five times at 83 checks with zero
failures. `npm run check:docs-all` passed all documentation gates, retaining
only the repository's pre-existing non-blocking count/version/date drift
warnings.

## Connection/reactor lifecycle adapter (Task 022)

The connection-reactor adapter is the first runtime bridge across the existing
connection, registry, and reactor primitives (`include/omniroute/
connection_reactor.h`, `src/connection_reactor.c`, and
`tests/test_connection_reactor.c`):

```text
listener / accepted owner -> omni_connection -> registry membership
                                      |
                                      v
                           omni_connection_reactor
                                      |
                                      v
                               omni_reactor
                                      |
                                      v
                         connection callback(connection, token,
                                             events, context)
```

The public adapter surface is `omni_connection_reactor_init`,
`omni_connection_reactor_attach`, `omni_connection_reactor_detach`, and
`omni_connection_reactor_dispatch`, with explicit
`omni_connection_reactor_make_inert` / `omni_connection_reactor_destroy`
lifecycle helpers. Initialization borrows one live reactor and one live
caller-owned registry plus one callback/context pair. Attach accepts only an
`OPEN` connection, adds its descriptor and interest mask to the reactor, and
returns the adapter token. Detach removes the reactor registration and the
registry membership while leaving the connection untouched. Adapter destroy
retires remaining memberships and registrations but never closes or destroys
a connection.

**Token lifetime and stale events**: the adapter encodes the registry handle
as `(uint64_t)generation << 32 | index`. The registry generation changes when
a slot is reused, so an event captured before detach cannot resolve to a later
occupant. Detach invalidates the registry identity before returning; if an
already-live reactor reports a removal failure, the identity is still retired
so a later callback is ignored. Tokens do not survive registry or adapter
destruction, and the registry's bounded 32-bit generation wrap is the only
eventual reuse rule. Callers must detach before destroying or reusing a
connection object and must keep the registry, reactor, callback, and context
alive until adapter destruction.

**Callback and ownership boundary**: the generic reactor callback is a small
bridge that resolves the token through the registry and synchronously invokes
the adapter callback only for an attached `OPEN` connection. Readable,
writable, error, hangup, and invalid readiness bits are forwarded unchanged.
The callback does not read or write bytes, close descriptors, transition the
connection, or destroy objects. The adapter owns only the reactor registration
state; the connection owns its accepted descriptor, buffers, and lifecycle;
the registry owns borrowed connection lookup; and the reactor/poller own only
readiness notification/bookkeeping. The adapter does not create a global,
queue, worker, timer, or singleton.

**Bounded memory**: there is no adapter heap allocation and no adapter-local
per-connection allocation. For each attached connection, the existing fixed
registry slot, fixed reactor registration, poller slot/token, and reused event
record provide all steady-state storage. Attach failure rolls the registry
membership back when reactor capacity or registration validation fails. The
production executable remains unchanged and test-only linked libraries keep
the Task 011 startup path socket-free.

Task 022 validation covers valid and duplicate attach, valid and missing
detach, readable/writable/error forwarding, callback context propagation,
stale-token rejection, removal of a later event during dispatch, closed/
invalid connection rejection, reactor-capacity rollback, reactor destruction
without connection destruction, and 1,000 repeated attach/detach cycles.

Task 022 validation record (2026-09-20): the focused
`connection-reactor-unit` suite reports 69 checks with zero failures. The
network-boundary gate passes, and the GCC Release startup binary remains
4,767 bytes in `size` output with no connection, registry, reactor, poller,
or accepted symbols linked into `omniroute-native`; one measured
`--meminfo` run reports RSS/HWM 1,748/1,748 kB. These are the same
test-only-linking and one-shot-process qualifications as the earlier native
baseline, not a server-runtime memory claim.

Final Task 022 validation detail: GCC Debug, GCC Release, Clang Debug, Clang
Release, GCC ASan+UBSan Debug, and Clang ASan+UBSan Debug all built
warning-clean. Each configuration passed all 20 CTest cases: the CLI,
network-boundary, Tasks 012-021 regression suites, and `connection-reactor-unit`.
The focused adapter executable was repeated in GCC and Clang Debug at 69
checks with zero failures. `npm run check:docs-all` passed all documentation
gates, retaining only the repository's pre-existing non-blocking count,
version, and date drift warnings.

## Native runtime coordinator foundation (Task 023)

The runtime coordinator adds lifecycle orchestration over the existing native
listener, connection registry, poller, and reactor primitives
(`include/omniroute/runtime.h`, `src/runtime.c`, and
`tests/test_runtime.c`):

```text
omni_runtime
    |-- listener
    |-- connection registry
    |-- poller support for the reactor
    `-- reactor
```

The public surface is `omni_runtime_make_inert`, `omni_runtime_init`,
`omni_runtime_start`, `omni_runtime_stop`, `omni_runtime_destroy`, and
`omni_runtime_state`. Initialization accepts one caller-owned listener,
registry, and reactor object plus fixed backing arrays for the registry,
poller, and reactor. The runtime embeds only the poller support object that
the existing reactor requires; its poller descriptor/token arrays remain
caller-owned. Capacities are explicit and independent, and no coordinator
operation allocates, grows, queues, or retains dynamic storage.

**Lifecycle**: the explicit states are `INERT`, `INITIALIZED`, `RUNNING`,
`STOPPING`, and `STOPPED`. `init` is valid from `INERT` or `STOPPED`,
`start` is valid only from `INITIALIZED`, and `stop` is valid only from
`RUNNING`. `destroy` is idempotent and also cleans an initialized-but-not-
started runtime. Failed initialization cleans every earlier subsystem and
returns the runtime to `INERT`.

**Startup order** is deterministic: the caller has already reserved the
fixed arrays, then the coordinator initializes the registry, initializes its
borrowed-storage poller support, initializes the reactor over that poller,
initializes the loopback listener, and marks the runtime `INITIALIZED`.
`start` only changes the lifecycle state to `RUNNING`; it does not wait,
accept, dispatch, or run a loop.

**Shutdown order** is the reverse: entering `STOPPING` is the explicit
stop-accepting boundary because no accept loop exists yet; reactor
registrations are destroyed first, registry metadata is forgotten next, the
borrowed-storage poller support is retired, and the listener is destroyed
last before the runtime becomes `STOPPED`. These calls reuse the existing
primitive logic rather than duplicating registration, membership, or
descriptor handling.

**Ownership**: the runtime owns only lifecycle ordering and initialization
flags. It borrows the caller-owned runtime context, listener, registry,
reactor, and all backing arrays. A listener initialized through the runtime
still follows the listener primitive's descriptor ownership and is closed by
that listener teardown. The runtime rejects already-live subsystem objects,
never closes random or external connection descriptors, never destroys
connections, and never frees caller memory. Application protocol state,
connection payloads, HTTP state, authentication, providers, TLS, and
database state are outside this layer.

**Memory**: on the current 64-bit Linux ABI, `sizeof(struct omni_runtime)`
is 72 bytes and the transient configuration view is 104 bytes. For registry
capacity `R`, poller capacity `P`, and reactor capacity `N`, the caller
reserves the existing fixed arrays: `R * sizeof(struct
omni_connection_registry_slot)` plus `P * (sizeof(struct pollfd) +
sizeof(uint64_t))` plus `N * (sizeof(struct
omni_reactor_registration) + sizeof(struct omni_poller_event))`. There is no
runtime heap allocation, dynamic subsystem array, queue, timer, worker, or
per-connection coordinator storage. These values describe the coordinator
context and caller reservations, not a full server RSS budget.

The coordinator is deliberately not wired into `omniroute-native` startup:
`omni_runtime` is linked only into the native test libraries, so the existing
production binary remains the short-lived, socket-free Task 011 skeleton.
Task 024 consumes a running coordinator from a separate event-loop object; it
does not change the production startup path or add protocol handling.

Task 023 validation covers inert construction, successful initialization and
state transitions, invalid reinitialization and stop/start transitions,
late listener-failure cleanup, live-listener rejection without closing the
external descriptor, reverse teardown, preservation of an externally owned
open connection, idempotent destruction, and 1,000 repeated bounded
init/start/stop/destroy cycles. The focused runtime suite reports 38 checks
with zero failures.

Final Task 023 validation detail: GCC Debug, GCC Release, Clang Debug, Clang
Release, GCC ASan+UBSan Debug, and Clang ASan+UBSan Debug all built
warning-clean. Each configuration passed all 21 CTest cases: the CLI,
network-boundary gate, Tasks 012-022 regression suites, and `runtime-unit`.
The GCC Release production executable remains 4,767 bytes in `size` output,
and `nm` shows no runtime, listener, registry, reactor, poller, or connection
symbols linked into `omniroute-native`; the coordinator remains test-only.

## Native event loop foundation (Task 024)

Task 024 turns the Task 023 lifecycle coordinator into a bounded synchronous
execution engine without adding an application protocol
(`include/omniroute/event_loop.h`, `src/event_loop.c`, and
`tests/test_event_loop.c`):

```text
omni_runtime (RUNNING)
        |
        v
omni_event_loop_run()
        |
        +-- omni_reactor_step(timeout_ms)
        |       |
        |       +-- synchronous callbacks
        |       `-- caller-owned connections
        `-- repeat until stop, failure, interruption, or empty reactor
```

The public surface is `omni_event_loop_make_inert`, `omni_event_loop_init`,
`omni_event_loop_run`, `omni_event_loop_stop`, `omni_event_loop_destroy`, and
`omni_event_loop_state`, with fixed accounting views for iterations, processed
events, and the last reactor status. Initialization borrows an initialized
runtime and validates a finite step timeout; it does not start the runtime.
`run` requires `omni_runtime_start` to have completed, and runtime teardown is
still performed by the runtime owner after the loop returns.

**Execution model**: the loop is synchronous and single-owner. Each loop
iteration calls exactly one existing `omni_reactor_step`; the reactor retains
responsibility for readiness waits, event storage, callback ordering, and
callback invocation. The event loop does not duplicate listener, registry,
poller, reactor, or connection logic. A callback may call `stop`, which marks
the loop `STOPPING`; the current reactor dispatch completes and the loop exits
without starting another step. Because this foundation has no wakeup
descriptor, thread, or signal integration, another owner cannot interrupt a
blocked wait asynchronously. Callback failures are not a separate result yet:
the existing callback type returns `void`, so reactor failures and interrupted
waits are the only step failures surfaced here.

**Lifecycle and shutdown**: `INERT` and `STOPPED` storage can be initialized;
`INITIALIZED` rejects stop-before-run explicitly; `RUNNING` accepts one stop
request; and repeated stop calls in `STOPPING` or `STOPPED` are safe. A normal
stop, an empty-reactor exit, a reactor failure, or an interrupted wait always
returns from `run` without a shutdown wait. The event loop never calls
`omni_runtime_stop`, destroys a connection, removes a registration, or closes
a descriptor. `destroy` only clears the loop's borrowed runtime reference
after execution has returned.

**Timeouts**: `OMNI_EVENT_LOOP_DEFAULT_TIMEOUT_MS` is 1,000 ms and
`OMNI_EVENT_LOOP_MAX_TIMEOUT_MS` is 60,000 ms. Zero is a nonblocking probe;
negative values and values above the maximum are rejected before execution.
Every positive value bounds one reactor wait. An empty reactor has no kernel
wait source in the existing reactor, so the loop performs one zero-event step
and exits cleanly instead of busy-spinning forever.

**Memory and counters**: the event-loop object is caller-owned and has no
backing arrays; on the current 64-bit Linux ABI it is 48 bytes, while the
configuration view is 16 bytes. Initialization, stepping, stop, and destroy
perform no heap allocation. The loop reuses the reactor's existing fixed
registration and event storage, creates no queues, and retains no events.
`iterations` increments once per attempted reactor step and
`events_processed` adds the step's callback count. Both are fixed `uint64_t`
counters that saturate at `UINT64_MAX` rather than wrapping.

**Protocol boundary**: HTTP, parsing, routing, authentication, providers,
TLS, database access, JSON, SSE, WebSocket, timers, background jobs, and
worker threads remain deferred. The event loop only supplies execution
orchestration for the already-landed runtime/reactor/callback contracts.
`omniroute-native` still links only its Task 011 CLI sources; this foundation
is test-only until a later task explicitly wires a production startup path.

Task 024 validation covers valid and invalid initialization, runtime-state
gating, empty-reactor execution, zero/default/maximum timeout validation,
readiness dispatch, callback-driven stop, repeated stop, interrupted-wait
propagation, descriptor and connection ownership survival, and 1,000 repeated
init/run/stop/destroy cycles. The focused event-loop suite reports 52 checks
with zero failures in the current validation run.

Final Task 024 validation detail: GCC Debug, GCC Release, Clang Debug, Clang
Release, GCC ASan+UBSan Debug, and Clang ASan+UBSan Debug all built
warning-clean. Each configuration passed all 22 CTest cases, covering the
CLI, network-boundary gate, Tasks 012-023 regression suites, and
`event-loop-unit`; the canonical `npm run test:native` Debug run passed the
same 22 cases. `npm run check:docs-all` passed its documentation gates with
only the repository's pre-existing soft count/version/date drift warnings.

## Bounded connection I/O state machine (Task 025)

Task 025 is the first connection I/O lifecycle layer above the transport
primitives (`include/omniroute/connection_io.h`, `src/connection_io.c`,
and `tests/test_connection_io.c`):

```text
event_loop
      |
      v
connection_reactor
      |
      v
connection_io
      |
      +-- receive buffer (omni_bytebuf, borrowed)
      |
      +-- send buffer (omni_bytebuf, borrowed)
      |
      +-- socket read/write (through omni_recv / omni_send)
```

```text
connection
     |
     v
connection_io
     |
     +-- omni_bytebuf receive
     |
     +-- omni_bytebuf send
     |
     +-- socket fd (borrowed from omni_accepted)
```

The public surface is `omni_connection_io_make_inert`,
`omni_connection_io_init`, `omni_connection_io_readable`,
`omni_connection_io_writable`, `omni_connection_io_close`, and
`omni_connection_io_destroy`, with `omni_connection_io_state`,
`omni_connection_io_is_open`, `omni_connection_io_receive_buffer`,
`omni_connection_io_send_buffer`, and `omni_connection_io_fd` views.
`init` borrows one live `omni_accepted` owner plus two caller-provided
fixed backing ranges for the receive and send `omni_bytebuf` objects. No
heap allocation occurs at any connection I/O operation; the caller keeps the
accepted owner and both backing ranges alive until after `destroy`.

**State machine**: the explicit states are `NEW`, `OPEN`, `READABLE`,
`WRITABLE`, `CLOSING`, and `CLOSED`. `make_inert` canonicalizes fresh or
`CLOSED` storage to `NEW`. `init` transitions `NEW -> OPEN` and rejects every
other source state, invalid storage, zero capacity, or a non-live accepted
owner without changing state. `readable` and `writable` are valid only from
`OPEN` and perform the transient `OPEN -> READABLE -> OPEN` and
`OPEN -> WRITABLE -> OPEN` transitions around exactly one delegated
`omni_recv_once` or `omni_send_once` call. `close` moves `OPEN -> CLOSING`
and is idempotent from `CLOSING` and `CLOSED` while being rejected from
`NEW`. `destroy` releases both borrowed byte-buffer objects and reaches
`CLOSED`; it is idempotent and safe on `NEW`, `NULL`, and `CLOSED`. Closed
connections reject every I/O attempt with `ERR_CLOSED`, and `make_inert` is
not a cleanup for `OPEN`, `READABLE`, `WRITABLE`, or `CLOSING` objects.

**Read path**: `omni_connection_io_readable` delegates once to
`omni_recv_once` into the bounded receive tail. It handles successful data
receipt, `EAGAIN`/`EWOULDBLOCK`, `EINTR`, `EOF` (orderly peer shutdown),
buffer-full (zero writable tail, no kernel wait), and fatal errors, mapping
each `omni_recv_status` to the corresponding `omni_connection_io_status`
and preserving `errno` where the primitive does. It never grows the buffer,
never compacts or resets it, never parses bytes, and never performs more
than one kernel wait. A buffer that is full reports `ERR_BUFFER_FULL`
without consuming pending socket bytes.

**Write path**: `omni_connection_io_writable` writes only already-buffered
bytes from the send `omni_bytebuf`. It obtains the readable view and
delegates once to `omni_send_once` with that span, then consumes exactly the
positive `sent` count through `omni_bytebuf_consume` on progress. It handles
full write (positive progress draining the readable region), partial write
(leaving the remaining tail for a later readiness event), `EAGAIN`/
`EWOULDBLOCK`, `EINTR`, and fatal/peer-closed errors, mapping each
`omni_send_status` to the corresponding connection I/O status and preserving
`errno`. It performs no automatic retry loop, no blocking wait, and no extra
probe call when the send buffer is already empty.

**Ownership**: the connection I/O object owns only its receive and send
`omni_bytebuf` objects (borrowed backing) and the I/O state. It borrows the
`omni_accepted` owner and its descriptor and never destroys the accepted
owner, never closes the descriptor, never destroys a registry entry, never
removes a reactor registration, and never frees caller memory. The connection
still owns lifecycle identity and registry membership; the socket follows the
existing accepted/connection ownership rules. The caller must remove any
external poller registration before `destroy`, as with the earlier
connection object.

**Memory**: on the current 64-bit Linux ABI, `struct omni_connection_io`
is 112 bytes (`struct omni_accepted *` plus two `struct omni_bytebuf` at
48 bytes each plus 4-byte state plus 4 bytes padding) and the transient
`struct omni_connection_io_config` is 40 bytes. For receive capacity `R`
and send capacity `S`, the caller reserves exactly `R + S` bytes of backing
plus the connection I/O object itself. There is no per-read or per-write
heap allocation, no dynamic buffer growth, no queue, and no hidden
allocation. Receive and send capacities are independent, fixed at `init`,
and never changed.

**Error model**: every I/O operation returns an explicit
`omni_connection_io_result` with a status, a preserved `sys_errno`, and a
`count`. The statuses cover `OK`, `ERR_INVALID` (NULL, bad storage, bad fd,
zero capacity), `ERR_STATE` (invalid lifecycle transition), `ERR_CLOSED`
(I/O after close), `ERR_BUFFER_FULL`, `ERR_WOULD_BLOCK`, `ERR_INTERRUPTED`,
`ERR_EOF`, and `ERR_IO` (fatal socket error with `errno` preserved). Would-
block and interrupted outcomes are normal control flow, not hidden.

**Protocol boundary**: HTTP, JSON, OpenAI API, routing, authentication,
TLS, compression, WebSocket, SSE, providers, and database remain deferred.
The connection I/O layer owns data movement only and never interprets
protocol bytes. `omniroute-native` still links only its Task 011 CLI
sources; `omni_connection_io` is linked only into `test_connection_io`.

Task 025 validation covers valid and invalid initialization, inert behavior,
readable socket receives with EOF, write with full and partial drain and
buffer-draining verification on the client side, close and repeated-close
transitions, I/O-after-close rejection, descriptor and registry
non-destruction, fixed-capacity memory checks, and 10 repeated
read/write/close/destroy stress cycles. The focused connection I/O suite
reports 157 checks with zero failures in the current validation run.

Final Task 025 validation detail: GCC Debug, GCC Release, Clang Debug, Clang
Release, GCC ASan+UBSan Debug, and Clang ASan+UBSan Debug all built
warning-clean. Each configuration passed all 23 CTest cases, covering the
CLI, network-boundary gate, Tasks 012-024 regression suites, and
`connection-io-unit`; the canonical `npm run test:native` Debug run passed
the same 23 cases. `npm run check:docs-all` passed its documentation gates
with only the repository's pre-existing soft count/version/date drift
warnings.

## Bounded connection/session integration (Task 026)

Task 026 connects the existing connection ownership model with the bounded
connection I/O state machine without introducing protocol behavior
(`include/omniroute/connection_session.h`, `src/connection_session.c`,
and `tests/test_connection_session.c`):

```text
listener -> accepted -> connection (owns FD + receive buffer)
                              |
                              v
                    connection_session (borrows connection,
                              |         owns connection_io)
                              v
                    connection_io (owns receive/send bytebufs,
                              |     borrows accepted FD)
                              +-- receive buffer (borrowed backing)
                              +-- send buffer (borrowed backing)
                              +-- socket read/write via recv/send primitives
```

The public surface is `omni_connection_session_make_inert`,
`omni_connection_session_init`, `omni_connection_session_open`,
`omni_connection_session_close`, and
`omni_connection_session_destroy`, with
`omni_connection_session_state`, `omni_connection_session_is_open`,
`omni_connection_session_is_closed`,
`omni_connection_session_connection`,
`omni_connection_session_io`, `omni_connection_session_io_state`,
`omni_connection_session_receive_buffer`,
`omni_connection_session_send_buffer`,
`omni_connection_session_fd`,
`omni_connection_session_readable`, and
`omni_connection_session_writable`. `init` binds a live `OPEN`
`omni_connection` and records two caller-provided fixed backing ranges for the
future `connection_io` receive and send buffers; `open` then initializes the
owned `connection_io` from those borrowed references. No heap allocation
occurs at any session operation; the caller keeps the connection and both
backing ranges alive until after `destroy`.

**State machine**: the explicit states are `NEW`, `INIT`, `OPEN`, `CLOSING`,
and `CLOSED`. `make_inert` canonicalizes fresh or `CLOSED` storage to `NEW`.
`init` transitions `NEW -> INIT` and rejects every other source state, a
non-`OPEN` connection, a non-live connection, or a NULL/zero-capacity buffer,
leaving the session `NEW` on failure. `open` transitions `INIT -> OPEN` by
calling `omni_connection_io_init` with the stored borrowed connection's
`accepted` owner and the stored backing ranges; failure leaves the session in
`INIT` with the `io` still `NEW` so the caller may retry or destroy. `close`
moves `OPEN -> CLOSING` via `omni_connection_io_close` and is idempotent from
`CLOSING` and `CLOSED` while being rejected from `NEW`/`INIT`. `destroy`
releases the owned `connection_io` (both bytebufs) and reaches `CLOSED`; it
is idempotent and safe on `NULL`, `NEW`, `INIT`, `OPEN`, `CLOSING`, and
`CLOSED`. Closed sessions reject `readable`/`writable` with `ERR_CLOSED`, and
`make_inert` is not a cleanup for `INIT`, `OPEN`, or `CLOSING` objects.

**Integration**: the session stores only a borrowed `omni_connection *`,
the owned `omni_connection_io`, and the four fixed buffer references plus
state. It never closes a descriptor, never destroys an `omni_accepted` owner,
never removes a registry entry, never manages a poller registration, and never
runs an event loop. It initializes `connection_io` correctly with
`connection->accepted` as the borrowed `accepted` owner, preserves connection
ownership boundaries (the connection still owns the FD until its own destroy),
exposes `connection_io` state and buffers through session views, and allows
the caller to perform exactly one bounded `readable`/`writable` operation
through the session boundary when `OPEN`. It does not read automatically,
write automatically, retry, or loop.

**Memory**: on the current 64-bit Linux ABI, `struct
omni_connection_session` is 160 bytes (`struct omni_connection *` 8 +
`struct omni_connection_io` 112 + `void *`+`size_t` receive 16 + `void *`+
`size_t` send 16 + 4-byte state + 4 bytes padding) and the transient
`struct omni_connection_session_config` is 40 bytes. The caller reserves the
connection object plus its own receive buffer plus `R+S` backing bytes for the
session I/O plus the session object itself. There is no per-read or
per-write heap allocation, no dynamic container, no queue, and no hidden
allocation. Receive and send capacities are independent, fixed at `init` for
use at `open`, and never changed after `open`.

**Error model**: `init`/`open`/`close` return an explicit
`omni_connection_session_result` with `OK`, `ERR_INVALID` (NULL, bad
connection, bad storage, zero capacity, non-OPEN connection), `ERR_STATE`
(invalid lifecycle transition), and `ERR_CLOSED` (operation on closing/closed
session). `readable`/`writable` return the underlying
`omni_connection_io_result` so `WOULD_BLOCK`, `INTERRUPTED`, `BUFFER_FULL`,
`EOF`, and `ERR_IO` remain explicit with preserved `errno`.

**Protocol boundary**: HTTP, JSON, OpenAI API, routing, authentication, TLS,
providers, SSE, WebSocket, MCP, and database remain deferred. The session
only coordinates lifecycle and delegates data movement to the existing
`connection_io`/`recv`/`send` primitives without interpreting protocol bytes.
`omniroute-native` still links only its Task 011 CLI sources;
`omni_connection_session` is linked only into `test_connection_session`.

Task 026 validation covers inert object, valid init/open/close, invalid
init (NULL, non-OPEN connection, zero/NULL buffers, duplicate init),
invalid transitions (open from NEW, close from INIT, init from OPEN,
readable/writable from NEW/INIT/CLOSING/CLOSED), connection and descriptor
preservation after close/destroy, registry non-destruction, connection I/O
initialization and distinct-buffer checks, readable/writable delegation with
client-visible payload and EOF handling, bounded memory size check, 20 repeated
lifecycle cycles, and bystander-FD survival. The focused session suite reports
327 checks with zero failures in the current validation run.

Final Task 026 validation detail: GCC Debug, GCC Release, Clang Debug, Clang
Release, GCC ASan+UBSan Debug, and Clang ASan+UBSan Debug all built
warning-clean. Each configuration passed all 24 CTest cases, covering the
CLI, network-boundary gate, Tasks 012-025 regression suites, and
`connection-session-unit`; the canonical `npm run test:native` Debug run
passed the same 24 cases. `npm run check:docs-all` passed its documentation
gates with only the repository's pre-existing soft count/version/date drift
warnings.

## Bounded connection runtime binding (Task 027)

Task 027 is the bounded runtime binding layer that coordinates the event
loop, connection reactor, and connection session without introducing protocol
behavior (`include/omniroute/connection_runtime.h`,
`src/connection_runtime.c`, and `tests/test_connection_runtime.c`):

```text
connection
    |
connection_session (create/open on attach)
    |
connection_reactor registration (token)
    |
event_loop dispatch (reactor_step -> dispatch)
```

Expected integration flow:

```text
event_loop
    |
connection_reactor (registry + reactor)
    |
connection_runtime (bounded entries)
    |
connection_session -> connection_io -> recv/send -> bytebuf
    |
connection (accepted FD)
```

The public surface is `omni_connection_runtime_make_inert`,
`omni_connection_runtime_init`, `omni_connection_runtime_attach`,
`omni_connection_runtime_detach`, and
`omni_connection_runtime_destroy`, with
`omni_connection_runtime_state`,
`omni_connection_runtime_count`/`capacity`/`is_initialized`,
`omni_connection_runtime_find_session`, and
`omni_connection_runtime_fd`. `init` binds a live
`omni_connection_registry`, live `omni_connection_reactor` adapter, optional
borrowed `omni_event_loop`, and a caller-provided fixed array of
`omni_connection_runtime_entry` objects. `attach` validates an `OPEN`
`omni_connection` and caller-provided recv/send backing, finds a free entry,
creates/opens the owned `omni_connection_session` (which owns its
`omni_connection_io` and two borrowed bytebufs), then registers the
connection's borrowed descriptor through the existing adapter and stores the
resulting token. No heap allocation occurs.

**State machine**: the runtime object has explicit states `NEW`,
`INITIALIZED`, `ATTACHED`, `DETACHING`, and `CLOSED`. `make_inert`
canonicalizes fresh or `CLOSED` storage to `NEW`. `init` requires `NEW` and
transitions to `INITIALIZED`; invalid input leaves it `NEW`. `attach` is
valid from `INITIALIZED` or `ATTACHED`, transitions to `ATTACHED` and
increments `count`; duplicate, full, or invalid input leaves the runtime
consistent with `count` unchanged and no partial entry. `detach` is valid
from `INITIALIZED` or `ATTACHED`, unregisters via the adapter, destroys the
owned session, clears the entry, decrements `count`, and returns to
`INITIALIZED` when the last entry is removed (otherwise stays `ATTACHED`);
`DETACHING` is the transient synchronous detach marker, not a persistent
state. Missing target returns `ERR_NOT_FOUND` without corrupting state.
`destroy` detaches all occupied entries, releases session bookkeeping, and
reaches `CLOSED`; it is idempotent and safe on `NULL`, `NEW`,
`INITIALIZED`, `ATTACHED`, and `CLOSED`.

**Integration**: the runtime stores only borrowed `registry`, `adapter`,
`event_loop` pointers plus the borrowed fixed entry array and `count`/`capacity`
plus state. It creates/opens the session on `attach` and closes/destroys it
on `detach`/`destroy`, then delegates registration to the existing
`omni_connection_reactor_attach`/`detach` helpers which preserve token
ownership rules (generation << 32 | index). Stale tokens captured before
`detach` dispatch as `IGNORED` via the adapter. The runtime never reads or
writes payload bytes, never parses, never processes requests, never closes a
descriptor, never destroys an `omni_accepted` owner, never frees a
connection, never owns a poller, and never runs the event loop.

**Memory**: on the current 64-bit Linux ABI, `struct
omni_connection_runtime` is 56 bytes (`registry*8 + adapter*8 +
event_loop*8 + entries*8 + capacity8 + count8 + state4 + live1 + 7 pad) and
`struct omni_connection_runtime_entry` is 184 bytes (`connection*8 +
session160 + token8 + occupied1 + 7 pad`). `struct
omni_connection_session`remains 160 bytes and its config 40 bytes, while`struct omni_connection_runtime_config`and`omni_connection_runtime_attach_config`are each 40 bytes transient. For
capacity`N`with per-session`R+S`backing, the caller reserves`N *
184 + N*(R+S)` bytes for the runtime entries plus the registry/poller/reactor
backing already accounted for, plus the 56-byte runtime object itself. There
is no per-attach heap allocation, no dynamic map, no queue.

**Protocol boundary**: HTTP, JSON, OpenAI API, authentication, routing, TLS,
database, and parser logic remain deferred. The binding layer only coordinates
lifecycle and token registration.

Task 027 validation covers inert state, valid/invalid initialization
(NULL registry/adapter, zero capacity, double init), attach success
(token, count, state, session FD, reactor/registry counts, duplicate
rejection), detach success (count, state, session gone, reactor/registry
cleanup, connection live preserved, NOT_FOUND on second detach), invalid
transitions (attach from NEW, NULL connection, zero caps, over capacity
FULL), stale registration handling (old token IGNORED, new token differs),
descriptor and connection ownership preservation after attach/detach/destroy,
session lifecycle integration (session OPEN, io OPEN, buffers distinct,
readable/writable delegation), reactor registration cleanup on destroy,
20 repeated attach/detach cycles, bystander-FD census, and bounded-size
checks. The focused runtime suite reports 259 checks with zero failures.

Final Task 027 validation detail: GCC Debug, GCC Release, Clang Debug, Clang
Release, GCC ASan+UBSan Debug, and Clang ASan+UBSan Debug all built
warning-clean. Each configuration passed all 25 CTest cases, covering the
CLI, network-boundary gate, Tasks 012-026 regression suites, and
`connection-runtime-unit`; the canonical `npm run test:native` Debug run
passed the same 25 cases. `npm run check:docs-all` passed its documentation
gates with only the repository's pre-existing soft count/version/date drift
warnings.

## Bounded connection manager (Task 028)

Task 028 adds the fixed-capacity membership layer above the Task 027 runtime
binding (`include/omniroute/connection_manager.h`,
`src/connection_manager.c`, and `tests/test_connection_manager.c`):

```text
OPEN connection
    |
connection_runtime (session + registry/reactor registration)
    |
connection_manager (fixed membership entries + count)
```

The public lifecycle and membership API is
`omni_connection_manager_make_inert`, `omni_connection_manager_init`,
`omni_connection_manager_start`, `omni_connection_manager_stop`,
`omni_connection_manager_add`, `omni_connection_manager_remove`,
`omni_connection_manager_find`, `omni_connection_manager_destroy`,
`omni_connection_manager_state`, `omni_connection_manager_count`, and
`omni_connection_manager_capacity`. Find returns a read-only view of a live
manager entry; callers that need FD or session views use the borrowed runtime.
Initialization borrows a live `omni_connection_runtime` and a caller-provided array of
`omni_connection_manager_entry` objects with fixed nonzero capacity. There is
no growth, allocator, map, queue, worker, or production startup wiring.

**State machine**: `NEW -> INITIALIZED -> RUNNING -> STOPPING -> CLOSED`.
`make_inert` establishes `NEW`; init requires `NEW`. A successful add attaches
through the runtime first, then publishes the manager entry and count; the
first successful add promotes `INITIALIZED` to `RUNNING`. Failed add leaves
manager membership and state unchanged, including runtime-session rollback
when adapter registration fails. Removing one member detaches through the
runtime before clearing its entry. Removing the last member returns `RUNNING`
to `INITIALIZED`; after `STOPPING`, remove remains available while add is
rejected. Destroy detaches every member and reaches `CLOSED` idempotently.

**Ownership and identity**: the manager owns only bookkeeping in its fixed
entry array and its membership count. It borrows the runtime, caller backing
array, connections, and session receive/send backing. The runtime continues to
own session and I/O lifecycle; registry/reactor registration is delegated via
the runtime. The manager never closes a descriptor, destroys an accepted
owner, frees a connection, destroys the registry/reactor/event loop, runs the
loop, reads or writes payload bytes, or parses protocols. Each manager entry
stores the connection pointer, the existing registry/reactor token, and an
occupied flag. Slot reuse receives the registry's next generation token, so a
stale token cannot resolve to the new connection.

**Measured manager memory**: on the current 64-bit Linux ABI,
`sizeof(struct omni_connection_manager)` is 40 bytes,
`sizeof(struct omni_connection_manager_entry)` is 24 bytes, and
`sizeof(struct omni_connection_manager_config)` is 24 bytes, measured by the
focused native test. For manager capacity `N`, the manager object and its
caller-provided membership array reserve exactly
`40 + N * 24` bytes on this ABI. The runtime entries, sessions, and caller
receive/send buffers are separate borrowed/runtime storage and are not added
to this manager-only formula. No per-add allocation occurs.

Task 028 validation covers lifecycle and invalid operations, full capacity,
duplicate and missing membership, rollback after runtime session setup reaches
a duplicate adapter registration, generation-safe slot reuse, one- and
multiple-connection destruction, borrowed descriptor/registry/reactor/event-loop
preservation, 1,000 deterministic add/remove operations with a stable Linux FD
census, and measured-size/formula checks. The focused manager suite reports
420 checks with zero failures. GCC Debug, GCC Release, Clang Debug, Clang
Release, GCC ASan+UBSan Debug, and Clang ASan+UBSan Debug all built
warning-clean and passed all 26 CTest cases, including the CLI,
network-boundary gate, Tasks 012-027 regressions, and
`connection-manager-unit`. `npm run check:docs-all` passed; it reported
the repository's existing soft count/version/date drift warnings, with no
documentation check failures.

## Bounded connection admission (Task 029)

Task 029 adds the test-only bridge from an already-live listener into the
existing bounded manager (`include/omniroute/connection_admission.h`,
`src/connection_admission.c`, and `tests/test_connection_admission.c`):

```text
borrowed listener
    |
omni_accept_once (at most one per call)
    |
temporary omni_accepted owner
    |
omni_connection_init + omni_connection_from_accepted
    |
omni_connection_manager_add (session/runtime/reactor attach)
    |
published fixed admission slot + generation identity
```

The public surface is `omni_connection_admission_make_inert`,
`omni_connection_admission_init`, `omni_connection_admission_once`,
`omni_connection_admission_drain`, `omni_connection_admission_release`,
`omni_connection_admission_stop`, `omni_connection_admission_destroy`, and
the state/count/capacity queries. Every connection slot and receive/send byte
range is caller-provided. Initialization requires a nonzero admission
capacity no greater than the manager capacity, enough slot bytes, and three
non-overlapping fixed pools: connection receive, session receive, and session
send. Pool sizes and all capacity multiplications are checked before use.
There is no per-connection allocation or growth.

`once` checks lifecycle and both capacities before accepting, then prepares a
connection, transfers the accepted owner, and attaches through the manager.
The slot and admission count become visible only after manager attach
succeeds. A preparation failure destroys the temporary accepted owner. An
adoption failure destroys whichever owner still holds the FD. A manager/runtime
attach failure leaves manager membership unpublished and destroys the
connection owner, which releases the accepted FD through its existing
connection API. Earlier successful admissions from a later-stopping drain
remain live.

`drain` requires a finite nonzero attempt limit and caller-owned identity
output with at least that many elements. Each attempt calls the existing
one-shot accept operation at most once. It returns the exact successful
admission count and distinguishes queue drained, budget reached, slot full,
manager/runtime full, interruption, transient handshake failure, accept
failure, connection failure, and manager failure. It stops before accepting
when either manager or admission storage is full.

The lifecycle is `NEW -> INITIALIZED -> ACTIVE -> STOPPING -> CLOSED`.
Successful first admission enters `ACTIVE`; `stop` forbids new admissions
but continues to allow release. Release checks both slot index and the
manager's existing generation token, removes manager/runtime/reactor/session
membership first, then destroys the connection and makes the slot reusable.
Destroy applies the same ordering to every occupied slot. If a lower-layer
detach reports an error, admission retains the connection slot and backing
references rather than destroying storage that may still be referenced.

The admission layer owns only slot occupancy and lifecycle coordination. It
borrows the listener, manager/runtime/registry/reactor/event loop, slot array,
and all three byte pools. `accept` gives the temporary `omni_accepted` sole FD
ownership; `omni_connection_from_accepted` moves that ownership to
`omni_connection`; the manager/session/reactor only borrow it. Admission
release/destroy never calls `close` directly and never destroys the listener,
manager dependencies, or caller-side client descriptors. No readiness loop is
started, and no payload is read or written.

**Measured admission memory**: on the current 64-bit Linux ABI, the focused
test measured `sizeof(struct omni_connection_admission)` as 96 bytes,
`sizeof(struct omni_connection_admission_slot)` as 88 bytes,
`sizeof(struct omni_connection_admission_config)` as 112 bytes, and
`sizeof(struct omni_connection_admission_result)` as 56 bytes. For capacity
`N`, connection receive slice `C`, session receive slice `R`, and session send
slice `S`, caller-reserved admission storage is `96 + N * (88 + C + R + S)`
bytes on this ABI. The 88-byte slot already contains its `omni_connection`;
the three byte pools are separate borrowed storage. Config/result temporaries
and the per-drain caller identity output (`max_attempts` identities) are
separate transient storage. External manager/runtime/registry/reactor/poller
arrays are not part of that reservation formula.

Task 029 validation covers invalid/overflowing configuration, empty and
successful one-shot admission, bounded multi-client drain, manager and slot
capacity, preparation and runtime-attach rollback, stale identity and reactor
token rejection after slot reuse, STOPPING release, empty/single/multiple
destruction, bystander descriptor and dependency survival, and 1,000
deterministic admission/release cycles with a Linux FD census on each cycle.
The focused suite reports 1,141 checks with zero failures and prints its
measured structure sizes. The manager and admission libraries remain absent
from the production executable; the native CLI does not start a listener.
GCC 16.2.1 and Clang 22.1.8 each passed Debug, Release, and Debug
ASan+UBSan builds, with all 27 native CTest cases green in every
configuration. The network-boundary gate passed. `npm run check:docs-all`
passed with 91 existing soft documentation-drift warnings and no broken-link
or fabricated-reference findings. The production CLI startup and version
output remain unchanged, and `nm` found no admission/manager/runtime/network
symbols in `omniroute-native`.

## Bounded listener admission bridge (Task 030)

Task 030 connects the existing listener, Task 021 reactor, Task 024 event
loop, and Task 029 admission pipeline through a test-only bridge
(`include/omniroute/listener_admission.h`, `src/listener_admission.c`, and
`tests/test_listener_admission.c`):

```text
event_loop_run()
    -> omni_reactor_step()
    -> listener READ callback
    -> omni_connection_admission_drain(finite budget, caller identities)
    -> connection_manager
    -> live managed connections
```

The public surface is `omni_listener_admission_make_inert`,
`omni_listener_admission_init`, `omni_listener_admission_start`,
`omni_listener_admission_stop`, `omni_listener_admission_destroy`, and the
state, last-result, dispatch-count, and total-admitted queries. Initialization
borrows one live listener, reactor, and Task 029 admission object, plus a
caller-owned identity array. It rejects a zero budget, a mismatched listener,
unavailable dependencies, or identity storage smaller than the configured
`max_admissions_per_dispatch`.

Start registers the borrowed listener FD with `OMNI_POLLER_INTEREST_READ` and
the bridge callback under the reserved
`OMNI_LISTENER_ADMISSION_REACTOR_TOKEN` (`UINT64_MAX`). That token must be
available in the reactor; a token/descriptor/capacity conflict is returned as
a reactor registration failure with the underlying reactor status preserved.
The bridge does not build another poller or event loop. Its lifecycle is
`INERT -> INITIALIZED -> ACTIVE -> STOPPED -> CLOSED`; a failed start remains
`INITIALIZED` without a partial registration. Stop removes the token before
the callback context may be destroyed. If removal fails, the bridge remains
`ACTIVE` and retains its borrowed references so the caller can retry. Stop
does not release any connection already admitted.

The callback runs synchronously within one reactor dispatch. ERROR, HANGUP,
or INVALID readiness is recorded without admission. WRITE-only readiness is
recorded and ignored. READ readiness invokes exactly one Task 029 drain with
the caller's finite attempt budget and identity storage. The bridge does not
retry an interrupted or transient result and preserves the lower admission
result in its last-result record. DRAINED, budget reached, capacity full,
stopped admission, and other admission failures remain distinguishable. A
full admission slot or manager returns from the callback without spinning or
removing the listener registration; later readiness can retry after capacity
is released.

The bridge owns no listener, descriptor, accepted connection, manager,
runtime, reactor, event loop, or result array. It makes no direct network or
heap calls. Callback work is limited to the bounded admission drain and
saturating local accounting; it does not nest a reactor step or event-loop
run. `dispatches` counts bridge callbacks, and `total_admitted` accumulates
successful lower-layer admissions; both saturate at the maximum `uint64_t`
value.

The focused test uses ephemeral loopback ports. Its event-loop proof queues a
client before `omni_event_loop_run()`, observes the registered listener
callback admit it through Task 029, and uses a separate test-only ready pipe
callback to request loop stop after the manager count rises. It does not call
the admission drain directly for that proof. The suite also covers readiness
filtering, registration rollback, bounded batches, slot/manager backpressure,
stop/destroy ownership, and 1,000 bounded admit/release cycles with an FD
census. The focused suite reports 3,093 checks with zero failures.

Final Task 030 validation: GCC and Clang Debug, Release, and Debug
ASan+UBSan builds each pass the full native CTest suite (28/28). The
network/heap boundary gate and `npm run check:docs-all` pass; the docs gate
reports 91 existing soft drift warnings. The production executable builds,
prints its existing version/startup output, and has no listener, reactor,
event-loop, manager, admission, or socket symbols according to `nm`.

**Measured bridge memory**: on the current 64-bit Linux ABI, the focused test
measures `sizeof(struct omni_listener_admission)` as 152 bytes,
`sizeof(struct omni_listener_admission_config)` as 48 bytes, and
`sizeof(struct omni_listener_admission_result)` as 80 bytes. The bridge's
fixed object contains its borrowed references, counters, and last result; it
has no embedded identity array. For budget `B`, the separately caller-owned
identity output requires at least `B * sizeof(struct
omni_connection_admission_identity)` bytes (16 bytes per identity on this
ABI). Config/result temporaries and all underlying listener, reactor,
admission, manager, runtime, and event-loop storage are separate.

`omni_listener_admission` is linked only into `test_listener_admission`.
`omniroute-native` startup remains unchanged: it starts no listener or
production event loop. HTTP, protocol state, production daemon startup, and
Task 031 readiness dispatch and Task 032 lifecycle policy are documented
below; production startup and protocol work remain deferred.

## Bounded managed connection readiness dispatch (Task 031)

Task 031 connects the existing connection-reactor callback to the session and
I/O layer through a test-only bridge
(`include/omniroute/connection_dispatch.h`, `src/connection_dispatch.c`, and
`tests/test_connection_dispatch.c`):

```text
event_loop / reactor_step
    -> connection_reactor generation-token validation
    -> connection_dispatch manager membership + token validation
    -> connection_runtime_find_session()
    -> connection_session_readable() / connection_session_writable()
    -> existing bounded receive/send operations
```

The callback is passed directly to `omni_connection_reactor_init()`. Init
borrows a live manager and runtime; it does not register or remove reactor
entries. A callback must match the current manager entry's connection pointer
and generation token, then resolve an OPEN session through the manager's
existing runtime. Missing membership, a retired or mismatched token, a
closing connection, or a non-OPEN session produces an explicit ignored/stale
result and performs no I/O. The bridge keeps no second connection/session map.

The fixed result records the original event mask and token, per-direction
session I/O results (including their errno), bytes moved, attempted flags,
stale/ignored flags, and close-worthy observations. Dispatch counters,
ignored/stale counts, read/write counts, error-event observations, and byte
totals saturate at `UINT64_MAX`. The bridge lifecycle is
`INERT -> ACTIVE -> CLOSED`; duplicate init is rejected, and destroy only
clears borrowed references and local lifecycle state.

For combined readiness, ERROR/HANGUP/INVALID are recorded first. INVALID
suppresses ordinary I/O because the descriptor cannot be used safely. If
READ and WRITE are also present on a valid descriptor, one READ runs before
one WRITE. Each delegates once through the session. Bytes remain in the
existing bounded byte buffers; this layer does not parse, compact, queue,
retry, or loop. EOF, fatal receive/send errors, and peer-closed writes are
reported through the embedded I/O results and `close_worthy`; manager
membership, connection/session lifecycle, reactor registrations, and the FD
remain with their existing owners.

The bridge borrows the manager and uses manager membership/token lookup,
then `connection_runtime_find_session()`. It owns only local accounting and
the last fixed-size result. It never owns or closes an FD, destroys a
connection/session, removes membership, changes registration interests, or
allocates. Its callback context must stay alive until connection-reactor
registrations that can call it have been detached, even if the bridge was
destroyed earlier.

The bridge preserves the connection's configured interest mask. A socket is
often writable continuously, so permanent WRITE interest on connections with
no queued data can keep a readiness loop busy. Tests that need WRITE readiness
configure their connection explicitly with WRITE interest. Dynamic
READ/WRITE interest synchronization and backpressure policy were deferred to
Task 032; the policy section below now implements interest synchronization.

The focused loopback suite proves the full listener-readiness admission path
followed by connection READ readiness into the exact session receive buffer,
including NUL and high-byte values. A separate WRITE-readiness proof appends
binary data to an already-managed session buffer and verifies exact peer
bytes. The suite also covers would-block, buffer-full/no-compaction, EOF,
fatal read/write, ERROR/HANGUP/real INVALID, combined events, manager removal,
session/connection closure, generation reuse, ownership preservation,
saturating counters, and 1,000 real admit/read/release cycles with manager and
admission capacity bounds plus an FD census.

On the current 64-bit Linux ABI, the focused test measures
`sizeof(struct omni_connection_dispatch)` as 152 bytes,
`sizeof(struct omni_connection_dispatch_config)` as 8 bytes, and
`sizeof(struct omni_connection_dispatch_result)` as 72 bytes. No per-
connection dispatch table or heap-backed callback state is retained. The
bridge is linked only into `test_connection_dispatch`; production startup,
version output, and the native executable remain unchanged. HTTP, API parity,
protocol framing, and production server startup remain out of scope.

## Task 031 validation record

`connection-dispatch-unit` passes 90 checks with zero failures, including
listener-to-session READ integration, managed WRITE integration, and the
1,000-cycle admit/read/release stress with a stable FD census. The source
boundary gate passes with no direct network, readiness, descriptor-lifecycle,
heap, thread, TLS, or alternate-loop calls in `connection_dispatch.c`.
Full native CTest passes 29/29 in every matrix leg: GCC Debug, GCC Release,
GCC Debug ASan+UBSan, Clang Debug, Clang Release, and Clang Debug ASan+UBSan.
`npm run check:docs-all` passes; its reports include the repository's 91
version-drift warnings and two soft documentation-count warnings. The
production executable target builds, `--version` still prints
`omniroute-native 0.1.0`, and ordinary startup still prints the existing
`ready (debug, c11)` line before exiting. `nm -g` shows only `main`,
`omni_meminfo_read`, and runtime support symbols; no listener, reactor,
manager, admission, session, or dispatch symbols are linked into the
executable, and startup opens no listener.

## Bounded connection lifecycle and reactor-interest policy (Task 032)

Task 032 adds one explicit policy owner above the Task 031 dispatch result:
`include/omniroute/connection_policy.h` and `src/connection_policy.c`.
It is linked only into `test_connection_policy`; it is not wired into
`main.c`, the production executable, or the native startup path.

The connection-reactor callback can use
`omni_connection_policy_callback()`, which calls the existing Task 031
dispatcher once and applies its last result synchronously. ERROR or HANGUP
readiness adds READ to that single bounded dispatch unless INVALID is also
present. This allows pending bytes to be received, or EOF/fatal I/O to be
surfaced, before lifecycle policy runs. INVALID suppresses all I/O and is
close-worthy immediately. ERROR/HANGUP bits alone do not close a connection
when the bounded READ result is viable; EOF, fatal I/O, peer-closed send
results, and INVALID do. No second reactor step, retry, or recursive dispatch
occurs.

For a live connection the policy derives pending output from the existing
session send byte buffer. Desired interests are always READ while the
connection is open, plus WRITE only when that buffer has readable bytes.
`omni_connection_policy_sync_interests()` is the explicit synchronization
point after output is appended; `apply()` performs the same calculation after
each dispatch. WRITE WOULD_BLOCK preserves pending bytes and READ|WRITE
interest. When a write drains the buffer, the policy removes WRITE and leaves
READ. It skips redundant updates when the desired mask already matches the
connection metadata.

Interest changes use the existing `omni_poller_update()` through the new
`omni_reactor_update_interests()` and
`omni_connection_reactor_update_interests()` forwarding operations. They
resolve the existing generation token, keep registration and registry
membership, update the poller first, then the reactor record and connection
interest metadata. A poller failure leaves all three masks and the token
unchanged. No new registration is created.

Close-worthy policy calls `omni_connection_admission_release()` with the
existing admission slot index and manager/reactor generation token. That owner
detaches the reactor registration and registry membership through manager →
runtime → connection-reactor, destroys the session bookkeeping, then destroys
the connection owner and frees the admission slot. The policy never calls
`close()` or duplicates release ordering. A release error is returned with
its admission/manager status and errno; the result does not claim success.
As connection-lifecycle cleanup, release destroys the session receive/send
buffer objects and accepted connection owner; it does not parse, transform,
or protocol-drain buffered input.

Stale or ignored dispatch results are handled before close classification.
They do not update interests or release a slot, including when an old result
contains INVALID. The policy keeps no second per-connection table; it resolves
the current admission slot by its existing generation token. Its lifecycle is
`INERT -> ACTIVE -> CLOSED`; destroy clears borrowed references but does not
release owners. Caller storage must remain alive while the reactor retains the
policy callback context.

The focused loopback proof covers listener readiness through admission,
READ-only idle registration, exact binary output delivery, READ|WRITE enable,
drain and WRITE removal, an idle zero-time probe with no repeated WRITE
callback, EOF and reset-error cleanup, HANGUP with buffered bytes, stale-token
reuse, update/release failures, listener and unrelated-connection survival,
and 1,000 admit/dispatch/interest-update/release cycles with bounded manager,
admission, poller/reactor registrations, and a baseline Linux FD census. On
the current 64-bit Linux ABI, `struct omni_connection_policy`, config, and
result measure 208, 16, and 120 bytes. The policy performs no allocation and
retains no history or per-connection state. HTTP, protocol parsing, protocol
drain semantics, and production serving remain deferred.

## Task 032 validation record

`connection-policy-unit` reports 85 checks, zero failures, and 64-bit Linux
sizes of 208 bytes for the policy, 16 bytes for its config, and 120 bytes for
its result. Full native CTest passes 30/30 in each final build: GCC Debug,
GCC Release, GCC Debug ASan+UBSan, Clang Debug, Clang Release, and Clang Debug
ASan+UBSan. Both sanitizer legs use
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1`. The 1,000-cycle test returns the Linux FD
census to baseline. The direct network/heap boundary gate passes and includes
`connection_policy.c`. The GCC Release production executable builds, reports
`omniroute-native 0.1.0`, and prints the existing release startup line; its
symbol table contains `main` and `omni_meminfo_read` plus runtime support, with
no Task 031/032 or networking symbols. `npm run check:docs-all` passes after
this validation record was added; only the repository's pre-existing soft
documentation drift/count warnings are reported.

## Bounded HTTP request-line parser (Task 033)

Task 033 adds `include/omniroute/http_request_line.h` and
`src/http_request_line.c`, a standalone parser for exactly
`METHOD SP REQUEST-TARGET SP HTTP-VERSION CRLF`. It is built as the separate
`omni_http_request_line` static library and linked only into
`http-request-line-unit`; it is not connected to `main.c`, the event loop,
connection dispatch, or production startup. This is the first HTTP protocol
primitive in the native backend.

`omni_http_request_line_parse(data, length)` is stateless and single-pass. It
accepts a nonempty HTTP token method, a nonempty visible-ASCII target byte
sequence (`0x21` through `0x7e`), and the exact version shape `HTTP/d.d`.
Only `HTTP/1.0` and `HTTP/1.1` are supported; other digit-shaped versions
return `UNSUPPORTED_VERSION`. It requires CRLF. Bare LF is invalid, and a
prefix ending after CR remains incomplete until LF arrives.

The public limits are 32 method bytes, 4,060 target bytes, and 4,096 total
request-line bytes including CRLF. `COMPLETE` exposes borrowed method and
target spans, the version enum, and the exact consumed byte count. Spans point
into caller storage and remain valid only while that storage stays alive and
unchanged; they are not NUL-terminated. `INCOMPLETE` exposes no partial
spans and consumes zero bytes. `INVALID`, `TOO_LARGE`,
`UNSUPPORTED_VERSION`, and `ERR_INVALID_ARGUMENT` are explicit results with
deterministic offsets. Bytes after the first CRLF are ignored by this parser.

The parser uses no heap, I/O, syscalls, locale-sensitive classification, or
retained state. Its work is O(min(input prefix, 4,096 bytes)); additional
memory and persistent parser state are both zero bytes. On the tested 64-bit
Linux ABI, `sizeof(struct omni_http_byte_span)` is 16 bytes and
`sizeof(struct omni_http_request_line_result)` is 64 bytes. This primitive
does not parse headers or bodies, serialize responses, route, or implement an
HTTP server.

## Task 033 validation record

`http-request-line-unit` reports 3,043 checks and zero failures. It covers
valid borrowed spans, bytes after CRLF, every prefix of three valid request
lines, strict spacing and endings, field and total limits, null arguments,
stable error offsets, and all 256 byte values at every method, target, and
version-token position. Prefix tests use exact-size input allocations so the
sanitizer legs can detect a read beyond the supplied length. Input snapshots
remain unchanged, and the test measures the 16-byte span and 64-byte result.

Full native CTest passes 31/31 in GCC Debug, GCC Release, GCC Debug
ASan+UBSan, Clang Debug, Clang Release, and Clang Debug ASan+UBSan. Both
sanitizer configurations use
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1`. The direct network/heap boundary gate passes
with `http_request_line.c` included. The GCC Release production executable
builds; its `--version` and normal startup output match the pre-change
Task 032 baseline byte-for-byte, and `nm` shows no request-line parser symbol
linked into it. `npm run check:docs-all` passes; it reports 91 repository-wide
potential stale-date/version drifts. `git diff --check` passes.

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

Task 017 regression: unchanged — 16,640 bytes, `size` dec 4,767, and
one-shot `--meminfo` samples of 1,748–1,752 kB for both RSS and peak HWM
(`nm` confirms no receive, accepted, poller, listener, socket, bind,
readiness, or recv symbols in `omniroute-native`; the receive layer lives in
a separate static lib linked only into `test_recv`, and `main` receives
nothing).

Task 018 regression: unchanged — 16,640 bytes, `size` dec 4,767, and
one-shot `--meminfo` samples of 1,744–1,752 kB for both RSS and peak HWM
(`nm` confirms no send, receive, accepted, poller, listener, socket, bind,
readiness, or payload-I/O symbols in `omniroute-native`; the send layer lives
in a separate static lib linked only into `test_send`, and `main` sends
nothing).

Task 019 regression: unchanged — 16,640 bytes, `size` dec 4,767, and
one-shot `--meminfo` samples of 1,748–1,752 kB for both RSS and peak HWM
(`nm` confirms no connection, send, receive, accepted, poller, listener,
socket, bind, readiness, or payload-I/O symbols in `omniroute-native`; the
connection layer lives in a separate static lib linked only into
`test_connection`, and `main` owns no connection).

Task 020 regression: unchanged — 16,640 bytes, `size` dec 4,767, and
one-shot `--meminfo` samples of 1,748 kB for both RSS and peak HWM
(`nm` confirms no registry, connection, send, receive, accepted, poller,
listener, socket, bind, readiness, or payload-I/O symbols in
`omniroute-native`; the registry lives in a separate static lib linked only
into `test_registry`, and `main` tracks no connection).

Task 022 regression target: the adapter remains test-only linked, so the
production startup binary and its socket-free Task 011 memory baseline stay
unchanged. The focused adapter suite and full compiler/sanitizer matrix are
reported in the Task 022 validation record above after the final validation
run.

## Platform boundary

Linux x86_64/arm64 is the build target; the `/proc` reader is the only
Linux-specific code, isolated in `src/meminfo.c` behind `__linux__` with
an explicit unavailable stub elsewhere. No iOS code, no Objective-C/Swift,
no Apple frameworks yet. The `platform/` + `platform-toolchain/` split
from the migration design happens when iOS work starts.

## Intentionally deferred

Alternate event-loop backends (epoll/io_uring), production connection dispatcher,
HTTP headers/bodies, response serialization, full HTTP serving, `/health`,
`/v1/models`, TLS, SQLite, crypto,
auth, providers, routing, streaming, compression, MCP, A2A,
Objective-C/Swift/assembly.
(Arenas, byte buffers, listener lifecycle, readiness observation,
accepted-socket ownership with its bounded accept drain, receive with its
bounded drain, the immutable-span send primitive with its bounded drain,
the protocol-agnostic connection owner, the bounded connection registry, the
connection/reactor lifecycle adapter, the runtime coordinator, the bounded
synchronous event loop, the bounded connection I/O state machine, the
bounded connection/session integration, and the
bounded connection runtime binding, the Task 032 connection lifecycle policy,
and the Task 033 standalone request-line parser are
landed primitives now — see above.) Each
remaining item gets its own reviewable task.
// linguist refresh
