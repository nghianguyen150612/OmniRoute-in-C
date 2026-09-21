/*
 * OmniRoute native backend — bounded connection runtime binding tests (Task 027).
 *
 * Validates the runtime binding layer that coordinates:
 *   event_loop + connection_reactor + connection_session
 * without protocol logic.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "omniroute/accepted.h"
#include "omniroute/bytebuf.h"
#include "omniroute/connection.h"
#include "omniroute/connection_reactor.h"
#include "omniroute/connection_runtime.h"
#include "omniroute/connection_session.h"
#include "omniroute/event_loop.h"
#include "omniroute/listener.h"
#include "omniroute/poller.h"
#include "omniroute/reactor.h"
#include "omniroute/registry.h"
#include "omniroute/runtime.h"

#define CONN_RECV_CAP 128u
#define SESS_RECV_CAP 128u
#define SESS_SEND_CAP 256u
#define REG_CAP 8u
#define REACTOR_CAP 8u
#define POLLER_CAP 8u
#define RT_CAP 4u

static int check_count = 0;
static int failure_count = 0;

static void check(bool cond, const char *name) {
  ++check_count;
  if (cond) printf("ok - %s\n", name);
  else { ++failure_count; printf("NOT OK - %s\n", name); }
}

/* ---------- helpers ---------- */

static bool start_listener(struct omni_listener *l, uint16_t *port_out) {
  struct omni_listener_result r;
  omni_listener_make_inert(l);
  r = omni_listener_init(l, "127.0.0.1", 0u);
  check(r.status == OMNI_LISTENER_OK, "listener binds for runtime test");
  if (r.status != OMNI_LISTENER_OK) return false;
  check(omni_listener_port(l) != OMNI_LISTENER_PORT_INVALID, "port valid");
  check(omni_listener_port(l) != 20128, "non-20128 port");
  if (port_out) *port_out = omni_listener_port(l);
  return true;
}

static int open_client(uint16_t port) {
  struct sockaddr_in peer;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  check(fd != -1, "client socket opens");
  if (fd == -1) return -1;
  memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port = htons(port);
  peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr*)&peer, sizeof(peer)) != 0) {
    check(false, "client connects");
    close(fd);
    return -1;
  }
  check(true, "client connects");
  return fd;
}

static bool accept_one(struct omni_listener *l, struct omni_accepted *a) {
  struct omni_accept_result r;
  omni_accepted_make_inert(a);
  r = omni_accept_once(l, a);
  check(r.status == OMNI_ACCEPT_OK && r.accepted == 1u, "accept_one succeeds");
  return r.status == OMNI_ACCEPT_OK;
}

static bool make_open_connection(struct omni_connection *c, unsigned char *storage, size_t cap, struct omni_accepted *a) {
  struct omni_connection_config cfg = {0};
  cfg.receive_storage = storage;
  cfg.receive_capacity = cap;
  cfg.poller_token = 0x1234u;
  cfg.poller_interests = OMNI_POLLER_INTEREST_READ;
  omni_connection_make_inert(c);
  if (omni_connection_init(c, &cfg).status != OMNI_CONNECTION_OK) return false;
  if (omni_connection_from_accepted(c, a).status != OMNI_CONNECTION_OK) return false;
  return true;
}

static void client_send_all(int fd, const unsigned char *data, size_t len) {
  size_t sent = 0u;
  while (sent < len) {
    ssize_t n = send(fd, data + sent, len - sent, 0);
    if (n > 0) sent += (size_t)n;
    else break;
  }
}

static void dummy_cb(struct omni_connection *c, uint64_t tok, uint32_t ev, void *ctx) {
  (void)c; (void)tok; (void)ev; (void)ctx;
}

/* bundle runtime deps */
struct test_runtime_env {
  struct omni_poller poller;
  struct pollfd pfds[POLLER_CAP];
  uint64_t ptokens[POLLER_CAP];
  struct omni_reactor reactor;
  struct omni_reactor_registration rregs[REACTOR_CAP];
  struct omni_poller_event revents[REACTOR_CAP];
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot rslots[REG_CAP];
  struct omni_connection_reactor adapter;
  struct omni_runtime runtime;
  struct omni_event_loop loop;
  struct omni_connection_runtime crt;
  struct omni_connection_runtime_entry entries[RT_CAP];
};

