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
  include/omniroute/         # version.h, exit_code.h, meminfo.h, arena.h, bytebuf.h, listener.h, poller.h, accepted.h, recv.h, send.h
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
  tests/
    CMakeLists.txt           # CTest cases (CLI, meminfo, units, network-boundary gate)
    check_network_boundary.sh  # review gate: socket setup confined to src/listener.c,
                              # readiness wait to src/poller.c, accept path plus
                              # accepted-FD lifecycle to src/accepted.c, receive
                              # to src/recv.c, and send to src/send.c;
                              # deferred layers (loop/queues/IO/scatter/TLS)
                              # banned everywhere in production sources, heap
                              # allocation banned in accepted.c, recv.c, send.c
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

## Platform boundary

Linux x86_64/arm64 is the build target; the `/proc` reader is the only
Linux-specific code, isolated in `src/meminfo.c` behind `__linux__` with
an explicit unavailable stub elsewhere. No iOS code, no Objective-C/Swift,
no Apple frameworks yet. The `platform/` + `platform-toolchain/` split
from the migration design happens when iOS work starts.

## Intentionally deferred

Event loop (epoll/io_uring/threads), production connection dispatcher,
output queues and connection write state, HTTP, `/health`, `/v1/models`, TLS, SQLite, crypto,
auth, providers, routing, streaming, compression, MCP, A2A,
Objective-C/Swift/assembly.
(Arenas, byte buffers, listener lifecycle, readiness observation,
accepted-socket ownership with its bounded accept drain, receive with its
bounded drain, and the immutable-span send primitive with its bounded drain
are landed primitives now — see above.) Each
remaining item gets its own reviewable task.
