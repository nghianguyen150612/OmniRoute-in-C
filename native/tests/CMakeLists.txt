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

# Networking is confined to the deliberate native layers: fail if socket/FD
# calls appear outside their owners, or if deferred layers (alternate event
# loop backends, payload I/O, threads, TLS) appear anywhere at all. Tests under
# tests/ are intentionally not scanned — the loopback tests legitimately use
# client-side sockets.
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

# Bounded nonblocking socket send tests (Task 018): immutable binary spans,
# partial progress, bounded drain, backpressure, SIGPIPE-safe peer failure,
# poller WRITE integration, stress, and descriptor census. Loopback-only,
# self-cleaning, no external network, and no production server loop.
add_executable(test_send test_send.c)
target_link_libraries(test_send PRIVATE omni_send omni_accepted omni_listener omni_poller)
omni_apply_native_flags(test_send)
add_test(NAME send-unit COMMAND test_send)
set_tests_properties(send-unit PROPERTIES TIMEOUT 180)

# Protocol-agnostic connection ownership tests (Task 019): explicit
# accepted-FD transfer, READY/OPEN/CLOSING/CLOSED lifecycle, embedded
# borrowed receive storage, delegated recv/send, poller metadata, FD-0,
# peer failure, deterministic stress, and descriptor census. Loopback-only,
# self-cleaning, no external network, and no production event loop.
add_executable(test_connection test_connection.c)
target_link_libraries(test_connection PRIVATE omni_connection omni_recv omni_send omni_accepted
                                             omni_listener omni_poller omni_bytebuf)
omni_apply_native_flags(test_connection)
add_test(NAME connection-unit COMMAND test_connection)
set_tests_properties(connection-unit PROPERTIES TIMEOUT 240)

# Bounded connection registry tests (Task 020): caller-owned slots, handle
# generations, add/remove/lookup, deterministic iteration, stress, and
# caller-ownership survival. Pure bookkeeping, no sockets, no poller waits,
# no payload transfer, and no production event loop.
add_executable(test_registry test_registry.c)
target_link_libraries(test_registry PRIVATE omni_registry omni_connection omni_recv omni_send
                                             omni_accepted omni_listener omni_poller omni_bytebuf)
omni_apply_native_flags(test_registry)
add_test(NAME registry-unit COMMAND test_registry)
set_tests_properties(registry-unit PROPERTIES TIMEOUT 120)

# Bounded reactor foundation tests (Task 021): poller-reactor integration,
# callback dispatch, token identity round-trip, capacity boundaries, duplicate
# handling, interruption, and bounded step execution. Local pipe-only, self-cleaning,
# no external network, no production payload transfer, no protocol handling.
add_executable(test_reactor test_reactor.c)
target_link_libraries(test_reactor PRIVATE omni_reactor omni_poller)
omni_apply_native_flags(test_reactor)
add_test(NAME reactor-unit COMMAND test_reactor)
set_tests_properties(reactor-unit PROPERTIES TIMEOUT 120)

# Connection/reactor lifecycle adapter tests (Task 022): fixed registry
# membership plus reactor registration, generation/index token invalidation,
# synchronous readable/writable/error callback forwarding, removal during a
# bounded dispatch step, reactor-failure rollback, destruction ordering, and
# repeated attach/detach. No protocol handling, payload I/O, or production
# startup wiring is involved.
add_executable(test_connection_reactor test_connection_reactor.c)
target_link_libraries(test_connection_reactor PRIVATE omni_connection_reactor omni_reactor
                                                       omni_registry omni_connection omni_recv
                                                       omni_send omni_accepted omni_listener omni_poller
                                                       omni_bytebuf)
omni_apply_native_flags(test_connection_reactor)
add_test(NAME connection-reactor-unit COMMAND test_connection_reactor)
set_tests_properties(connection-reactor-unit PROPERTIES TIMEOUT 180)

# Native runtime coordinator tests (Task 023): explicit subsystem startup and
# reverse shutdown ordering, late listener-failure cleanup, invalid lifecycle
# transitions, caller-owned connection survival, and repeated bounded cycles.
# The runtime remains a lifecycle-only dependency for the separate Task 024
# event-loop tests.
add_executable(test_runtime test_runtime.c)
target_link_libraries(test_runtime PRIVATE omni_runtime omni_reactor omni_registry omni_poller
                                           omni_listener omni_connection omni_recv omni_send
                                           omni_accepted omni_bytebuf)