static bool init_env(struct test_runtime_env *env) {
  struct omni_connection_runtime_config ccfg = {0};
  memset(env, 0, sizeof(*env));

  if (omni_poller_init_borrowed(&env->poller, env->pfds, env->ptokens, POLLER_CAP).status != OMNI_POLLER_OK) return false;

  omni_reactor_make_inert(&env->reactor);
  if (omni_reactor_init(&env->reactor, &env->poller, env->rregs, env->revents, REACTOR_CAP).status != OMNI_REACTOR_OK) return false;

  omni_connection_registry_make_inert(&env->registry);
  if (omni_connection_registry_init(&env->registry, env->rslots, REG_CAP).status != OMNI_CONNECTION_REGISTRY_OK) return false;

  omni_connection_reactor_make_inert(&env->adapter);
  if (omni_connection_reactor_init(&env->adapter, &env->reactor, &env->registry, dummy_cb, NULL).status != OMNI_CONNECTION_REACTOR_OK) return false;

  omni_runtime_make_inert(&env->runtime);
  omni_event_loop_make_inert(&env->loop);

  omni_connection_runtime_make_inert(&env->crt);
  ccfg.registry=&env->registry;
  ccfg.adapter=&env->adapter;
  ccfg.event_loop=NULL;
  ccfg.entries=env->entries;
  ccfg.capacity=RT_CAP;
  if (omni_connection_runtime_init(&env->crt, &ccfg).status != OMNI_CONNECTION_RUNTIME_OK) return false;
  return true;
}

static void cleanup_env(struct test_runtime_env *env) {
  omni_connection_runtime_destroy(&env->crt);
  omni_event_loop_destroy(&env->loop);
  omni_runtime_destroy(&env->runtime);
  omni_connection_reactor_destroy(&env->adapter);
  omni_connection_registry_destroy(&env->registry);
  omni_reactor_destroy(&env->reactor);
  omni_poller_destroy(&env->poller);
}

/* ---------- tests ---------- */

static void test_inert_state(void) {
  struct omni_connection_runtime rt = {0};
  omni_connection_runtime_make_inert(NULL);
  check(true, "make_inert NULL safe");
  omni_connection_runtime_make_inert(&rt);
  check(rt.state == OMNI_CONNECTION_RUNTIME_NEW, "NEW after make_inert");
  check(omni_connection_runtime_state(&rt)==OMNI_CONNECTION_RUNTIME_NEW, "state NEW");
  check(omni_connection_runtime_count(&rt)==0u, "count 0 on NEW");
  check(omni_connection_runtime_capacity(&rt)==0u, "capacity 0 on NEW");
  check(!omni_connection_runtime_is_initialized(&rt), "not initialized on NEW");
  check(omni_connection_runtime_state(NULL)==OMNI_CONNECTION_RUNTIME_NEW, "state NULL->NEW");
  check(omni_connection_runtime_count(NULL)==0u, "count NULL 0");
  omni_connection_runtime_destroy(&rt);
  check(rt.state == OMNI_CONNECTION_RUNTIME_CLOSED, "destroy NEW -> CLOSED");
  check(omni_connection_runtime_count(&rt)==0u, "count 0 after destroy");
  omni_connection_runtime_destroy(NULL);
  check(true, "destroy NULL safe");
  omni_connection_runtime_destroy(&rt);
  check(rt.state==OMNI_CONNECTION_RUNTIME_CLOSED, "double destroy stays CLOSED");
  struct omni_connection_runtime other={0};
  other.state=OMNI_CONNECTION_RUNTIME_CLOSED;
  omni_connection_runtime_make_inert(&other);
  check(other.state==OMNI_CONNECTION_RUNTIME_NEW, "make_inert CLOSED->NEW");
}

static void test_valid_init(void) {
  struct test_runtime_env env;
  if (!init_env(&env)) { check(false,"env init for valid_init"); return; }
  check(env.crt.state==OMNI_CONNECTION_RUNTIME_INITIALIZED, "init -> INITIALIZED");
  check(omni_connection_runtime_is_initialized(&env.crt), "is_initialized true");
  check(omni_connection_runtime_capacity(&env.crt)==RT_CAP, "capacity RT_CAP");
  check(omni_connection_runtime_count(&env.crt)==0u, "count 0 after init");
  cleanup_env(&env);
  check(env.crt.state==OMNI_CONNECTION_RUNTIME_CLOSED, "destroy -> CLOSED");
}

