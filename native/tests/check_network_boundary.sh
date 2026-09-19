#!/bin/sh
# Task 014 network source-boundary gate: native networking exists, but ONLY
# in the deliberate networking layer (src/listener.c). Every other
# production module (arena, bytebuf, meminfo, main, all other headers and
# sources) must stay socket-free, and the deferred layers — accept path,
# event loop, socket payload I/O, DNS/client, threads, TLS — stay banned
# from ALL production sources, including the networking layer itself.
# Later tasks extend this gate explicitly; they never loosen it silently.
#
# Usage: check_network_boundary.sh <native-dir>
set -u

NATIVE_DIR="${1:?usage: check_network_boundary.sh <native-dir>}"
FAIL=0

# 1. Socket/FD-lifecycle tokens: allowed ONLY in src/listener.c. Matches are
# code-level call tokens (word followed by an open paren), so prose in
# production sources must keep these words away from "(" (same discipline
# as the Task 011-013 gate). Tests under tests/ are intentionally NOT
# scanned: the loopback tests legitimately use client-side sockets.
SOCKET_ONLY='\<(socket|bind|listen|getsockname|setsockopt|inet_pton|htons|ntohs|ntohl|htonl|fcntl|close)\s*\('

LEAKS=$(grep -rEn --include='*.c' --include='*.h' "$SOCKET_ONLY" \
  "$NATIVE_DIR/src" "$NATIVE_DIR/include" | grep -v '/src/listener\.c:') || true
if [ -n "$LEAKS" ]; then
  echo "FAIL: socket/FD call outside src/listener.c (see match above)" >&2
  echo "$LEAKS" >&2
  FAIL=1
fi

# 2. Deferred layers: banned in every production source, including
# src/listener.c. Task 014 owns listener lifecycle only — no accept path,
# no event loop (epoll/poll/select/io_uring/kqueue), no payload I/O
# (send/recv/read/write/shutdown), no DNS/client resolution, no threads,
# no TLS.
BANNED_EVERYWHERE='\<(accept\w*|connect|poll|epoll_\w*|kqueue|kevent|io_uring|select|sendmsg|recvmsg|sendto|recvfrom|shutdown|send|recv|read|write|getaddrinfo|getnameinfo|pthread_\w*|SSL_\w*|TLS_\w*|mbedtls_\w*)\s*\('

HITS=$(grep -rEn --include='*.c' --include='*.h' "$BANNED_EVERYWHERE" \
  "$NATIVE_DIR/src" "$NATIVE_DIR/include") || true
if [ -n "$HITS" ]; then
  echo "FAIL: deferred-layer call in native sources (see match above)" >&2
  echo "$HITS" >&2
  FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
  exit 1
fi

echo "OK: networking confined to src/listener.c; no accept/loop/IO/thread/TLS calls"
