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

add_executable(omniroute-native
  src/main.c
  src/meminfo.c)

target_include_directories(omni_arena PUBLIC include)
target_include_directories(omni_bytebuf PUBLIC include)
target_include_directories(omni_listener PUBLIC include)
target_include_directories(omni_poller PUBLIC include)
target_include_directories(omni_accepted PUBLIC include)
target_include_directories(omni_recv PUBLIC include)
target_include_directories(omniroute-native PRIVATE include)

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
omni_apply_native_flags(omniroute-native)

include(CTest)
add_subdirectory(tests)
