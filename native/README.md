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
  include/omniroute/         # version.h, exit_code.h, meminfo.h
  src/
    main.c                   # entry point, tiny CLI, lifecycle
    meminfo.c                # Linux /proc/self/status RSS hook (+ stub elsewhere)
  tests/
    CMakeLists.txt           # CTest cases (CLI, meminfo, no-network gate)
    check_no_network.sh      # review gate: no socket/HTTP/TLS calls in sources
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

## Initial baseline (Task 011, measured 2026-09-19)

Environment: Linux 7.2.4-zen2 x86_64, 8 CPUs, 16 GiB RAM; GCC 16.2.1,
Clang 22.1.8; Release + Debug builds from this tree.

| Metric                              | Value                                                                      |
| ----------------------------------- | -------------------------------------------------------------------------- |
| Executable size (Release, GCC)      | 16,640 bytes (`text+data+bss` ≈ 4.8 KiB)                                   |
| Executable size (Debug, GCC)        | 22,504 bytes                                                               |
| Startup/current RSS (`--meminfo`)   | ≈ 1,748 kB (one-shot print-and-exit, not a server)                         |
| Peak RSS (`VmHWM`)                  | ≈ 1,748 kB                                                                 |
| Explicit heap allocation in sources | none (`malloc/calloc/realloc/free/strdup` absent; libc internals excluded) |

Not a competition with the full Node server (200–400 MB idle RSS at
`OMNIROUTE_MEMORY_MB=512` covers hundreds of routes, providers, and
caches this skeleton does not have). These numbers exist so future slices
can detect regressions from day one.

Task 012 regression: unchanged — 16,640 bytes, RSS 1,748 kB, peak
1,748 kB (`nm` confirms no arena symbols in `omniroute-native`; the arena
lives in a separate static lib linked only into `test_arena`).

## Platform boundary

Linux x86_64/arm64 is the build target; the `/proc` reader is the only
Linux-specific code, isolated in `src/meminfo.c` behind `__linux__` with
an explicit unavailable stub elsewhere. No iOS code, no Objective-C/Swift,
no Apple frameworks yet. The `platform/` + `platform-toolchain/` split
from the migration design happens when iOS work starts.

## Intentionally deferred

Allocator/arenas, event loop (epoll/io_uring/threads), sockets, HTTP,
`/health`, `/v1/models`, TLS, SQLite, crypto, auth, providers, routing,
streaming, compression, MCP, A2A, Objective-C/Swift/assembly. Each gets its
own reviewable task.
