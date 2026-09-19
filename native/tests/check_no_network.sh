#!/bin/sh
# Task 011 review gate: the native skeleton must not implement sockets, HTTP,
# TLS, or any network client/server call. Fails loudly on the first match.
# Usage: check_no_network.sh <native-dir>
set -u

NATIVE_DIR="${1:?usage: check_no_network.sh <native-dir>}"

# Code-level call tokens (word followed by an open paren), not prose: comments
# in native sources must avoid these tokens adjacent to "(".
PATTERN='\<(socket|bind|listen|accept|connect|getaddrinfo|getnameinfo|sendmsg|recvmsg|poll|epoll_create|epoll_wait|kqueue|SSL_|TLS_|mbedtls|pthread_create)\s*\('

if grep -rEn --include='*.c' --include='*.h' "$PATTERN" "$NATIVE_DIR/src" "$NATIVE_DIR/include"; then
  echo "FAIL: network/threading call found in native skeleton (see match above)" >&2
  exit 1
fi

echo "OK: no socket/HTTP/TLS/threading calls in native skeleton"
