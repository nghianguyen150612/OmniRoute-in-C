#!/bin/sh
# Task 016 network source-boundary gate: native networking exists, but ONLY
# in the deliberate networking layer (src/listener.c owns socket lifecycle,
# src/poller.c owns the readiness wait, src/accepted.c owns the accept4
# drain plus accepted-descriptor lifecycle). Every other production module
# (arena, bytebuf, meminfo, main, all other headers and sources) must stay
# socket-free, and the deferred layers — event loop, socket payload I/O,
# DNS/client, threads, TLS — stay banned from ALL production sources,
# including the networking layer itself. The accept path is confined to
# src/accepted.c exactly as the readiness wait is confined to
# src/poller.c. Later tasks extend this gate explicitly; they never loosen
# it silently.
#
# Usage: check_network_boundary.sh <native-dir>
set -u

NATIVE_DIR="${1:?usage: check_network_boundary.sh <native-dir>}"
FAIL=0

# 1. Socket-setup tokens: allowed ONLY in src/listener.c. Only the listener
# creates, binds, or queries listening sockets; the accept module borrows
# the listener descriptor and must never set up sockets itself. Matches
# are code-level call tokens (word followed by an open paren), so prose in
# production sources must keep these words away from "(" (same discipline
# as the Task 011-015 gate). Tests under tests/ are intentionally NOT
# scanned: the loopback tests legitimately use client-side sockets.
SOCKET_ONLY='\<(socket|bind|listen|getsockname|setsockopt|inet_pton|htons|ntohs|ntohl|htonl)\s*\('

LEAKS=$(grep -rEn --include='*.c' --include='*.h' "$SOCKET_ONLY" \
  "$NATIVE_DIR/src" "$NATIVE_DIR/include" | grep -v '/src/listener\.c:') || true
if [ -n "$LEAKS" ]; then
  echo "FAIL: socket-setup call outside src/listener.c (see match above)" >&2
  echo "$LEAKS" >&2
  FAIL=1
fi

# 2. Descriptor-lifecycle tokens: allowed ONLY in src/listener.c (listener
# ownership) and src/accepted.c (accepted-descriptor flag verification plus
# close-once destroy). No other production module may probe or release
# descriptors.
FD_ONLY='\<(fcntl|close)\s*\('

FD_LEAKS=$(grep -rEn --include='*.c' --include='*.h' "$FD_ONLY" \
  "$NATIVE_DIR/src" "$NATIVE_DIR/include" | grep -v '/src/listener\.c:' | grep -v '/src/accepted\.c:') || true
if [ -n "$FD_LEAKS" ]; then
  echo "FAIL: descriptor-lifecycle call outside src/listener.c and src/accepted.c (see match above)" >&2
  echo "$FD_LEAKS" >&2
  FAIL=1
fi

# 3. Readiness-wait token: allowed ONLY in src/poller.c. poll() is
# readiness observation, not socket creation, but it still belongs to one
# deliberate module — nowhere else in production.
POLL_ONLY='\<poll\s*\('

POLL_LEAKS=$(grep -rEn --include='*.c' --include='*.h' "$POLL_ONLY" \
  "$NATIVE_DIR/src" "$NATIVE_DIR/include" | grep -v '/src/poller\.c:') || true
if [ -n "$POLL_LEAKS" ]; then
  echo "FAIL: readiness wait outside src/poller.c (see match above)" >&2
  echo "$POLL_LEAKS" >&2
  FAIL=1
fi

# 4. Accept-path token: allowed ONLY in src/accepted.c. The accept4 drain
# is the newest deliberate layer; the listener never accepts (it only
# listens) and no other production module may produce client descriptors.
ACCEPT_ONLY='\<accept\w*\s*\('

ACCEPT_LEAKS=$(grep -rEn --include='*.c' --include='*.h' "$ACCEPT_ONLY" \
  "$NATIVE_DIR/src" "$NATIVE_DIR/include" | grep -v '/src/accepted\.c:') || true
if [ -n "$ACCEPT_LEAKS" ]; then
  echo "FAIL: accept-path call outside src/accepted.c (see match above)" >&2
  echo "$ACCEPT_LEAKS" >&2
  FAIL=1
fi

# 5. Deferred layers: banned in every production source, including
# src/listener.c, src/poller.c, and src/accepted.c. Task 016 owns
# acceptance only — no event loop (epoll/select/io_uring/kqueue), no
# payload I/O (send/recv/read/write/shutdown), no DNS/client resolution,
# no threads, no TLS.
BANNED_EVERYWHERE='\<(connect|epoll_\w*|kqueue|kevent|io_uring|select|sendmsg|recvmsg|sendto|recvfrom|shutdown|send|recv|read|write|getaddrinfo|getnameinfo|pthread_\w*|SSL_\w*|TLS_\w*|mbedtls_\w*)\s*\('

HITS=$(grep -rEn --include='*.c' --include='*.h' "$BANNED_EVERYWHERE" \
  "$NATIVE_DIR/src" "$NATIVE_DIR/include") || true
if [ -n "$HITS" ]; then
  echo "FAIL: deferred-layer call in native sources (see match above)" >&2
  echo "$HITS" >&2
  FAIL=1
fi

# 6. Zero-heap rule for the accept layer: single acceptance and bounded
# drain perform no heap allocation, so allocator tokens are banned in
# src/accepted.c outright (negative control for the Task 016 contract).
NOHEAP_IN_ACCEPT='\<(malloc|calloc|realloc|free|mmap)\s*\('

HEAP_HITS=$(grep -nE "$NOHEAP_IN_ACCEPT" "$NATIVE_DIR/src/accepted.c") || true
if [ -n "$HEAP_HITS" ]; then
  echo "FAIL: heap-allocation call in src/accepted.c (see match above)" >&2
  echo "$HEAP_HITS" >&2
  FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
  exit 1
fi

echo "OK: networking confined to src/listener.c + src/poller.c + src/accepted.c; accept path confined to src/accepted.c; no loop/IO/thread/TLS calls; no heap calls in src/accepted.c"
