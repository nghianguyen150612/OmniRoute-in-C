# OmniRoute native backend — Linux-first CMake build (Task 011 foundation).
#
# Design: docs/native-backend/MIGRATION_PLAN.md section 1
# (`native/CMakeLists.txt`, iOS toolchain file arrives with iOS work).
#
# Out-of-tree only; in-tree object files are never produced:
#   cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Debug
#   cmake --build native/build
#   ctest --test-dir native/build --output-on-failure
#
# Compiler selection is explicit (no surprises):
#   CC=clang cmake -S native -B native/build-clang ...
#   cmake -S native -B native/build -DCMAKE_C_COMPILER=gcc ...
#
# Sanitizers (Debug only, never Release):
#   cmake -S native -B native/build-asan -DOMNIROUTE_SANITIZE=ON \
#     -DCMAKE_BUILD_TYPE=Debug

cmake_minimum_required(VERSION 3.21)

project(omniroute-native
  VERSION 0.1.0
  DESCRIPTION "OmniRoute native backend executable skeleton"
  LANGUAGES C)

# Conservative portable baseline: C11 is the newest standard guaranteed by
# GCC, Clang, and the older Apple Clang behind the iOS 12 SDK. Strict ISO
# (no GNU extensions) so Linux-only code stays behind explicit #ifdefs.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Portable optimization: explicit -O2, no -ffast-math, no -march=native,
# no LTO. Tuning comes later with measurements.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
endif()

