# Focused Task 011 validation: CLI behavior, memory diagnostics, and the
# no-network guarantee. Every case runs the built executable directly with a
# hard timeout (a serving/blocking binary would fail here instead of hanging
# CI). No Node backend, no loopback servers, no network access.

set(OMNI_EXE "$<TARGET_FILE:omniroute-native>")

function(omni_cli_test name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "ARGS;PASS_REGEX;FAIL_REGEX;EXPECT_CODE" "")
  add_test(NAME ${name} COMMAND ${OMNI_EXE} ${ARG_ARGS})
  set_tests_properties(${name} PROPERTIES TIMEOUT 30)
  if(DEFINED ARG_PASS_REGEX)
    set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${ARG_PASS_REGEX}")
  endif()
  if(DEFINED ARG_FAIL_REGEX)
    set_tests_properties(${name} PROPERTIES FAIL_REGULAR_EXPRESSION "${ARG_FAIL_REGEX}")
  endif()
endfunction()

# --help exits 0 and documents every supported option.
omni_cli_test(help-exits-zero
  ARGS "--help"
  PASS_REGEX "Usage: omniroute-native.*--help.*--version.*--meminfo")

# --version exits 0 with the exact pinned version.
omni_cli_test(version-pinned
  ARGS "--version"
  PASS_REGEX "^omniroute-native 0\\.1\\.0")

# Unknown options fail with the documented usage code (2), not success.
# (OMNI_EXE is a generator expression; CMake expands it inside the sh -c
# script at generate time, so the shell sees a plain absolute path.)
add_test(NAME unknown-option-exit-code
  COMMAND sh -c "\"${OMNI_EXE}\" --serve >/dev/null 2>&1; test $? -eq 2")
set_tests_properties(unknown-option-exit-code PROPERTIES TIMEOUT 30)

# Positional arguments are usage errors too (no positional CLI exists yet).
add_test(NAME positional-arg-exit-code
  COMMAND sh -c "\"${OMNI_EXE}\" extra >/dev/null 2>&1; test $? -eq 2")
set_tests_properties(positional-arg-exit-code PROPERTIES TIMEOUT 30)

# Ordinary invocation prints the deterministic startup line and exits 0.
omni_cli_test(default-startup-line
  PASS_REGEX "omniroute-native 0\\.1\\.0 ready \\((debug|release), c11\\)")

# Memory diagnostics never crash and report numbers or explicit unavailable.
omni_cli_test(meminfo-shape
  ARGS "--meminfo"
  PASS_REGEX "rss_kb: ([0-9]+|unavailable)")

omni_cli_test(meminfo-peak-shape
  ARGS "--meminfo"
  PASS_REGEX "peak_rss_kb: ([0-9]+|unavailable)")

# On Linux the measurement must be available (numbers, never unavailable).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  omni_cli_test(meminfo-available-on-linux
    ARGS "--meminfo"
    PASS_REGEX "rss_kb: [0-9]+")
endif()

# Networking is confined to src/listener.c (Task 014): fail if socket/FD
# calls appear anywhere else in production sources, or if deferred layers
# (accept path, event loop, payload I/O, threads, TLS) appear anywhere at
# all. Tests under tests/ are intentionally not scanned — the loopback
# tests legitimately use client-side sockets.
add_test(NAME network-source-boundary
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/check_network_boundary.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(network-source-boundary PROPERTIES TIMEOUT 30)

# Bounded arena allocator unit tests (Task 012): self-contained C program,
# deterministic output, nonzero exit on failure. Inherits the strict warning
# set and sanitizer flags via the parent-scope helper.
add_executable(test_arena test_arena.c)
target_link_libraries(test_arena PRIVATE omni_arena)
omni_apply_native_flags(test_arena)
add_test(NAME arena-unit COMMAND test_arena)
set_tests_properties(arena-unit PROPERTIES TIMEOUT 60)

# Bounded reusable byte buffer unit tests (Task 013): self-contained C
# program, deterministic output, nonzero exit on failure. Same conventions
# as the arena suite (no randomness, no network, no giant buffers).
add_executable(test_bytebuf test_bytebuf.c)
target_link_libraries(test_bytebuf PRIVATE omni_bytebuf)
omni_apply_native_flags(test_bytebuf)
add_test(NAME bytebuf-unit COMMAND test_bytebuf)
set_tests_properties(bytebuf-unit PROPERTIES TIMEOUT 60)

# Linux TCP listener lifecycle tests (Task 014): loopback-only,
# self-cleaning, no external network. Same strict-warning/sanitizer
# treatment as the other suites.
add_executable(test_listener test_listener.c)
target_link_libraries(test_listener PRIVATE omni_listener)
omni_apply_native_flags(test_listener)
add_test(NAME listener-unit COMMAND test_listener)
set_tests_properties(listener-unit PROPERTIES TIMEOUT 120)

# Bounded readiness tests (Task 015): generic-FD behavior plus one
# loopback listener integration proof. Self-cleaning, no external
# network. Same strict-warning/sanitizer treatment as the other suites.
add_executable(test_poller test_poller.c)
target_link_libraries(test_poller PRIVATE omni_poller omni_listener)
omni_apply_native_flags(test_poller)
add_test(NAME poller-unit COMMAND test_poller)
set_tests_properties(poller-unit PROPERTIES TIMEOUT 120)

# Accepted-socket ownership plus bounded accept4 drain tests (Task 016):
# single acceptance, bounded drain, capacity limits, listener/poller
# integration, descriptor-leak stress. Loopback-only, self-cleaning, no
# external network, no payload transfer. Same strict-warning/sanitizer
# treatment as the other suites.
add_executable(test_accepted test_accepted.c)
target_link_libraries(test_accepted PRIVATE omni_accepted omni_listener omni_poller)
omni_apply_native_flags(test_accepted)
add_test(NAME accepted-unit COMMAND test_accepted)
set_tests_properties(accepted-unit PROPERTIES TIMEOUT 120)

# Bounded nonblocking socket receive tests (Task 017): one-shot receive,
# bounded drain, buffer-full/partial/EOF/RST paths, poller integration,
# deterministic stress. Loopback-only, self-cleaning, no external
# network. First suite allowed to move payload bytes (test client sends
# only; production receives only). Same strict-warning/sanitizer
# treatment as the other suites.
add_executable(test_recv test_recv.c)
target_link_libraries(test_recv PRIVATE omni_recv omni_accepted omni_listener omni_poller omni_bytebuf)
omni_apply_native_flags(test_recv)
add_test(NAME recv-unit COMMAND test_recv)
set_tests_properties(recv-unit PROPERTIES TIMEOUT 120)