static void test_invalid_init(void) {
  struct omni_connection_runtime rt = {0};
  struct omni_connection_registry reg = {0};
  struct omni_connection_registry_slot slots[2] = {0};
  struct omni_poller poller = {0};
  struct pollfd pf[2]; uint64_t pt[2];
  struct omni_reactor reactor = {0};
  struct omni_reactor_registration rr[2];
  struct omni_poller_event ev[2];
  struct omni_connection_reactor adapter = {0};
  struct omni_connection_runtime_entry entries[2] = {0};
  struct omni_connection_runtime_config cfg = {0};

  (void)omni_poller_init_borrowed(&poller, pf, pt, 2);
  omni_reactor_make_inert(&reactor);
  (void)omni_reactor_init(&reactor, &poller, rr, ev, 2);
  omni_connection_registry_make_inert(&reg);
  omni_connection_registry_init(&reg, slots, 2);
  omni_connection_reactor_make_inert(&adapter);
  omni_connection_reactor_init(&adapter, &reactor, &reg, dummy_cb, NULL);

  omni_connection_runtime_make_inert(&rt);
  cfg.registry=NULL; cfg.adapter=&adapter; cfg.entries=entries; cfg.capacity=2;
  check(omni_connection_runtime_init(&rt,&cfg).status==OMNI_CONNECTION_RUNTIME_ERR_INVALID, "init rejects NULL registry");
  check(rt.state==OMNI_CONNECTION_RUNTIME_NEW, "stays NEW on invalid registry");

  cfg.registry=&reg; cfg.adapter=NULL;
  check(omni_connection_runtime_init(&rt,&cfg).status==OMNI_CONNECTION_RUNTIME_ERR_INVALID, "rejects NULL adapter");

  cfg.adapter=&adapter; cfg.entries=NULL;
  check(omni_connection_runtime_init(&rt,&cfg).status==OMNI_CONNECTION_RUNTIME_ERR_INVALID, "rejects NULL entries");

  cfg.entries=entries; cfg.capacity=0u;
  check(omni_connection_runtime_init(&rt,&cfg).status==OMNI_CONNECTION_RUNTIME_ERR_INVALID, "rejects zero capacity");

  cfg.capacity=2u;
  check(omni_connection_runtime_init(&rt,&cfg).status==OMNI_CONNECTION_RUNTIME_OK, "valid init succeeds");
  check(omni_connection_runtime_init(&rt,&cfg).status==OMNI_CONNECTION_RUNTIME_ERR_STATE, "second init rejected");

  omni_connection_runtime_destroy(&rt);
  check(rt.state==OMNI_CONNECTION_RUNTIME_CLOSED, "destroy -> CLOSED");
  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
  omni_connection_registry_destroy(&reg);
  omni_connection_reactor_destroy(&adapter);
}

