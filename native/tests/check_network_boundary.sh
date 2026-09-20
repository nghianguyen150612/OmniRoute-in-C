#!/bin/sh
# Task 023 network source-boundary gate: native networking exists, but ONLY
# in the deliberate networking layer (src/listener.c owns socket lifecycle,
# src/poller.c owns the readiness wait, src/accepted.c owns the accept4
# drain plus accepted-descriptor lifecycle, src/recv.c owns nonblocking
# socket receive into byte buffers, src/send.c owns nonblocking socket send
# with its required per-call SIGPIPE guard, src/connection.c owns the
# higher-level transfer/lifecycle composition without direct socket calls,
# and src/registry.c owns borrowed-pointer membership bookkeeping with no
# socket, readiness, payload, timer, thread, or HTTP machinery at all. The
# connection/reactor adapter in src/connection_reactor.c only composes those
# contracts and performs no direct descriptor or payload operation. The
# runtime coordinator in src/runtime.c only sequences those existing
# contracts and has no direct networking operation. The synchronous execution
# layer in src/event_loop.c only calls the existing reactor step and owns no
# descriptor or readiness syscall.
# Every other production module (arena, bytebuf, meminfo, main, all other
# headers and sources) must stay socket-free, and the deferred layers — output
# queues, generic payload read/write, DNS/client, threads, TLS, and protocols —
# stay banned from ALL production sources, including the networking layer
# itself except for the one authorized send call in src/send.c. The accept
# path is confined to src/accepted.c, the receive path to src/recv.c, the
# send path to src/send.c, connection composition to src/connection.c, and
# membership bookkeeping to src/registry.c, connection/reactor binding to
# src/connection_reactor.c, and loop orchestration to src/event_loop.c,
# exactly as the readiness wait is confined to src/poller.c. Later tasks extend
# this gate explicitly; they never loosen it silently.
#
# Tests under tests/ are intentionally NOT scanned: the loopback tests
# legitimately use client-side sockets and controlled test-client sends.
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
  "$NATIVE_DIR/src" "$NATIVE_DIR/include" | grep -v '/src/listener\.c:' | grep -v '/src/accepted\.c:' | grep -v '/src/connection\.c:') || true
if [ -n "$FD_LEAKS" ]; then
  echo "FAIL: descriptor-lifecycle call outside src/listener.c, src/accepted.c, and src/connection.c (see match above)" >&2
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

# 5. Receive-path token: allowed ONLY in src/recv.c. Plain recv() is the
# first authorized production payload input; scatter/gather, peeking, and
# datagram variants stay banned (see below), and no other production
# module may pull socket bytes.
RECV_ONLY='\<recv\s*\('

RECV_LEAKS=$(grep -rEn --include='*.c' --include='*.h' "$RECV_ONLY" \
  "$NATIVE_DIR/src" "$NATIVE_DIR/include" | grep -v '/src/recv\.c:') || true
if [ -n "$RECV_LEAKS" ]; then
  echo "FAIL: receive-path call outside src/recv.c (see match above)" >&2
  echo "$RECV_LEAKS" >&2
  FAIL=1
fi

# 6. Send-path token: allowed ONLY in src/send.c. Plain send() is the
# production output primitive; sendmsg/writev and every other scatter/gather
# or generic output family remains forbidden below.
SEND_ONLY='\<send\s*\('

SEND_LEAKS=$(grep -rEn --include='*.c' --include='*.h' "$SEND_ONLY" \
  "$NATIVE_DIR/src" "$NATIVE_DIR/include" | grep -v '/src/send\.c:') || true
if [ -n "$SEND_LEAKS" ]; then
  echo "FAIL: send-path call outside src/send.c (see match above)" >&2
  echo "$SEND_LEAKS" >&2
  FAIL=1
fi

SEND_CALLS=$(grep -Ec "$SEND_ONLY" "$NATIVE_DIR/src/send.c") || true
if [ "$SEND_CALLS" -ne 1 ]; then
  echo "FAIL: src/send.c must contain exactly one production send() call" >&2
  FAIL=1
fi

# 7. Receive-mode flags: accepted descriptors are already nonblocking, so
# production receive must remain an ordinary recv(fd, ptr, len, 0). Peeking,
# wait-all semantics, and per-call nonblocking overrides are forbidden. This
# is a token-level negative control in addition to the call-family checks.
FORBIDDEN_RECV_FLAGS='\<(MSG_PEEK|MSG_WAITALL|MSG_DONTWAIT)\>'

