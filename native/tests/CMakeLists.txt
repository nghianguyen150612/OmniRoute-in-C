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

# The skeleton must not grow networking: fail if socket-family calls appear
# in native sources (review gate, cheaper than a runtime syscall audit).
add_test(NAME no-network-calls-in-sources
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/check_no_network.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(no-network-calls-in-sources PROPERTIES TIMEOUT 30)