option(OMNIROUTE_SANITIZE "Enable AddressSanitizer + UndefinedBehaviorSanitizer (Debug only)" OFF)
if(OMNIROUTE_SANITIZE AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  message(FATAL_ERROR "OMNIROUTE_SANITIZE=ON requires -DCMAKE_BUILD_TYPE=Debug")
endif()

add_library(omni_arena STATIC
  src/arena.c)

add_library(omni_bytebuf STATIC
  src/bytebuf.c)

# Listener lifecycle lives in its own static lib, linked only into the
# listener tests: omniroute-native stays a socket-free executable until a
# later server-runtime task deliberately wires it in.

add_library(omni_listener STATIC
  src/listener.c)

# Readiness wait lives in its own static lib, linked only into the poller
# tests: omniroute-native stays free of readiness machinery until a later
# server-runtime task deliberately wires it in.

add_library(omni_poller STATIC
  src/poller.c)

# Accepted-socket ownership plus the bounded accept4 drain live in their own
# static lib, linked only into the accepted tests: omniroute-native stays
# free of accept machinery until a later server-runtime task deliberately
# wires it in.

add_library(omni_accepted STATIC
  src/accepted.c)

# Bounded nonblocking socket receive lives in its own static lib, linked
# only into the receive tests: omniroute-native stays free of payload I/O
# until a later server-runtime task deliberately wires it in.

add_library(omni_recv STATIC
  src/recv.c)

# Bounded nonblocking socket send lives in its own static lib, linked only
# into the send tests: omniroute-native stays free of payload I/O until a
# later server-runtime task deliberately wires it in.

add_library(omni_send STATIC
  src/send.c)

# Protocol-agnostic connection ownership composes the accepted owner, the
# bounded receive buffer, and the existing recv/send primitives. It is linked
# only into connection tests: omniroute-native remains a short-lived,
# socket-free executable until a later server-runtime task deliberately wires
# a connection dispatcher in.

add_library(omni_connection STATIC
  src/connection.c)

# Bounded connection registry tracks borrowed connection references for a
# future event loop. It is linked only into the registry tests:
# omniroute-native remains a short-lived, socket-free executable until a
# later server-runtime task deliberately wires a dispatcher in.

add_library(omni_registry STATIC
  src/registry.c)

# Bounded reactor foundation connects poller to application callbacks. It is
# linked only into the reactor and higher-layer native tests: omniroute-native
# remains a socket-free executable until a later server-runtime task
# deliberately wires it in.

add_library(omni_reactor STATIC
  src/reactor.c)

# The connection/reactor adapter composes the existing connection registry,
# connection, and reactor contracts. It remains test-only; production startup
# still has no listener, connection, registry, or readiness wiring.
add_library(omni_connection_reactor STATIC
  src/connection_reactor.c)

# The runtime coordinator composes the existing listener, registry, poller,
# and reactor lifecycle contracts. It remains test-only; production startup
# still has no runtime loop or native server wiring.
add_library(omni_runtime STATIC
  src/runtime.c)

# The synchronous event loop consumes a running runtime and repeatedly calls
# the existing bounded reactor step. It remains test-only; it adds no
# production startup wiring, protocol behavior, or background execution.
add_library(omni_event_loop STATIC
  src/event_loop.c)

# Bounded connection I/O state machine foundation (Task 025). Owns receive
# and send buffer interaction, I/O state transitions, and read/write readiness
# handling. It remains test-only; production startup still has no connection
# dispatcher or I/O wiring.
add_library(omni_connection_io STATIC
  src/connection_io.c)

# Bounded connection/session integration (Task 026). Coordinates the
# connection ownership model with the connection I/O lifecycle without
# introducing protocol behavior. Remains test-only.
add_library(omni_connection_session STATIC
  src/connection_session.c)

# Bounded connection runtime binding layer (Task 027). Coordinates event loop,
# connection reactor and connection session without protocol behavior.
# Remains test-only.
add_library(omni_connection_runtime STATIC
  src/connection_runtime.c)

# Bounded connection manager layer (Task 028). Manages a fixed collection
# of active connection_runtime attachments without protocol behavior.
# Remains test-only.
add_library(omni_connection_manager STATIC
  src/connection_manager.c)

# Bounded connection admission pipeline (Task 029). Coordinates the existing
# listener/accepted/connection/manager layers with caller-owned slot and
# buffer storage. Remains test-only; production startup and binary stay free
# of networking and admission wiring.
add_library(omni_connection_admission STATIC
  src/connection_admission.c)

# The Task 030 listener-readiness bridge composes the generic reactor with
# Task 029's bounded admission drain. It remains linked only into native
# tests; production startup still does not register or accept a listener.
add_library(omni_listener_admission STATIC
  src/listener_admission.c)

# The Task 031 connection-readiness bridge resolves live managed sessions and
# delegates bounded read/write readiness. It remains test-only; production
# startup does not register a connection dispatcher.
add_library(omni_connection_dispatch STATIC
  src/connection_dispatch.c)

add_executable(omniroute-native
  src/main.c
  src/meminfo.c)

target_include_directories(omni_arena PUBLIC include)
target_include_directories(omni_bytebuf PUBLIC include)
target_include_directories(omni_listener PUBLIC include)
target_include_directories(omni_poller PUBLIC include)
target_include_directories(omni_accepted PUBLIC include)
target_include_directories(omni_recv PUBLIC include)
target_include_directories(omni_send PUBLIC include)
target_include_directories(omni_connection PUBLIC include)
target_include_directories(omni_registry PUBLIC include)
target_include_directories(omni_reactor PUBLIC include)
target_include_directories(omni_connection_reactor PUBLIC include)
target_include_directories(omni_runtime PUBLIC include)
target_include_directories(omni_event_loop PUBLIC include)
target_include_directories(omni_connection_io PUBLIC include)
target_include_directories(omni_connection_session PUBLIC include)
target_include_directories(omni_connection_runtime PUBLIC include)
target_include_directories(omni_connection_manager PUBLIC include)
target_include_directories(omni_connection_admission PUBLIC include)
target_include_directories(omni_listener_admission PUBLIC include)
target_include_directories(omni_connection_dispatch PUBLIC include)
target_include_directories(omniroute-native PRIVATE include)

target_link_libraries(omni_event_loop PUBLIC omni_runtime omni_reactor omni_registry omni_poller
                                              omni_listener)

# Strong warning set for GCC and Clang; warnings are errors on every native
# target. Kept to flags with stable cross-compiler behavior (no experimental
# noise).
set(OMNI_STRICT_WARNINGS
  -Wall -Wextra -Wpedantic -Werror
  -Wshadow
  -Wstrict-prototypes
  -Wold-style-definition
  -Wconversion)

function(omni_apply_native_flags target)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE ${OMNI_STRICT_WARNINGS})
  endif()
  if(OMNIROUTE_SANITIZE)
    target_compile_options(${target} PRIVATE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE
      -fsanitize=address,undefined)
  endif()
endfunction()

omni_apply_native_flags(omni_arena)
omni_apply_native_flags(omni_bytebuf)
omni_apply_native_flags(omni_listener)
omni_apply_native_flags(omni_poller)
omni_apply_native_flags(omni_accepted)
omni_apply_native_flags(omni_recv)
omni_apply_native_flags(omni_send)
omni_apply_native_flags(omni_connection)
omni_apply_native_flags(omni_registry)
omni_apply_native_flags(omni_reactor)
omni_apply_native_flags(omni_connection_reactor)
omni_apply_native_flags(omni_runtime)
omni_apply_native_flags(omni_event_loop)
omni_apply_native_flags(omni_connection_io)
omni_apply_native_flags(omni_connection_session)
omni_apply_native_flags(omni_connection_runtime)
omni_apply_native_flags(omni_connection_manager)
omni_apply_native_flags(omni_connection_admission)
omni_apply_native_flags(omni_listener_admission)
omni_apply_native_flags(omni_connection_dispatch)
omni_apply_native_flags(omniroute-native)

include(CTest)
add_subdirectory(tests)