omni_apply_native_flags(test_runtime)
add_test(NAME runtime-unit COMMAND test_runtime)
set_tests_properties(runtime-unit PROPERTIES TIMEOUT 240)

# Native event-loop foundation tests (Task 024): runtime binding, empty-reactor
# exit, finite timeout validation, synchronous readiness dispatch, callback
# stop requests, interrupted waits, borrowed descriptor/connection survival,
# repeated stop safety, and repeated init/run/destroy cycles. The loop owns no
# protocol state and is not wired into production startup.
add_executable(test_event_loop test_event_loop.c)
target_link_libraries(test_event_loop PRIVATE omni_event_loop omni_runtime
                                               omni_connection_reactor omni_reactor
                                               omni_registry omni_connection omni_recv omni_send
                                               omni_accepted omni_listener omni_poller omni_bytebuf)
omni_apply_native_flags(test_event_loop)
add_test(NAME event-loop-unit COMMAND test_event_loop)
set_tests_properties(event-loop-unit PROPERTIES TIMEOUT 240)

# Bounded connection I/O state machine tests (Task 025): state machine,
# read/write, close/error transitions, ownership survival. Loopback-only,
# self-cleaning, no external network, and no production event loop.
add_executable(test_connection_io test_connection_io.c)
target_link_libraries(test_connection_io PRIVATE omni_connection_io omni_recv omni_send omni_accepted omni_listener omni_poller omni_bytebuf)
omni_apply_native_flags(test_connection_io)
add_test(NAME connection-io-unit COMMAND test_connection_io)
set_tests_properties(connection-io-unit PROPERTIES TIMEOUT 240)

# Bounded connection/session integration tests (Task 026): lifecycle
# coordination between connection and connection_io without protocol logic.
# Loopback-only, self-cleaning, no external network, no heap, no production
# event loop.
add_executable(test_connection_session test_connection_session.c)
target_link_libraries(test_connection_session PRIVATE omni_connection_session omni_connection_io omni_connection omni_recv omni_send omni_accepted omni_listener omni_poller omni_bytebuf omni_registry)
omni_apply_native_flags(test_connection_session)
add_test(NAME connection-session-unit COMMAND test_connection_session)
set_tests_properties(connection-session-unit PROPERTIES TIMEOUT 240)

# Bounded connection runtime binding tests (Task 027): lifecycle coordination
# between event loop, connection reactor and connection session without
# protocol logic. Loopback-only, self-cleaning, no heap, no production
# server.
add_executable(test_connection_runtime test_connection_runtime.c)
target_link_libraries(test_connection_runtime PRIVATE omni_connection_runtime omni_connection_session omni_connection_io omni_connection omni_connection_reactor omni_reactor omni_registry omni_poller omni_event_loop omni_runtime omni_listener omni_recv omni_send omni_accepted omni_bytebuf)
omni_apply_native_flags(test_connection_runtime)
add_test(NAME connection-runtime-unit COMMAND test_connection_runtime)
set_tests_properties(connection-runtime-unit PROPERTIES TIMEOUT 240)

# Bounded connection manager tests (Task 028): fixed collection of active
# runtime attachments, lifecycle, bounded capacity, token invalidation,
# reactor cleanup, ownership preservation. Loopback-only, self-cleaning,
# no heap, no protocol, no production server.
add_executable(test_connection_manager test_connection_manager.c)
target_link_libraries(test_connection_manager PRIVATE omni_connection_manager omni_connection_runtime omni_connection_session omni_connection_io omni_connection omni_connection_reactor omni_reactor omni_registry omni_poller omni_event_loop omni_runtime omni_listener omni_recv omni_send omni_accepted omni_bytebuf)
omni_apply_native_flags(test_connection_manager)
add_test(NAME connection-manager-unit COMMAND test_connection_manager)
set_tests_properties(connection-manager-unit PROPERTIES TIMEOUT 240)