static void test_attach_detach_success(void) {
  struct omni_listener listener={0};
  struct omni_accepted accepted={0};
  struct omni_connection conn={0};
  unsigned char conn_storage[CONN_RECV_CAP];
  unsigned char sess_recv[SESS_RECV_CAP];
  unsigned char sess_send[SESS_SEND_CAP];
  uint16_t port=0; int client_fd=-1;
  struct test_runtime_env env;

  if (!init_env(&env)) { check(false,"init_env attach"); return; }
  if (!start_listener(&listener,&port)) { cleanup_env(&env); return; }
  client_fd=open_client(port);
  if (client_fd==-1) { cleanup_env(&env); omni_listener_destroy(&listener); return; }
  if (!accept_one(&listener,&accepted)) { cleanup_env(&env); close(client_fd); omni_listener_destroy(&listener); return; }
  if (!make_open_connection(&conn,conn_storage,CONN_RECV_CAP,&accepted)) { check(false,"make conn open"); cleanup_env(&env); close(client_fd); omni_listener_destroy(&listener); return; }

  struct omni_connection_runtime_attach_config acfg={0};
  acfg.connection=&conn; acfg.receive_storage=sess_recv; acfg.receive_capacity=SESS_RECV_CAP; acfg.send_storage=sess_send; acfg.send_capacity=SESS_SEND_CAP;
  struct omni_connection_runtime_result r = omni_connection_runtime_attach(&env.crt, &acfg);
  check(r.status==OMNI_CONNECTION_RUNTIME_OK, "attach success");
  check(r.token!=0u, "token non-zero");
  check(omni_connection_runtime_count(&env.crt)==1u, "count 1 after attach");
  check(env.crt.state==OMNI_CONNECTION_RUNTIME_ATTACHED, "state ATTACHED");
  check(omni_connection_runtime_capacity(&env.crt)==RT_CAP, "capacity RT_CAP");
  struct omni_connection_session *sess = omni_connection_runtime_find_session(&env.crt,&conn);
  check(sess!=NULL, "find_session returns non-NULL");
  check(omni_connection_session_is_open(sess), "session OPEN after attach");
  check(omni_connection_runtime_fd(&env.crt,&conn)>=0, "fd valid after attach");
  check(omni_connection_reactor_count(&env.adapter)==1u, "reactor count 1");
  check(omni_connection_registry_count(&env.registry)==1u, "registry count 1");

  /* Re-attach duplicate should fail */
  struct omni_connection_runtime_result dup = omni_connection_runtime_attach(&env.crt,&acfg);
  check(dup.status==OMNI_CONNECTION_RUNTIME_ERR_DUPLICATE, "duplicate attach rejected");
  check(omni_connection_runtime_count(&env.crt)==1u, "count unchanged after duplicate");

  /* Detach */
  r = omni_connection_runtime_detach(&env.crt,&conn);
  check(r.status==OMNI_CONNECTION_RUNTIME_OK, "detach success");
  check(omni_connection_runtime_count(&env.crt)==0u, "count 0 after detach");
  check(env.crt.state==OMNI_CONNECTION_RUNTIME_INITIALIZED, "state INITIALIZED after last detach");
  check(omni_connection_runtime_find_session(&env.crt,&conn)==NULL, "find_session NULL after detach");
  check(omni_connection_reactor_count(&env.adapter)==0u, "reactor 0 after detach");
  check(omni_connection_registry_count(&env.registry)==0u, "registry 0 after detach");
  check(omni_connection_is_live(&conn), "connection still live after detach");
  check(omni_connection_fd(&conn)>=0, "connection fd still valid after detach");

  /* Detach again not found */
  r = omni_connection_runtime_detach(&env.crt,&conn);
  check(r.status==OMNI_CONNECTION_RUNTIME_ERR_NOT_FOUND, "second detach NOT_FOUND");

  omni_connection_destroy(&conn);
  omni_accepted_destroy(&accepted);
  if (client_fd!=-1) close(client_fd);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_invalid_transitions(void) {
  struct test_runtime_env env;
  struct omni_connection conn={0};
  unsigned char conn_storage[CONN_RECV_CAP];
  unsigned char sess_recv[SESS_RECV_CAP];
  unsigned char sess_send[SESS_SEND_CAP];
  struct omni_listener listener={0};
  struct omni_accepted accepted={0};
  uint16_t port=0; int client_fd=-1;

  omni_connection_runtime_make_inert(&env.crt);
  /* attach from NEW should be STATE error */
  struct omni_connection_runtime_attach_config acfg={0};
  acfg.connection=&conn; acfg.receive_storage=sess_recv; acfg.receive_capacity=SESS_RECV_CAP; acfg.send_storage=sess_send; acfg.send_capacity=SESS_SEND_CAP;
  check(omni_connection_runtime_attach(&env.crt,&acfg).status==OMNI_CONNECTION_RUNTIME_ERR_STATE || omni_connection_runtime_attach(&env.crt,&acfg).status==OMNI_CONNECTION_RUNTIME_ERR_INVALID, "attach from NEW rejected");
  check(omni_connection_runtime_detach(&env.crt,&conn).status==OMNI_CONNECTION_RUNTIME_ERR_STATE || omni_connection_runtime_detach(&env.crt,&conn).status==OMNI_CONNECTION_RUNTIME_ERR_INVALID, "detach from NEW rejected");

  if (!init_env(&env)) return;
  /* valid init, try attach with bad connection */
  struct omni_connection bad_conn={0};
  omni_connection_make_inert(&bad_conn);
  acfg.connection=&bad_conn;
  check(omni_connection_runtime_attach(&env.crt,&acfg).status==OMNI_CONNECTION_RUNTIME_ERR_INVALID, "attach rejects INERT connection");
  acfg.connection=NULL;
  check(omni_connection_runtime_attach(&env.crt,&acfg).status==OMNI_CONNECTION_RUNTIME_ERR_INVALID, "attach rejects NULL connection");

  /* attach with zero capacities */
  if (!start_listener(&listener,&port)) { cleanup_env(&env); return; }
  client_fd=open_client(port);
  if (client_fd==-1) { cleanup_env(&env); omni_listener_destroy(&listener); return; }
  if (!accept_one(&listener,&accepted)) { cleanup_env(&env); close(client_fd); omni_listener_destroy(&listener); return; }
  if (!make_open_connection(&conn,conn_storage,CONN_RECV_CAP,&accepted)) { cleanup_env(&env); close(client_fd); omni_listener_destroy(&listener); return; }
  acfg.connection=&conn; acfg.receive_storage=NULL; acfg.receive_capacity=SESS_RECV_CAP; acfg.send_storage=sess_send; acfg.send_capacity=SESS_SEND_CAP;
  check(omni_connection_runtime_attach(&env.crt,&acfg).status==OMNI_CONNECTION_RUNTIME_ERR_INVALID, "attach rejects NULL recv storage");
  acfg.receive_storage=sess_recv; acfg.receive_capacity=0u;
  check(omni_connection_runtime_attach(&env.crt,&acfg).status==OMNI_CONNECTION_RUNTIME_ERR_INVALID, "attach rejects zero recv cap");
  acfg.receive_capacity=SESS_RECV_CAP; acfg.send_capacity=0u;
  check(omni_connection_runtime_attach(&env.crt,&acfg).status==OMNI_CONNECTION_RUNTIME_ERR_INVALID, "attach rejects zero send cap");
  acfg.send_capacity=SESS_SEND_CAP;

  /* valid attach to test full capacity */
  acfg.receive_storage=sess_recv; acfg.receive_capacity=SESS_RECV_CAP; acfg.send_storage=sess_send; acfg.send_capacity=SESS_SEND_CAP;
  struct omni_connection conn2, conn3, conn4, conn5;
  unsigned char cs2[CONN_RECV_CAP], cs3[CONN_RECV_CAP], cs4[CONN_RECV_CAP], cs5[CONN_RECV_CAP];
  struct omni_accepted a2, a3, a4, a5;
  int c2=-1,c3=-1,c4=-1,c5=-1;
  uint16_t p2=port;
  /* Need more connections to fill RT_CAP=4 */
  omni_connection_make_inert(&conn2); omni_connection_make_inert(&conn3); omni_connection_make_inert(&conn4); omni_connection_make_inert(&conn5);
  omni_accepted_make_inert(&a2); omni_accepted_make_inert(&a3); omni_accepted_make_inert(&a4); omni_accepted_make_inert(&a5);
  c2=open_client(p2); accept_one(&listener,&a2); make_open_connection(&conn2,cs2,CONN_RECV_CAP,&a2);
  c3=open_client(p2); accept_one(&listener,&a3); make_open_connection(&conn3,cs3,CONN_RECV_CAP,&a3);
  c4=open_client(p2); accept_one(&listener,&a4); make_open_connection(&conn4,cs4,CONN_RECV_CAP,&a4);
  c5=open_client(p2); accept_one(&listener,&a5); make_open_connection(&conn5,cs5,CONN_RECV_CAP,&a5);
  unsigned char sr2[SESS_RECV_CAP], ss2[SESS_SEND_CAP], sr3[SESS_RECV_CAP], ss3[SESS_SEND_CAP], sr4[SESS_RECV_CAP], ss4[SESS_SEND_CAP], sr5[SESS_RECV_CAP], ss5[SESS_SEND_CAP];
  struct omni_connection_runtime_attach_config ac2={&conn,sr2,SESS_RECV_CAP,ss2,SESS_SEND_CAP};
  ac2.connection=&conn; ac2.receive_storage=sess_recv; ac2.receive_capacity=SESS_RECV_CAP; ac2.send_storage=sess_send; ac2.send_capacity=SESS_SEND_CAP;
  check(omni_connection_runtime_attach(&env.crt,&ac2).status==OMNI_CONNECTION_RUNTIME_OK, "attach conn1 ok");
  struct omni_connection_runtime_attach_config acb2={&conn2,sr2,SESS_RECV_CAP,ss2,SESS_SEND_CAP};
  check(omni_connection_runtime_attach(&env.crt,&acb2).status==OMNI_CONNECTION_RUNTIME_OK, "attach conn2 ok");
  struct omni_connection_runtime_attach_config acb3={&conn3,sr3,SESS_RECV_CAP,ss3,SESS_SEND_CAP};
  check(omni_connection_runtime_attach(&env.crt,&acb3).status==OMNI_CONNECTION_RUNTIME_OK, "attach conn3 ok");
  struct omni_connection_runtime_attach_config acb4={&conn4,sr4,SESS_RECV_CAP,ss4,SESS_SEND_CAP};
  check(omni_connection_runtime_attach(&env.crt,&acb4).status==OMNI_CONNECTION_RUNTIME_OK, "attach conn4 ok (full)");
  check(omni_connection_runtime_count(&env.crt)==RT_CAP, "count == capacity after fills");
  struct omni_connection_runtime_attach_config acb5={&conn5,sr5,SESS_RECV_CAP,ss5,SESS_SEND_CAP};
  check(omni_connection_runtime_attach(&env.crt,&acb5).status==OMNI_CONNECTION_RUNTIME_ERR_FULL, "attach beyond capacity FULL");

  /* cleanup all attaches */
  omni_connection_runtime_detach(&env.crt,&conn);
  omni_connection_runtime_detach(&env.crt,&conn2);
  omni_connection_runtime_detach(&env.crt,&conn3);
  omni_connection_runtime_detach(&env.crt,&conn4);
  check(omni_connection_runtime_count(&env.crt)==0u, "all detached count 0");

  omni_connection_destroy(&conn); omni_connection_destroy(&conn2); omni_connection_destroy(&conn3); omni_connection_destroy(&conn4); omni_connection_destroy(&conn5);
  omni_accepted_destroy(&accepted); omni_accepted_destroy(&a2); omni_accepted_destroy(&a3); omni_accepted_destroy(&a4); omni_accepted_destroy(&a5);
  if (c2!=-1) close(c2);
  if (c3!=-1) close(c3);
  if (c4!=-1) close(c4);
  if (c5!=-1) close(c5);
  if (client_fd!=-1) close(client_fd);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_stale_registration(void) {
  struct test_runtime_env env;
  struct omni_listener listener={0};
  struct omni_accepted a1={0}, a2={0};
  struct omni_connection c1={0}, c2={0};
  unsigned char cs1[CONN_RECV_CAP], cs2[CONN_RECV_CAP];
  unsigned char sr1[SESS_RECV_CAP], ss1[SESS_SEND_CAP];
  unsigned char sr2[SESS_RECV_CAP], ss2[SESS_SEND_CAP];
  uint16_t port=0; int cf1=-1, cf2=-1;
  if (!init_env(&env)) return;
  if (!start_listener(&listener,&port)) { cleanup_env(&env); return; }
  cf1=open_client(port); accept_one(&listener,&a1); make_open_connection(&c1,cs1,CONN_RECV_CAP,&a1);
  struct omni_connection_runtime_attach_config ac1={&c1,sr1,SESS_RECV_CAP,ss1,SESS_SEND_CAP};
  struct omni_connection_runtime_result r1 = omni_connection_runtime_attach(&env.crt,&ac1);
  check(r1.status==OMNI_CONNECTION_RUNTIME_OK, "stale test attach c1");
  uint64_t tok1=r1.token;
  check(tok1!=0u, "token1 non-zero");

  /* detach c1, token should become stale */
  omni_connection_runtime_detach(&env.crt,&c1);
  /* dispatch old token via adapter directly should be IGNORED */
  struct omni_connection_reactor_result dr = omni_connection_reactor_dispatch(&env.adapter, tok1, OMNI_POLLER_INTEREST_READ);
  check(dr.status==OMNI_CONNECTION_REACTOR_IGNORED, "stale token ignored after detach");

  /* re-attach new connection, should get different token (generation changed) */
  cf2=open_client(port); accept_one(&listener,&a2); make_open_connection(&c2,cs2,CONN_RECV_CAP,&a2);
  struct omni_connection_runtime_attach_config ac2={&c2,sr2,SESS_RECV_CAP,ss2,SESS_SEND_CAP};
  struct omni_connection_runtime_result r2 = omni_connection_runtime_attach(&env.crt,&ac2);
  check(r2.status==OMNI_CONNECTION_RUNTIME_OK, "attach c2 after stale");
  check(r2.token!=tok1, "new token != stale token");

  omni_connection_runtime_detach(&env.crt,&c2);
  omni_connection_destroy(&c1); omni_connection_destroy(&c2);
  omni_accepted_destroy(&a1); omni_accepted_destroy(&a2);
  if (cf1!=-1) close(cf1);
  if (cf2!=-1) close(cf2);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_ownership_preservation(void) {
  struct test_runtime_env env;
  struct omni_listener listener={0};
  struct omni_accepted accepted={0};
  struct omni_connection conn={0};
  unsigned char cs[CONN_RECV_CAP];
  unsigned char sr[SESS_RECV_CAP], ss[SESS_SEND_CAP];
  uint16_t port=0; int cf=-1;
  if (!init_env(&env)) return;
  if (!start_listener(&listener,&port)) { cleanup_env(&env); return; }
  int lfd_before=omni_listener_fd(&listener);
  cf=open_client(port); accept_one(&listener,&accepted); make_open_connection(&conn,cs,CONN_RECV_CAP,&accepted);
  int cfd_before=omni_connection_fd(&conn);
  struct omni_connection_runtime_attach_config ac={&conn,sr,SESS_RECV_CAP,ss,SESS_SEND_CAP};
  omni_connection_runtime_attach(&env.crt,&ac);
  check(omni_listener_fd(&listener)==lfd_before, "listener fd preserved after attach");
  check(omni_connection_fd(&conn)==cfd_before, "conn fd preserved after attach");

  omni_connection_runtime_detach(&env.crt,&conn);
  check(omni_listener_fd(&listener)==lfd_before, "listener fd preserved after detach");
  check(omni_connection_fd(&conn)==cfd_before, "conn fd preserved after detach");
  check(omni_connection_is_live(&conn), "conn still live after detach");

  omni_connection_runtime_destroy(&env.crt);
  check(omni_listener_fd(&listener)==lfd_before, "listener fd preserved after destroy");
  check(omni_connection_fd(&conn)==cfd_before, "conn fd preserved after runtime destroy");
  check(omni_connection_is_live(&conn), "conn live after runtime destroy");

  omni_connection_destroy(&conn);
  check(omni_connection_fd(&conn)==OMNI_ACCEPTED_FD_INVALID, "conn fd invalid after conn destroy");
  omni_accepted_destroy(&accepted);
  if (cf!=-1) close(cf);
  check(omni_listener_fd(&listener)==lfd_before, "listener still valid after all");
  omni_listener_destroy(&listener);
  check(omni_listener_fd(&listener)==OMNI_ACCEPTED_FD_INVALID, "listener fd invalid after destroy");
  cleanup_env(&env);
}

static void test_session_lifecycle_integration(void) {
  struct test_runtime_env env;
  struct omni_listener listener={0};
  struct omni_accepted accepted={0};
  struct omni_connection conn={0};
  unsigned char cs[CONN_RECV_CAP];
  unsigned char sr[SESS_RECV_CAP], ss[SESS_SEND_CAP];
  uint16_t port=0; int cf=-1;
  if (!init_env(&env)) return;
  if (!start_listener(&listener,&port)) { cleanup_env(&env); return; }
  cf=open_client(port); accept_one(&listener,&accepted); make_open_connection(&conn,cs,CONN_RECV_CAP,&accepted);
  struct omni_connection_runtime_attach_config ac={&conn,sr,SESS_RECV_CAP,ss,SESS_SEND_CAP};
  omni_connection_runtime_attach(&env.crt,&ac);
  struct omni_connection_session *sess = omni_connection_runtime_find_session(&env.crt,&conn);
  check(sess!=NULL, "session found after attach");
  check(omni_connection_session_is_open(sess), "session OPEN");
  check(omni_connection_session_io_state(sess)==OMNI_CONNECTION_IO_OPEN, "io OPEN");
  check(omni_connection_session_fd(sess)==omni_connection_fd(&conn), "session fd matches conn fd");
  check(omni_bytebuf_capacity(omni_connection_session_receive_buffer(sess))==SESS_RECV_CAP, "recv cap correct");
  check(omni_bytebuf_capacity(omni_connection_session_send_buffer(sess))==SESS_SEND_CAP, "send cap correct");
  /* readable/writable via session should work */
  client_send_all(cf, (unsigned char*)"hi", 2);
  struct omni_connection_io_result rr = omni_connection_session_readable(sess);
  check(rr.status==OMNI_CONNECTION_IO_OK && rr.count==2u, "readable via session ok");
  struct omni_bytebuf *sb = omni_connection_session_send_buffer(sess);
  check(sb!=NULL, "send buffer via session");
  check(omni_bytebuf_append(sb,(unsigned char*)"bye",3), "append ok");
  struct omni_connection_io_result wr = omni_connection_session_writable(sess);
  check(wr.status==OMNI_CONNECTION_IO_OK, "writable via session ok");
  /* detach should destroy session */
  omni_connection_runtime_detach(&env.crt,&conn);
  check(omni_connection_runtime_find_session(&env.crt,&conn)==NULL, "session gone after detach");
  /* Ensure session buffers are not accessible via old pointer (already destroyed) */
  omni_connection_destroy(&conn);
  omni_accepted_destroy(&accepted);
  if (cf!=-1) close(cf);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_reactor_cleanup(void) {
  struct test_runtime_env env;
  struct omni_listener listener={0};
  struct omni_accepted a={0};
  struct omni_connection c={0};
  unsigned char cs[CONN_RECV_CAP];
  unsigned char sr[SESS_RECV_CAP], ss[SESS_SEND_CAP];
  uint16_t port=0; int cf=-1;
  if (!init_env(&env)) return;
  if (!start_listener(&listener,&port)) { cleanup_env(&env); return; }
  cf=open_client(port); accept_one(&listener,&a); make_open_connection(&c,cs,CONN_RECV_CAP,&a);
  struct omni_connection_runtime_attach_config ac={&c,sr,SESS_RECV_CAP,ss,SESS_SEND_CAP};
  omni_connection_runtime_attach(&env.crt,&ac);
  check(omni_connection_reactor_count(&env.adapter)==1u, "reactor 1 after attach");
  omni_connection_runtime_destroy(&env.crt);
  check(omni_connection_reactor_count(&env.adapter)==0u, "reactor 0 after runtime destroy");
  check(omni_connection_registry_count(&env.registry)==0u, "registry 0 after runtime destroy");
  check(omni_connection_is_live(&c), "conn still live after runtime destroy (no close)");
  omni_connection_destroy(&c);
  omni_accepted_destroy(&a);
  if (cf!=-1) close(cf);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_repeated_cycles(void) {
  struct test_runtime_env env;
  struct omni_listener listener={0};
  uint16_t port=0;
  if (!init_env(&env)) return;
  if (!start_listener(&listener,&port)) { cleanup_env(&env); return; }
  int i;
  for (i=0;i<20;++i) {
    struct omni_accepted a={0};
    struct omni_connection c={0};
    unsigned char cs[CONN_RECV_CAP];
    unsigned char sr[SESS_RECV_CAP], ss[SESS_SEND_CAP];
    int cf=-1;
    omni_accepted_make_inert(&a); omni_connection_make_inert(&c);
    cf=open_client(port);
    if (cf==-1) break;
    if (!accept_one(&listener,&a)) { close(cf); break; }
    if (!make_open_connection(&c,cs,CONN_RECV_CAP,&a)) { omni_accepted_destroy(&a); close(cf); break; }
    struct omni_connection_runtime_attach_config ac={&c,sr,SESS_RECV_CAP,ss,SESS_SEND_CAP};
    struct omni_connection_runtime_result rr = omni_connection_runtime_attach(&env.crt,&ac);
    check(rr.status==OMNI_CONNECTION_RUNTIME_OK, "cycle attach ok");
    /* do a quick readable/writable */
    client_send_all(cf,(unsigned char*)"x",1);
    struct omni_connection_session *sess = omni_connection_runtime_find_session(&env.crt,&c);
    if (sess) { (void)omni_connection_session_readable(sess); struct omni_bytebuf *sb=omni_connection_session_send_buffer(sess); if (sb) { omni_bytebuf_append(sb,(unsigned char*)"y",1); (void)omni_connection_session_writable(sess); } }
    rr = omni_connection_runtime_detach(&env.crt,&c);
    check(rr.status==OMNI_CONNECTION_RUNTIME_OK, "cycle detach ok");
    omni_connection_destroy(&c);
    omni_accepted_destroy(&a);
    close(cf);
  }
  check(i==20, "20 repeated attach/detach cycles");
  check(omni_connection_runtime_count(&env.crt)==0u, "count 0 after cycles");
  check(omni_connection_reactor_count(&env.adapter)==0u, "reactor 0 after cycles");
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_fd_census(void) {
  struct test_runtime_env env;
  struct omni_listener listener={0};
  struct omni_accepted a={0};
  struct omni_connection c={0};
  unsigned char cs[CONN_RECV_CAP];
  unsigned char sr[SESS_RECV_CAP], ss[SESS_SEND_CAP];
  uint16_t port=0; int cf=-1;
  int bystander=-1;
  if (!init_env(&env)) return;
  if (!start_listener(&listener,&port)) { cleanup_env(&env); return; }
  bystander=socket(AF_INET,SOCK_STREAM,0);
  check(bystander!=-1, "bystander fd opens");
  cf=open_client(port); accept_one(&listener,&a); make_open_connection(&c,cs,CONN_RECV_CAP,&a);
  struct omni_connection_runtime_attach_config ac={&c,sr,SESS_RECV_CAP,ss,SESS_SEND_CAP};
  omni_connection_runtime_attach(&env.crt,&ac);
  omni_connection_runtime_detach(&env.crt,&c);
  omni_connection_runtime_destroy(&env.crt);
  check(close(bystander)==0, "bystander survives runtime lifecycle");
  omni_connection_destroy(&c);
  omni_accepted_destroy(&a);
  if (cf!=-1) close(cf);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_memory_size(void) {
  struct omni_connection_runtime rt={0};
  printf("# sizeof(runtime)=%zu # sizeof(entry)=%zu # sizeof(session)=%zu\n", sizeof(rt), sizeof(struct omni_connection_runtime_entry), sizeof(struct omni_connection_session));
  check(sizeof(rt)>0 && sizeof(rt)<1024, "runtime size bounded <1K");
  check(sizeof(struct omni_connection_runtime_entry) >= sizeof(struct omni_connection_session), "entry contains session");
}

int main(void) {
  test_inert_state();
  test_valid_init();
  test_invalid_init();
  test_attach_detach_success();
  test_invalid_transitions();
  test_stale_registration();
  test_ownership_preservation();
  test_session_lifecycle_integration();
  test_reactor_cleanup();
  test_repeated_cycles();
  test_fd_census();
  test_memory_size();
  printf("---\n%d checks, %d failures\n", check_count, failure_count);
  return failure_count==0?0:1;
}