FLAG_HITS=$(grep -rEn --include='*.c' --include='*.h' "$FORBIDDEN_RECV_FLAGS" \
  "$NATIVE_DIR/src" "$NATIVE_DIR/include") || true
if [ -n "$FLAG_HITS" ]; then
  echo "FAIL: forbidden receive flag in native sources (see match above)" >&2
  echo "$FLAG_HITS" >&2
  FAIL=1
fi

# 8. Send signal safety: the authorized production call must carry the
# Linux-first MSG_NOSIGNAL mechanism. This is intentionally structural rather
# than a claim that shell matching replaces source review; the peer-close test
# below supplies the runtime survival proof.
if ! grep -Eq 'send[[:space:]]*\([^;]*MSG_NOSIGNAL' "$NATIVE_DIR/src/send.c"; then
  echo "FAIL: src/send.c does not visibly pass MSG_NOSIGNAL to send()" >&2
  FAIL=1
fi

# 9. Deferred layers: banned in every production source, including
# src/listener.c, src/poller.c, src/accepted.c, and src/recv.c. Task 017
# owns receive only and Task 018 owns one plain send — no alternate event-loop
# backend (epoll/select/io_uring/kqueue), no sendmsg, no generic payload read/write/shutdown, no
# scatter/gather or datagram receive variants (recvmsg/recvfrom), no
# DNS/client resolution, no threads, no TLS.
BANNED_EVERYWHERE='\<(connect|epoll_\w*|kqueue|kevent|io_uring|select|sendmsg|recvmsg|sendto|recvfrom|shutdown|read|write|getaddrinfo|getnameinfo|pthread_\w*|SSL_\w*|TLS_\w*|mbedtls_\w*)\s*\('

HITS=$(grep -rEn --include='*.c' --include='*.h' "$BANNED_EVERYWHERE" \
  "$NATIVE_DIR/src" "$NATIVE_DIR/include") || true
if [ -n "$HITS" ]; then
  echo "FAIL: deferred-layer call in native sources (see match above)" >&2
  echo "$HITS" >&2
  FAIL=1
fi

# Even in the authorized send module, scatter/gather and generic output
# calls remain forbidden. The global check above covers every other module.
SEND_FORBIDDEN='\<(sendmsg|writev|write|shutdown)\s*\('
SEND_FORBIDDEN_HITS=$(grep -nE "$SEND_FORBIDDEN" "$NATIVE_DIR/src/send.c") || true
if [ -n "$SEND_FORBIDDEN_HITS" ]; then
  echo "FAIL: forbidden generic/scatter output in src/send.c (see match above)" >&2
  echo "$SEND_FORBIDDEN_HITS" >&2
  FAIL=1
fi

# 10. Zero-heap rule for the accept, receive, send, connection, registry,
# connection/reactor adapter, runtime, and event-loop layers: bounded
# acceptance, bounded drain, single receive, single send, both bounded drains,
# lifecycle composition, membership bookkeeping, adapter binding, runtime
# coordination, and loop execution perform no heap allocation.
# Allocator tokens are banned outright (negative control for the
# Task 016/017/018/019/020/022 contracts).
NOHEAP_IN_ACCEPT='\<(malloc|calloc|realloc|free|mmap)\s*\('

HEAP_HITS=$(grep -nE "$NOHEAP_IN_ACCEPT" "$NATIVE_DIR/src/accepted.c" \
  "$NATIVE_DIR/src/recv.c" "$NATIVE_DIR/src/send.c" "$NATIVE_DIR/src/connection.c" \
  "$NATIVE_DIR/src/registry.c" "$NATIVE_DIR/src/connection_reactor.c" \
  "$NATIVE_DIR/src/runtime.c" "$NATIVE_DIR/src/event_loop.c") || true
if [ -n "$HEAP_HITS" ]; then
  echo "FAIL: heap-allocation call in accepted/recv/send/connection/registry/connection_reactor/runtime/event_loop production layer (see match above)" >&2
  echo "$HEAP_HITS" >&2
  FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
  exit 1
fi

echo "OK: networking confined to listener/poller/accepted/recv/send/connection/registry/connection_reactor production modules; runtime coordinates lifecycle; event_loop coordinates reactor execution; send uses MSG_NOSIGNAL; no alternate-loop/scatter/generic-IO/thread/TLS calls; no heap calls in accepted.c, recv.c, send.c, connection.c, registry.c, connection_reactor.c, runtime.c, or event_loop.c"