# Bounded listener-to-manager admission (Task 029): fixed caller storage,
# accepted-FD transfer, manager attach rollback, generation identity, and
# synchronous release. Loopback-only; production startup remains unchanged.
add_executable(test_connection_admission test_connection_admission.c)
target_link_libraries(test_connection_admission PRIVATE omni_connection_admission omni_connection_manager omni_connection_runtime omni_connection_session omni_connection_io omni_connection omni_connection_reactor omni_reactor omni_registry omni_poller omni_event_loop omni_runtime omni_listener omni_recv omni_send omni_accepted omni_bytebuf)
omni_apply_native_flags(test_connection_admission)
add_test(NAME connection-admission-unit COMMAND test_connection_admission)
set_tests_properties(connection-admission-unit PROPERTIES TIMEOUT 300)

# Bounded listener-readiness admission bridge (Task 030): generic reactor
# registration, finite Task 029 drains, lifecycle removal, and loopback
# event-loop integration. Production startup stays unchanged.
add_executable(test_listener_admission test_listener_admission.c)
target_link_libraries(test_listener_admission PRIVATE
  omni_listener_admission omni_connection_admission omni_connection_manager
  omni_connection_runtime omni_connection_session omni_connection_io
  omni_connection omni_connection_reactor omni_reactor omni_registry omni_poller
  omni_event_loop omni_runtime omni_listener omni_recv omni_send omni_accepted
  omni_bytebuf)
omni_apply_native_flags(test_listener_admission)
add_test(NAME listener-admission-unit COMMAND test_listener_admission)
set_tests_properties(listener-admission-unit PROPERTIES TIMEOUT 300)

# Bounded managed connection readiness dispatch (Task 031). The bridge
# remains test-only and is not linked into omniroute-native.
add_executable(test_connection_dispatch test_connection_dispatch.c)
target_link_libraries(test_connection_dispatch PRIVATE
  omni_connection_dispatch omni_connection_admission omni_listener_admission
  omni_connection_manager omni_connection_runtime omni_connection_session
  omni_connection_io omni_connection omni_connection_reactor omni_reactor
  omni_registry omni_poller omni_event_loop omni_runtime omni_listener omni_recv
  omni_send omni_accepted omni_bytebuf)
omni_apply_native_flags(test_connection_dispatch)
add_test(NAME connection-dispatch-unit COMMAND test_connection_dispatch)
set_tests_properties(connection-dispatch-unit PROPERTIES TIMEOUT 300)

# Bounded managed-connection lifecycle policy (Task 032). Kept test-only.
add_executable(test_connection_policy test_connection_policy.c)
target_link_libraries(test_connection_policy PRIVATE
  omni_connection_policy omni_connection_dispatch omni_connection_admission
  omni_listener_admission omni_connection_manager omni_connection_runtime
  omni_connection_session omni_connection_io omni_connection
  omni_connection_reactor omni_reactor omni_registry omni_poller
  omni_event_loop omni_runtime omni_listener omni_recv omni_send omni_accepted
  omni_bytebuf)
omni_apply_native_flags(test_connection_policy)
add_test(NAME connection-policy-unit COMMAND test_connection_policy)
set_tests_properties(connection-policy-unit PROPERTIES TIMEOUT 300)

# Bounded HTTP request-line parser (Task 033). Standalone and test-only; it
# does not introduce production startup, headers, bodies, or routing.
add_executable(test_http_request_line test_http_request_line.c)
target_link_libraries(test_http_request_line PRIVATE omni_http_request_line)
omni_apply_native_flags(test_http_request_line)
add_test(NAME http-request-line-unit COMMAND test_http_request_line)
set_tests_properties(http-request-line-unit PROPERTIES TIMEOUT 60)

# Bounded HTTP header-line parser (Task 034). Standalone and test-only; it
# does not introduce header-block parsing or production startup wiring.
add_executable(test_http_header_line test_http_header_line.c)
target_link_libraries(test_http_header_line PRIVATE omni_http_header_line)
omni_apply_native_flags(test_http_header_line)
add_test(NAME http-header-line-unit COMMAND test_http_header_line)
set_tests_properties(http-header-line-unit PROPERTIES TIMEOUT 60)
