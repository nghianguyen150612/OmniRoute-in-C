/*
 * OmniRoute native backend — bounded connection manager tests (Task 028).
 *
 * Validates the bounded manager that tracks a fixed collection of active
 * connection_runtime attachments without protocol handling, heap allocation,
 * or descriptor ownership.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
#include "omniroute/connection_manager.h"
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
#define MGR_CAP 4u
#define MANAGER_STRESS_CYCLES 500u
#define MULTI_ACTIVE_CAP 3u

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
  check(r.status == OMNI_LISTENER_OK, "listener binds for manager test");
  if (r.status != OMNI_LISTENER_OK) return false;
  check(omni_listener_port(l) != OMNI_LISTENER_PORT_INVALID, "port valid manager");
  check(omni_listener_port(l) != 20128, "non-20128 port manager");
  if (port_out) *port_out = omni_listener_port(l);
  return true;
}

static int open_client(uint16_t port) {
  struct sockaddr_in peer;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  check(fd != -1, "client socket opens manager");
  if (fd == -1) return -1;
  memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port = htons(port);
  peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr*)&peer, sizeof(peer)) != 0) {
    check(false, "client connects manager");
    close(fd);
    return -1;
  }
  check(true, "client connects manager");
  return fd;
}

static bool accept_one(struct omni_listener *l, struct omni_accepted *a) {
  struct omni_accept_result r;
  omni_accepted_make_inert(a);
  r = omni_accept_once(l, a);
  check(r.status == OMNI_ACCEPT_OK && r.accepted == 1u, "accept_one manager succeeds");
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

/* bundle env for manager tests */
struct test_manager_env {
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
  struct omni_listener loop_listener;
  struct omni_connection_registry loop_registry;
  struct omni_reactor loop_reactor;
  struct pollfd loop_pfds[POLLER_CAP];
  uint64_t loop_ptokens[POLLER_CAP];
  struct omni_connection_registry_slot loop_rslots[REG_CAP];
  struct omni_reactor_registration loop_rregs[REACTOR_CAP];
  struct omni_poller_event loop_events[REACTOR_CAP];
  struct omni_connection_runtime crt;
  struct omni_connection_runtime_entry rt_entries[RT_CAP];
  struct omni_connection_manager mgr;
  struct omni_connection_manager_entry mgr_entries[MGR_CAP];
};

static bool init_env_internal(struct test_manager_env *env, bool with_event_loop) {
  struct omni_runtime_config runtime_cfg = {0};
  struct omni_event_loop_config loop_cfg = {0};
  struct omni_connection_runtime_config rcfg = {0};
  struct omni_connection_manager_config mcfg = {0};
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
  if (with_event_loop) {
    omni_listener_make_inert(&env->loop_listener);
    omni_connection_registry_make_inert(&env->loop_registry);
    omni_reactor_make_inert(&env->loop_reactor);
    runtime_cfg.listener = &env->loop_listener;
    runtime_cfg.registry = &env->loop_registry;
    runtime_cfg.reactor = &env->loop_reactor;
    runtime_cfg.registry_slots = env->loop_rslots;
    runtime_cfg.registry_capacity = REG_CAP;
    runtime_cfg.poller_fds = env->loop_pfds;
    runtime_cfg.poller_tokens = env->loop_ptokens;
    runtime_cfg.poller_capacity = POLLER_CAP;
    runtime_cfg.reactor_registrations = env->loop_rregs;
    runtime_cfg.reactor_events = env->loop_events;
    runtime_cfg.reactor_capacity = REACTOR_CAP;
    runtime_cfg.listener_address = "127.0.0.1";
    runtime_cfg.listener_port = 0u;
    if (omni_runtime_init(&env->runtime, &runtime_cfg).status != OMNI_RUNTIME_OK) return false;
    loop_cfg.runtime = &env->runtime;
    loop_cfg.timeout_ms = 0;
    if (omni_event_loop_init(&env->loop, &loop_cfg).status != OMNI_EVENT_LOOP_OK) return false;
  }

  omni_connection_runtime_make_inert(&env->crt);
  rcfg.registry=&env->registry;
  rcfg.adapter=&env->adapter;
  rcfg.event_loop=with_event_loop ? &env->loop : NULL;
  rcfg.entries=env->rt_entries;
  rcfg.capacity=RT_CAP;
  if (omni_connection_runtime_init(&env->crt, &rcfg).status != OMNI_CONNECTION_RUNTIME_OK) return false;

  omni_connection_manager_make_inert(&env->mgr);
  mcfg.runtime=&env->crt;
  mcfg.entries=env->mgr_entries;
  mcfg.capacity=MGR_CAP;
  if (omni_connection_manager_init(&env->mgr, &mcfg).status != OMNI_CONNECTION_MANAGER_OK) return false;
  return true;
}

static bool init_env(struct test_manager_env *env) {
  return init_env_internal(env, false);
}

static bool init_env_with_event_loop(struct test_manager_env *env) {
  return init_env_internal(env, true);
}

static void cleanup_env(struct test_manager_env *env) {
  omni_connection_manager_destroy(&env->mgr);
  omni_connection_runtime_destroy(&env->crt);
  omni_event_loop_destroy(&env->loop);
  omni_runtime_destroy(&env->runtime);
  omni_connection_reactor_destroy(&env->adapter);
  omni_connection_registry_destroy(&env->registry);
  omni_reactor_destroy(&env->reactor);
  omni_poller_destroy(&env->poller);
}

static bool fd_is_open(int fd) {
  if (fd < 0) return false;
  errno = 0;
  return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

/* Linux test-only process-wide descriptor census; subtract opendir's own FD. */
static int fd_census(void) {
  DIR *directory = opendir("/proc/self/fd");
  struct dirent *entry = NULL;
  int count = 0;
  if (directory == NULL) return -1;
  while ((entry = readdir(directory)) != NULL) {
    if (entry->d_name[0] != '.') count += 1;
  }
  if (closedir(directory) != 0 || count == 0) return -1;
  return count - 1;
}

/* ---------- tests ---------- */

static void test_inert_state(void) {
  struct omni_connection_manager mgr = {0};
  omni_connection_manager_make_inert(NULL);
  check(true, "make_inert NULL safe manager");
  omni_connection_manager_make_inert(&mgr);
  check(mgr.state == OMNI_CONNECTION_MANAGER_NEW, "NEW after make_inert manager");
  check(omni_connection_manager_state(&mgr)==OMNI_CONNECTION_MANAGER_NEW, "state NEW manager");
  check(omni_connection_manager_count(&mgr)==0u, "count 0 on NEW manager");
  check(omni_connection_manager_capacity(&mgr)==0u, "capacity 0 on NEW manager");
  check(omni_connection_manager_state(NULL)==OMNI_CONNECTION_MANAGER_NEW, "state NULL->NEW manager");
  check(omni_connection_manager_count(NULL)==0u, "count NULL 0 manager");
  omni_connection_manager_destroy(&mgr);
  check(mgr.state == OMNI_CONNECTION_MANAGER_CLOSED, "destroy NEW -> CLOSED manager");
  check(omni_connection_manager_count(&mgr)==0u, "count 0 after destroy manager");
  omni_connection_manager_destroy(NULL);
  check(true, "destroy NULL safe manager");
  omni_connection_manager_destroy(&mgr);
  check(mgr.state==OMNI_CONNECTION_MANAGER_CLOSED, "double destroy stays CLOSED manager");
  struct omni_connection_manager other={0};
  other.state=OMNI_CONNECTION_MANAGER_CLOSED;
  omni_connection_manager_make_inert(&other);
  check(other.state==OMNI_CONNECTION_MANAGER_NEW, "make_inert CLOSED->NEW manager");
}

static void test_valid_init(void) {
  struct test_manager_env env;
  if (!init_env(&env)) { check(false,"env init for valid_init manager"); return; }
  check(omni_connection_manager_state(&env.mgr) == OMNI_CONNECTION_MANAGER_INITIALIZED,
        "manager state reports initialized");
  check(omni_connection_manager_capacity(&env.mgr)==MGR_CAP, "manager capacity MGR_CAP");
  check(omni_connection_manager_count(&env.mgr)==0u, "manager count 0 after init");
  struct omni_connection missing = {0};
  size_t count_before_find = omni_connection_manager_count(&env.mgr);
  enum omni_connection_manager_state state_before_find = omni_connection_manager_state(&env.mgr);
  check(omni_connection_manager_find(&env.mgr, NULL) == NULL, "find NULL connection returns NULL");
  check(omni_connection_manager_find(NULL, &missing) == NULL, "find NULL manager returns NULL");
  check(omni_connection_manager_find(&env.mgr, &missing) == NULL, "find missing connection returns NULL");
  check(omni_connection_manager_count(&env.mgr) == count_before_find,
        "find does not change manager count");
  check(omni_connection_manager_state(&env.mgr) == state_before_find,
        "find does not change manager state");
  /* start transition */
  struct omni_connection_manager_result sr = omni_connection_manager_start(&env.mgr);
  check(sr.status==OMNI_CONNECTION_MANAGER_OK, "start INITIALIZED->RUNNING");
  check(omni_connection_manager_state(&env.mgr) == OMNI_CONNECTION_MANAGER_RUNNING,
        "manager state reports running");
  /* stop */
  sr = omni_connection_manager_stop(&env.mgr);
  check(sr.status==OMNI_CONNECTION_MANAGER_OK, "stop RUNNING->STOPPING");
  check(env.mgr.state==OMNI_CONNECTION_MANAGER_STOPPING, "state STOPPING after stop");
  /* destroy */
  cleanup_env(&env);
  check(env.mgr.state==OMNI_CONNECTION_MANAGER_CLOSED, "manager destroy -> CLOSED");
}

static void test_invalid_init(void) {
  struct omni_connection_manager mgr = {0};
  struct omni_connection_registry reg = {0};
  struct omni_connection_registry_slot slots[2] = {0};
  struct omni_poller poller = {0};
  struct pollfd pf[2]; uint64_t pt[2];
  struct omni_reactor reactor = {0};
  struct omni_reactor_registration rr[2];
  struct omni_poller_event ev[2];
  struct omni_connection_reactor adapter = {0};
  struct omni_connection_runtime crt = {0};
  struct omni_connection_runtime_entry rt_entries[2] = {0};
  struct omni_connection_manager_entry entries[2] = {0};
  struct omni_connection_manager_config cfg = {0};
  struct omni_connection_runtime_config rcfg = {0};

  (void)omni_poller_init_borrowed(&poller, pf, pt, 2);
  omni_reactor_make_inert(&reactor);
  (void)omni_reactor_init(&reactor, &poller, rr, ev, 2);
  omni_connection_registry_make_inert(&reg);
  omni_connection_registry_init(&reg, slots, 2);
  omni_connection_reactor_make_inert(&adapter);
  omni_connection_reactor_init(&adapter, &reactor, &reg, dummy_cb, NULL);
  omni_connection_runtime_make_inert(&crt);
  rcfg.registry=&reg; rcfg.adapter=&adapter; rcfg.event_loop=NULL; rcfg.entries=rt_entries; rcfg.capacity=2;
  omni_connection_runtime_init(&crt, &rcfg);

  omni_connection_manager_make_inert(&mgr);
  cfg.runtime=NULL; cfg.entries=entries; cfg.capacity=2;
  check(omni_connection_manager_init(&mgr,&cfg).status==OMNI_CONNECTION_MANAGER_ERR_INVALID, "manager init rejects NULL runtime");
  check(mgr.state==OMNI_CONNECTION_MANAGER_NEW, "stays NEW on invalid runtime manager");

  cfg.runtime=&crt; cfg.entries=NULL;
  check(omni_connection_manager_init(&mgr,&cfg).status==OMNI_CONNECTION_MANAGER_ERR_INVALID, "manager rejects NULL entries");

  cfg.entries=entries; cfg.capacity=0u;
  check(omni_connection_manager_init(&mgr,&cfg).status==OMNI_CONNECTION_MANAGER_ERR_INVALID, "manager rejects zero capacity");

  cfg.capacity=2u;
  check(omni_connection_manager_init(&mgr,&cfg).status==OMNI_CONNECTION_MANAGER_OK, "manager valid init succeeds");
  check(omni_connection_manager_init(&mgr,&cfg).status==OMNI_CONNECTION_MANAGER_ERR_STATE, "second manager init rejected");

  /* invalid runtime: NEW state */
  struct omni_connection_manager mgr2={0};
  struct omni_connection_runtime bad_rt={0};
  omni_connection_runtime_make_inert(&bad_rt);
  omni_connection_manager_make_inert(&mgr2);
  struct omni_connection_manager_config bad_cfg={&bad_rt, entries, 2};
  check(omni_connection_manager_init(&mgr2,&bad_cfg).status==OMNI_CONNECTION_MANAGER_ERR_INVALID, "manager rejects non-live runtime");

  /* no operations before init */
  struct omni_connection_manager fresh={0};
  omni_connection_manager_make_inert(&fresh);
  struct omni_connection_manager_attach_config acfg={0};
  unsigned char sr[SESS_RECV_CAP], ss[SESS_SEND_CAP];
  struct omni_connection bad_conn={0};
  acfg.connection=&bad_conn; acfg.receive_storage=sr; acfg.receive_capacity=SESS_RECV_CAP; acfg.send_storage=ss; acfg.send_capacity=SESS_SEND_CAP;
  check(omni_connection_manager_add(&fresh,&acfg).status==OMNI_CONNECTION_MANAGER_ERR_STATE, "add before init rejected");
  check(omni_connection_manager_remove(&fresh,&bad_conn).status==OMNI_CONNECTION_MANAGER_ERR_STATE, "remove before init rejected");
  check(omni_connection_manager_start(&fresh).status==OMNI_CONNECTION_MANAGER_ERR_STATE, "start before init rejected");
  check(omni_connection_manager_stop(&fresh).status==OMNI_CONNECTION_MANAGER_ERR_STATE, "stop before init rejected");

  omni_connection_manager_destroy(&mgr);
  check(mgr.state==OMNI_CONNECTION_MANAGER_CLOSED, "manager destroy -> CLOSED invalid init");
  omni_connection_manager_destroy(&mgr2);
  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
  omni_connection_registry_destroy(&reg);
  omni_connection_reactor_destroy(&adapter);
  omni_connection_runtime_destroy(&crt);
}

static void test_add_remove_success(void) {
  struct omni_listener listener={0};
  struct omni_accepted accepted={0};
  struct omni_connection conn={0};
  unsigned char conn_storage[CONN_RECV_CAP];
  unsigned char sess_recv[SESS_RECV_CAP];
  unsigned char sess_send[SESS_SEND_CAP];
  uint16_t port=0; int client_fd=-1;
  struct test_manager_env env;

  if (!init_env(&env)) { check(false,"init_env add_remove manager"); return; }
  if (!start_listener(&listener,&port)) { cleanup_env(&env); return; }
  client_fd=open_client(port);
  if (client_fd==-1) { cleanup_env(&env); omni_listener_destroy(&listener); return; }
  if (!accept_one(&listener,&accepted)) { cleanup_env(&env); close(client_fd); omni_listener_destroy(&listener); return; }
  if (!make_open_connection(&conn,conn_storage,CONN_RECV_CAP,&accepted)) { check(false,"make conn open manager"); cleanup_env(&env); close(client_fd); omni_listener_destroy(&listener); return; }

  struct omni_connection_manager_attach_config acfg={0};
  acfg.connection=&conn; acfg.receive_storage=sess_recv; acfg.receive_capacity=SESS_RECV_CAP; acfg.send_storage=sess_send; acfg.send_capacity=SESS_SEND_CAP;
  struct omni_connection_manager_result r = omni_connection_manager_add(&env.mgr, &acfg);
  check(r.status==OMNI_CONNECTION_MANAGER_OK, "manager add success");
  check(r.token!=0u, "manager token non-zero");
  check(omni_connection_manager_count(&env.mgr)==1u, "manager count 1 after add");
  check(env.mgr.state==OMNI_CONNECTION_MANAGER_RUNNING, "manager state RUNNING after add");
  check(omni_connection_manager_capacity(&env.mgr)==MGR_CAP, "manager capacity MGR_CAP");
  const struct omni_connection_manager_entry *e = omni_connection_manager_find(&env.mgr,&conn);
  check(e!=NULL, "manager find returns non-NULL");
  check(e->token==r.token, "entry token matches");
  size_t count_before_lookup = omni_connection_manager_count(&env.mgr);
  enum omni_connection_manager_state state_before_lookup = omni_connection_manager_state(&env.mgr);
  (void)omni_connection_manager_find(&env.mgr, &conn);
  check(omni_connection_manager_count(&env.mgr) == count_before_lookup &&
            omni_connection_manager_state(&env.mgr) == state_before_lookup,
        "find of live connection does not mutate manager state");
  check(omni_connection_runtime_fd(&env.crt,&conn)>=0, "runtime fd valid after manager add");
  check(omni_connection_runtime_find_session(&env.crt,&conn)!=NULL,
        "runtime session found after manager add");
  check(omni_connection_reactor_count(&env.adapter)==1u, "reactor count 1 manager add");
  check(omni_connection_registry_count(&env.registry)==1u, "registry count 1 manager add");
  check(omni_connection_runtime_count(&env.crt)==1u, "runtime count 1 manager add");

  /* Detach via manager */
  r = omni_connection_manager_remove(&env.mgr,&conn);
  check(r.status==OMNI_CONNECTION_MANAGER_OK, "manager remove success");
  check(omni_connection_manager_count(&env.mgr)==0u, "manager count 0 after remove");
  check(env.mgr.state==OMNI_CONNECTION_MANAGER_INITIALIZED, "manager state INITIALIZED after last remove");
  check(omni_connection_manager_find(&env.mgr,&conn)==NULL, "manager find NULL after remove");
  check(omni_connection_reactor_count(&env.adapter)==0u, "reactor 0 after manager remove");
  check(omni_connection_registry_count(&env.registry)==0u, "registry 0 after manager remove");
  check(omni_connection_runtime_count(&env.crt)==0u, "runtime 0 after manager remove");
  check(omni_connection_is_live(&conn), "connection still live after manager remove");
  check(omni_connection_fd(&conn)>=0, "connection fd still valid after manager remove");

  /* Remove again not found */
  size_t count_before_missing_remove = omni_connection_manager_count(&env.mgr);
  enum omni_connection_manager_state state_before_missing_remove =
      omni_connection_manager_state(&env.mgr);
  r = omni_connection_manager_remove(&env.mgr,&conn);
  check(r.status==OMNI_CONNECTION_MANAGER_ERR_NOT_FOUND, "second manager remove NOT_FOUND");
  check(omni_connection_manager_count(&env.mgr) == count_before_missing_remove &&
            omni_connection_manager_state(&env.mgr) == state_before_missing_remove,
        "missing manager remove leaves state and count unchanged");

  omni_connection_destroy(&conn);
  omni_accepted_destroy(&accepted);
  if (client_fd!=-1) close(client_fd);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_duplicate_add(void) {
  struct omni_listener listener={0};
  struct omni_accepted accepted={0};
  struct omni_connection conn={0};
  unsigned char conn_storage[CONN_RECV_CAP];
  unsigned char sess_recv[SESS_RECV_CAP];
  unsigned char sess_send[SESS_SEND_CAP];
  uint16_t port=0; int client_fd=-1;
  struct test_manager_env env;
  if (!init_env(&env)) return;
  if (!start_listener(&listener,&port)) { cleanup_env(&env); return; }
  client_fd=open_client(port);
  if (client_fd==-1) { cleanup_env(&env); omni_listener_destroy(&listener); return; }
  if (!accept_one(&listener,&accepted)) { cleanup_env(&env); close(client_fd); omni_listener_destroy(&listener); return; }
  if (!make_open_connection(&conn,conn_storage,CONN_RECV_CAP,&accepted)) { cleanup_env(&env); close(client_fd); omni_listener_destroy(&listener); return; }
  struct omni_connection_manager_attach_config acfg={&conn,sess_recv,SESS_RECV_CAP,sess_send,SESS_SEND_CAP};
  struct omni_connection_manager_result r = omni_connection_manager_add(&env.mgr,&acfg);
  check(r.status==OMNI_CONNECTION_MANAGER_OK, "duplicate test first add ok");
  size_t count_before = omni_connection_manager_count(&env.mgr);
  enum omni_connection_manager_state state_before = omni_connection_manager_state(&env.mgr);
  struct omni_connection_manager_result dup = omni_connection_manager_add(&env.mgr,&acfg);
  check(dup.status==OMNI_CONNECTION_MANAGER_ERR_DUPLICATE, "duplicate add rejected manager");
  check(omni_connection_manager_count(&env.mgr)==count_before, "count unchanged after duplicate manager");
  check(omni_connection_manager_state(&env.mgr)==state_before, "state unchanged after duplicate manager");
  check(omni_connection_reactor_count(&env.adapter)==1u, "reactor still 1 after duplicate");
  omni_connection_manager_remove(&env.mgr,&conn);
  omni_connection_destroy(&conn);
  omni_accepted_destroy(&accepted);
  if (client_fd!=-1) close(client_fd);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_capacity_full(void) {
  struct omni_listener listener={0};
  uint16_t port=0;
  struct test_manager_env env;
  if (!init_env(&env)) return;
  if (!start_listener(&listener,&port)) { cleanup_env(&env); return; }

  struct omni_connection conns[MGR_CAP+1];
  unsigned char cs[MGR_CAP+1][CONN_RECV_CAP];
  struct omni_accepted accs[MGR_CAP+1];
  int cfs[MGR_CAP+1];
  unsigned char srs[MGR_CAP+1][SESS_RECV_CAP];
  unsigned char sss[MGR_CAP+1][SESS_SEND_CAP];
  size_t i;
  for (i=0;i<MGR_CAP+1;++i) { omni_connection_make_inert(&conns[i]); omni_accepted_make_inert(&accs[i]); cfs[i]=-1; }

  for (i=0;i<MGR_CAP;++i) {
    cfs[i]=open_client(port);
    if (cfs[i]==-1) break;
    if (!accept_one(&listener,&accs[i])) break;
    if (!make_open_connection(&conns[i],cs[i],CONN_RECV_CAP,&accs[i])) break;
    struct omni_connection_manager_attach_config ac={&conns[i],srs[i],SESS_RECV_CAP,sss[i],SESS_SEND_CAP};
    struct omni_connection_manager_result r = omni_connection_manager_add(&env.mgr,&ac);
    char name[64]; snprintf(name,sizeof(name),"manager attach %zu ok",i);
    check(r.status==OMNI_CONNECTION_MANAGER_OK, name);
  }
  check(omni_connection_manager_count(&env.mgr)==MGR_CAP, "manager count == capacity after fills");
  check(omni_connection_runtime_count(&env.crt)==MGR_CAP, "runtime count == capacity after fills manager");
  enum omni_connection_manager_state state_before = omni_connection_manager_state(&env.mgr);
  size_t count_before = omni_connection_manager_count(&env.mgr);
  struct omni_connection_manager_attach_config duplicate_ac = {
    &conns[0], srs[0], SESS_RECV_CAP, sss[0], SESS_SEND_CAP
  };
  struct omni_connection_manager_result duplicate_full =
      omni_connection_manager_add(&env.mgr, &duplicate_ac);
  check(duplicate_full.status == OMNI_CONNECTION_MANAGER_ERR_DUPLICATE,
        "duplicate remains DUPLICATE when manager is full");
  check(omni_connection_manager_count(&env.mgr) == count_before &&
            omni_connection_manager_state(&env.mgr) == state_before,
        "duplicate at capacity leaves manager unchanged");
  /* one more should be FULL */
  cfs[MGR_CAP]=open_client(port);
  if (cfs[MGR_CAP]!=-1) {
    accept_one(&listener,&accs[MGR_CAP]);
    make_open_connection(&conns[MGR_CAP],cs[MGR_CAP],CONN_RECV_CAP,&accs[MGR_CAP]);
    struct omni_connection_manager_attach_config ac5={&conns[MGR_CAP],srs[MGR_CAP],SESS_RECV_CAP,sss[MGR_CAP],SESS_SEND_CAP};
    struct omni_connection_manager_result r5 = omni_connection_manager_add(&env.mgr,&ac5);
    check(r5.status==OMNI_CONNECTION_MANAGER_ERR_FULL, "manager attach beyond capacity FULL");
    check(omni_connection_manager_count(&env.mgr)==count_before, "manager count unchanged after FULL");
    check(omni_connection_manager_state(&env.mgr)==state_before, "manager state unchanged after FULL");
  } else {
    check(false, "manager capacity full bystander client");
  }

  const size_t count_before_one_remove = omni_connection_manager_count(&env.mgr);
  check(omni_connection_manager_remove(&env.mgr, &conns[0]).status ==
            OMNI_CONNECTION_MANAGER_OK,
        "remove one connection from full manager");
  check(omni_connection_manager_count(&env.mgr) == count_before_one_remove - 1u &&
            omni_connection_manager_find(&env.mgr, &conns[1]) != NULL &&
            omni_connection_manager_find(&env.mgr, &conns[MGR_CAP - 1u]) != NULL,
        "removing one connection preserves the other memberships");

  /* cleanup */
  for (i=1u;i<MGR_CAP;++i) {
    omni_connection_manager_remove(&env.mgr,&conns[i]);
  }
  check(omni_connection_manager_count(&env.mgr)==0u, "manager all detached count 0 capacity full");
  for (i=0;i<MGR_CAP+1;++i) {
    omni_connection_destroy(&conns[i]);
    omni_accepted_destroy(&accs[i]);
    if (cfs[i]!=-1) close(cfs[i]);
  }
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_stale_slot_reuse(void) {
  struct test_manager_env env;
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
  struct omni_connection_manager_attach_config ac1={&c1,sr1,SESS_RECV_CAP,ss1,SESS_SEND_CAP};
  struct omni_connection_manager_result r1 = omni_connection_manager_add(&env.mgr,&ac1);
  check(r1.status==OMNI_CONNECTION_MANAGER_OK, "stale test manager attach c1");
  uint64_t tok1=r1.token;
  check(tok1!=0u, "manager token1 non-zero");
  /* also check runtime token matches */
  const struct omni_connection_manager_entry *e1 = omni_connection_manager_find(&env.mgr,&c1);
  check(e1!=NULL && e1->token==tok1, "manager entry token matches");

  /* remove c1, token should become stale */
  omni_connection_manager_remove(&env.mgr,&c1);
  struct omni_connection_reactor_result dr = omni_connection_reactor_dispatch(&env.adapter, tok1, OMNI_POLLER_INTEREST_READ);
  check(dr.status==OMNI_CONNECTION_REACTOR_IGNORED, "manager stale token ignored after remove");

  /* re-attach new connection, should get different token (generation changed) */
  cf2=open_client(port); accept_one(&listener,&a2); make_open_connection(&c2,cs2,CONN_RECV_CAP,&a2);
  struct omni_connection_manager_attach_config ac2={&c2,sr2,SESS_RECV_CAP,ss2,SESS_SEND_CAP};
  struct omni_connection_manager_result r2 = omni_connection_manager_add(&env.mgr,&ac2);
  check(r2.status==OMNI_CONNECTION_MANAGER_OK, "manager attach c2 after stale");
  check(r2.token!=tok1, "manager new token != stale token");
  const struct omni_connection_manager_entry *e2 = omni_connection_manager_find(&env.mgr,&c2);
  check(e2 == e1, "manager reuses the same slot for the new connection");

  /* remove c2, slot reusable again */
  omni_connection_manager_remove(&env.mgr,&c2);
  check(omni_connection_manager_count(&env.mgr)==0u, "manager count 0 after stale reuse");

  /* re-add c1 again to test slot reuse explicitly */
  /* Need fresh accepted for c1? c1 still OPEN but was removed from manager; we can re-add same c1 object */
  /* c1's connection is still live; re-add it */
  struct omni_connection_manager_attach_config ac1b={&c1,sr1,SESS_RECV_CAP,ss1,SESS_SEND_CAP};
  struct omni_connection_manager_result r1b = omni_connection_manager_add(&env.mgr,&ac1b);
  check(r1b.status==OMNI_CONNECTION_MANAGER_OK, "manager re-add c1 after reuse");
  check(r1b.token!=tok1, "manager re-add token differs from original");
  omni_connection_manager_remove(&env.mgr,&c1);

  omni_connection_destroy(&c1); omni_connection_destroy(&c2);
  omni_accepted_destroy(&a1); omni_accepted_destroy(&a2);
  if (cf1!=-1) close(cf1);
  if (cf2!=-1) close(cf2);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_destroy_cleanup(void) {
  struct test_manager_env env;
  struct omni_listener listener={0};
  struct omni_accepted a={0};
  struct omni_connection c={0};
  unsigned char cs[CONN_RECV_CAP];
  unsigned char sr[SESS_RECV_CAP], ss[SESS_SEND_CAP];
  uint16_t port=0; int cf=-1;
  if (!init_env(&env)) return;
  if (!start_listener(&listener,&port)) { cleanup_env(&env); return; }
  cf=open_client(port); accept_one(&listener,&a); make_open_connection(&c,cs,CONN_RECV_CAP,&a);
  struct omni_connection_manager_attach_config ac={&c,sr,SESS_RECV_CAP,ss,SESS_SEND_CAP};
  omni_connection_manager_add(&env.mgr,&ac);
  check(omni_connection_manager_count(&env.mgr)==1u, "destroy cleanup count 1 before");
  check(omni_connection_reactor_count(&env.adapter)==1u, "reactor 1 before destroy cleanup");
  omni_connection_manager_destroy(&env.mgr);
  check(omni_connection_manager_state(&env.mgr)==OMNI_CONNECTION_MANAGER_CLOSED, "manager destroy -> CLOSED");
  check(omni_connection_manager_count(&env.mgr)==0u, "manager count 0 after destroy");
  check(omni_connection_reactor_count(&env.adapter)==0u, "reactor 0 after manager destroy");
  check(omni_connection_registry_count(&env.registry)==0u, "registry 0 after manager destroy");
  check(omni_connection_runtime_count(&env.crt)==0u, "runtime 0 after manager destroy");
  check(omni_connection_is_live(&c), "conn still live after manager destroy");
  /* idempotent */
  omni_connection_manager_destroy(&env.mgr);
  check(omni_connection_manager_state(&env.mgr)==OMNI_CONNECTION_MANAGER_CLOSED, "double destroy stays CLOSED manager");
  struct omni_connection_manager_attach_config closed_attach = {0};
  check(omni_connection_manager_add(&env.mgr, &closed_attach).status == OMNI_CONNECTION_MANAGER_ERR_STATE,
        "closed manager rejects add");
  check(omni_connection_manager_remove(&env.mgr, &c).status == OMNI_CONNECTION_MANAGER_ERR_STATE,
        "closed manager rejects remove");
  check(omni_connection_manager_start(&env.mgr).status == OMNI_CONNECTION_MANAGER_ERR_STATE,
        "closed manager rejects start");
  check(omni_connection_manager_stop(&env.mgr).status == OMNI_CONNECTION_MANAGER_ERR_STATE,
        "closed manager rejects stop");
  omni_connection_destroy(&c);
  omni_accepted_destroy(&a);
  if (cf!=-1) close(cf);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_destroy_multiple_active(void) {
  struct test_manager_env env;
  struct omni_listener listener={0};
  struct omni_accepted accepted[MULTI_ACTIVE_CAP];
  struct omni_connection connections[MULTI_ACTIVE_CAP];
  unsigned char connection_storage[MULTI_ACTIVE_CAP][CONN_RECV_CAP];
  unsigned char receive_storage[MULTI_ACTIVE_CAP][SESS_RECV_CAP];
  unsigned char send_storage[MULTI_ACTIVE_CAP][SESS_SEND_CAP];
  int client_fds[MULTI_ACTIVE_CAP];
  int connection_fds[MULTI_ACTIVE_CAP];
  uint16_t port=0;
  size_t prepared=0u;

  for (size_t i=0u; i<MULTI_ACTIVE_CAP; ++i) {
    client_fds[i] = -1;
    connection_fds[i] = OMNI_ACCEPTED_FD_INVALID;
    omni_accepted_make_inert(&accepted[i]);
    omni_connection_make_inert(&connections[i]);
  }
  if (!init_env_with_event_loop(&env)) {
    check(false, "manager env with live borrowed event loop initializes");
    return;
  }
  if (!start_listener(&listener, &port)) {
    cleanup_env(&env);
    return;
  }

  for (size_t i=0u; i<MULTI_ACTIVE_CAP; ++i) {
    client_fds[i] = open_client(port);
    if (client_fds[i] < 0 || !accept_one(&listener, &accepted[i]) ||
        !make_open_connection(&connections[i], connection_storage[i],
                              CONN_RECV_CAP, &accepted[i])) {
      break;
    }
    connection_fds[i] = omni_connection_fd(&connections[i]);
    struct omni_connection_manager_attach_config attach = {
      &connections[i], receive_storage[i], SESS_RECV_CAP,
      send_storage[i], SESS_SEND_CAP
    };
    if (omni_connection_manager_add(&env.mgr, &attach).status != OMNI_CONNECTION_MANAGER_OK) {
      break;
    }
    prepared += 1u;
  }

  check(prepared == MULTI_ACTIVE_CAP, "multiple manager connections active before destroy");
  check(omni_connection_manager_count(&env.mgr) == prepared,
        "manager count tracks multiple active connections");
  check(omni_connection_runtime_count(&env.crt) == prepared,
        "runtime tracks multiple active manager connections");
  check(omni_connection_reactor_count(&env.adapter) == prepared,
        "reactor has multiple manager registrations before destroy");
  check(env.loop.state == OMNI_EVENT_LOOP_INITIALIZED &&
            env.runtime.state == OMNI_RUNTIME_INITIALIZED,
        "borrowed event loop and runtime valid before manager destroy");

  omni_connection_manager_destroy(&env.mgr);
  check(omni_connection_manager_state(&env.mgr) == OMNI_CONNECTION_MANAGER_CLOSED,
        "multi-connection manager destroy reaches CLOSED");
  check(omni_connection_manager_count(&env.mgr) == 0u,
        "multi-connection manager count cleared");
  check(omni_connection_reactor_count(&env.adapter) == 0u,
        "all multi-connection reactor registrations removed");
  check(omni_connection_registry_count(&env.registry) == 0u,
        "all multi-connection registry entries removed");
  check(omni_connection_runtime_count(&env.crt) == 0u,
        "all multi-connection runtime sessions released");
  check(env.registry.live && env.adapter.live,
        "borrowed registry and adapter remain live");
  check(env.reactor.live && env.poller.live,
        "borrowed reactor and poller remain live");
  for (size_t i=0u; i<MGR_CAP; ++i) {
    check(!env.mgr_entries[i].occupied && env.mgr_entries[i].connection == NULL &&
              env.mgr_entries[i].token == 0u,
          "manager destroy clears caller-owned membership entries");
  }
  check(env.loop.state == OMNI_EVENT_LOOP_INITIALIZED && env.loop.runtime == &env.runtime &&
            env.runtime.state == OMNI_RUNTIME_INITIALIZED &&
            omni_listener_fd(&env.loop_listener) >= 0,
        "borrowed event loop and runtime remain live after manager destroy");

  for (size_t i=0u; i<prepared; ++i) {
    check(omni_connection_is_live(&connections[i]), "external connection remains live after destroy");
    check(omni_connection_fd(&connections[i]) == connection_fds[i] &&
              fd_is_open(connection_fds[i]),
          "external connection descriptor remains open after destroy");
    check(connections[i].accepted.live && connections[i].accepted.fd == connection_fds[i],
          "accepted owner remains with caller connection");
    check(fd_is_open(client_fds[i]), "external client descriptor remains open after destroy");
    check(!accepted[i].live && omni_accepted_fd(&accepted[i]) == OMNI_ACCEPTED_FD_INVALID,
          "moved accepted source remains inert after manager destroy");
  }

  for (size_t i=0u; i<MULTI_ACTIVE_CAP; ++i) {
    omni_connection_destroy(&connections[i]);
    omni_accepted_destroy(&accepted[i]);
    if (client_fds[i] >= 0) (void)close(client_fds[i]);
  }
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_runtime_detach(void) {
  struct test_manager_env env;
  struct omni_listener listener={0};
  struct omni_accepted a={0};
  struct omni_connection c={0};
  unsigned char cs[CONN_RECV_CAP];
  unsigned char sr[SESS_RECV_CAP], ss[SESS_SEND_CAP];
  uint16_t port=0; int cf=-1;
  if (!init_env(&env)) return;
  if (!start_listener(&listener,&port)) { cleanup_env(&env); return; }
  cf=open_client(port); accept_one(&listener,&a); make_open_connection(&c,cs,CONN_RECV_CAP,&a);
  struct omni_connection_manager_attach_config ac={&c,sr,SESS_RECV_CAP,ss,SESS_SEND_CAP};
  omni_connection_manager_add(&env.mgr,&ac);
  check(omni_connection_runtime_count(&env.crt)==1u, "runtime 1 after manager add runtime_detach test");
  struct omni_connection_session *sess = omni_connection_runtime_find_session(&env.crt,&c);
  check(sess!=NULL && omni_connection_session_is_open(sess), "manager session OPEN runtime_detach");
  /* manager remove should delegate to runtime detach */
  omni_connection_manager_remove(&env.mgr,&c);
  check(omni_connection_runtime_count(&env.crt)==0u, "runtime 0 after manager remove runtime_detach");
  check(omni_connection_manager_find(&env.mgr,&c)==NULL,
        "manager membership gone after remove");
  check(omni_connection_runtime_find_session(&env.crt,&c)==NULL, "runtime session gone after manager remove");
  omni_connection_destroy(&c);
  omni_accepted_destroy(&a);
  if (cf!=-1) close(cf);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_reactor_registration_cleanup(void) {
  struct test_manager_env env;
  struct omni_listener listener={0};
  struct omni_accepted a={0};
  struct omni_connection c={0};
  unsigned char cs[CONN_RECV_CAP];
  unsigned char sr[SESS_RECV_CAP], ss[SESS_SEND_CAP];
  uint16_t port=0; int cf=-1;
  if (!init_env(&env)) return;
  if (!start_listener(&listener,&port)) { cleanup_env(&env); return; }
  cf=open_client(port); accept_one(&listener,&a); make_open_connection(&c,cs,CONN_RECV_CAP,&a);
  struct omni_connection_manager_attach_config ac={&c,sr,SESS_RECV_CAP,ss,SESS_SEND_CAP};
  omni_connection_manager_add(&env.mgr,&ac);
  check(omni_connection_reactor_count(&env.adapter)==1u, "reactor 1 after manager add cleanup test");
  omni_connection_manager_remove(&env.mgr,&c);
  check(omni_connection_reactor_count(&env.adapter)==0u, "reactor 0 after manager remove cleanup");
  /* add again then destroy manager should also clean reactor */
  omni_connection_manager_add(&env.mgr,&ac);
  check(omni_connection_reactor_count(&env.adapter)==1u, "reactor 1 after re-add");
  omni_connection_manager_destroy(&env.mgr);
  check(omni_connection_reactor_count(&env.adapter)==0u, "reactor 0 after manager destroy cleanup");
  check(omni_connection_registry_count(&env.registry)==0u, "registry 0 after manager destroy cleanup2");
  omni_connection_destroy(&c);
  omni_accepted_destroy(&a);
  if (cf!=-1) close(cf);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_descriptor_preservation(void) {
  struct test_manager_env env;
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
  struct omni_connection_manager_attach_config ac={&conn,sr,SESS_RECV_CAP,ss,SESS_SEND_CAP};
  omni_connection_manager_add(&env.mgr,&ac);
  check(omni_listener_fd(&listener)==lfd_before, "manager listener fd preserved after add");
  check(omni_connection_fd(&conn)==cfd_before, "manager conn fd preserved after add");
  check(omni_connection_runtime_fd(&env.crt,&conn)==cfd_before,
        "runtime fd view matches after manager add");

  omni_connection_manager_remove(&env.mgr,&conn);
  check(omni_listener_fd(&listener)==lfd_before, "manager listener fd preserved after remove");
  check(omni_connection_fd(&conn)==cfd_before, "manager conn fd preserved after remove");
  check(omni_connection_is_live(&conn), "manager conn still live after remove");

  omni_connection_manager_add(&env.mgr,&ac);
  omni_connection_manager_destroy(&env.mgr);
  check(omni_listener_fd(&listener)==lfd_before, "manager listener fd preserved after destroy");
  check(omni_connection_fd(&conn)==cfd_before, "manager conn fd preserved after manager destroy");
  check(omni_connection_is_live(&conn), "manager conn live after manager destroy");

  omni_connection_destroy(&conn);
  check(omni_connection_fd(&conn)==OMNI_ACCEPTED_FD_INVALID, "manager conn fd invalid after conn destroy");
  omni_accepted_destroy(&accepted);
  if (cf!=-1) close(cf);
  check(omni_listener_fd(&listener)==lfd_before, "manager listener still valid after all");
  omni_listener_destroy(&listener);
  check(omni_listener_fd(&listener)==OMNI_ACCEPTED_FD_INVALID, "manager listener fd invalid after destroy");
  cleanup_env(&env);
}

static void test_connection_ownership_preservation(void) {
  struct test_manager_env env;
  struct omni_listener listener={0};
  struct omni_accepted accepted={0};
  struct omni_connection conn={0};
  unsigned char cs[CONN_RECV_CAP];
  unsigned char sr[SESS_RECV_CAP], ss[SESS_SEND_CAP];
  uint16_t port=0; int cf=-1;
  if (!init_env(&env)) return;
  if (!start_listener(&listener,&port)) { cleanup_env(&env); return; }
  cf=open_client(port); accept_one(&listener,&accepted); make_open_connection(&conn,cs,CONN_RECV_CAP,&accepted);
  int cfd = omni_connection_fd(&conn);
  check(cfd>=0, "ownership test fd valid");
  struct omni_connection_manager_attach_config ac={&conn,sr,SESS_RECV_CAP,ss,SESS_SEND_CAP};
  omni_connection_manager_add(&env.mgr,&ac);
  check(omni_connection_is_live(&conn), "manager ownership conn live after add");
  check(omni_connection_fd(&conn)==cfd, "manager fd unchanged after add ownership");
  omni_connection_manager_remove(&env.mgr,&conn);
  check(omni_connection_is_live(&conn), "manager ownership conn live after remove");
  check(omni_connection_fd(&conn)==cfd, "manager fd unchanged after remove ownership");
  /* re-add and destroy manager, connection should remain */
  omni_connection_manager_add(&env.mgr,&ac);
  omni_connection_manager_destroy(&env.mgr);
  check(omni_connection_is_live(&conn), "manager ownership conn live after destroy");
  check(omni_connection_fd(&conn)==cfd, "manager fd unchanged after destroy ownership");
  omni_connection_destroy(&conn);
  check(!omni_connection_is_live(&conn), "ownership conn not live after explicit destroy");
  omni_accepted_destroy(&accepted);
  if (cf!=-1) close(cf);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_repeated_cycles(void) {
  struct test_manager_env env;
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
    struct omni_connection_manager_attach_config ac={&c,sr,SESS_RECV_CAP,ss,SESS_SEND_CAP};
    struct omni_connection_manager_result rr = omni_connection_manager_add(&env.mgr,&ac);
    check(rr.status==OMNI_CONNECTION_MANAGER_OK, "manager cycle add ok");
    client_send_all(cf,(unsigned char*)"x",1);
    const struct omni_connection_manager_entry *e = omni_connection_manager_find(&env.mgr,&c);
    if (e) {
      struct omni_connection_session *sess = omni_connection_runtime_find_session(&env.crt,&c);
      if (sess) { (void)omni_connection_session_readable(sess); struct omni_bytebuf *sb=omni_connection_session_send_buffer(sess); if (sb) { omni_bytebuf_append(sb,(unsigned char*)"y",1); (void)omni_connection_session_writable(sess); } }
    }
    rr = omni_connection_manager_remove(&env.mgr,&c);
    check(rr.status==OMNI_CONNECTION_MANAGER_OK, "manager cycle remove ok");
    omni_connection_destroy(&c);
    omni_accepted_destroy(&a);
    close(cf);
  }
  check(i==20, "20 repeated manager add/remove cycles");
  check(omni_connection_manager_count(&env.mgr)==0u, "manager count 0 after cycles");
  check(omni_connection_reactor_count(&env.adapter)==0u, "manager reactor 0 after cycles");
  check(omni_connection_runtime_count(&env.crt)==0u, "manager runtime 0 after cycles");
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_thousand_operation_stress(void) {
  struct test_manager_env env;
  struct omni_listener listener={0};
  struct omni_accepted accepted={0};
  struct omni_connection connection={0};
  unsigned char connection_storage[CONN_RECV_CAP];
  unsigned char receive_storage[SESS_RECV_CAP];
  unsigned char send_storage[SESS_SEND_CAP];
  uint16_t port=0;
  int client_fd=-1;
  int baseline_fds=-1;
  size_t operations=0u;
  bool stable=true;

  if (!init_env(&env)) return;
  if (!start_listener(&listener, &port)) { cleanup_env(&env); return; }
  client_fd = open_client(port);
  if (client_fd < 0 || !accept_one(&listener, &accepted) ||
      !make_open_connection(&connection, connection_storage, CONN_RECV_CAP, &accepted)) {
    if (client_fd >= 0) (void)close(client_fd);
    omni_listener_destroy(&listener);
    cleanup_env(&env);
    return;
  }

  baseline_fds = fd_census();
  if (baseline_fds < 0) {
    check(false, "FD census available for manager stress");
    omni_connection_destroy(&connection);
    omni_accepted_destroy(&accepted);
    (void)close(client_fd);
    omni_listener_destroy(&listener);
    cleanup_env(&env);
    return;
  }

  struct omni_connection_manager_attach_config attach = {
    &connection, receive_storage, SESS_RECV_CAP, send_storage, SESS_SEND_CAP
  };
  for (size_t cycle=0u; cycle<MANAGER_STRESS_CYCLES && stable; ++cycle) {
    struct omni_connection_manager_result added =
        omni_connection_manager_add(&env.mgr, &attach);
    if (added.status != OMNI_CONNECTION_MANAGER_OK) {
      fprintf(stderr, "stress add failed at cycle %zu with status %d\n",
              cycle, (int)added.status);
      stable = false;
      break;
    }
    operations += 1u;
    if (omni_connection_manager_count(&env.mgr) >
            omni_connection_manager_capacity(&env.mgr) ||
        omni_connection_manager_count(&env.mgr) != 1u || fd_census() != baseline_fds) {
      fprintf(stderr, "manager bound or FD census changed after add at cycle %zu\n", cycle);
      stable = false;
      break;
    }

    struct omni_connection_manager_result removed =
        omni_connection_manager_remove(&env.mgr, &connection);
    if (removed.status != OMNI_CONNECTION_MANAGER_OK) {
      fprintf(stderr, "stress remove failed at cycle %zu with status %d\n",
              cycle, (int)removed.status);
      stable = false;
      break;
    }
    operations += 1u;
    if (omni_connection_manager_count(&env.mgr) >
            omni_connection_manager_capacity(&env.mgr) ||
        omni_connection_manager_count(&env.mgr) != 0u || fd_census() != baseline_fds) {
      fprintf(stderr, "manager bound or FD census changed after remove at cycle %zu\n", cycle);
      stable = false;
      break;
    }
  }

  check(stable && operations >= 1000u,
        "1000 manager add/remove operations keep capacity and FD census stable");
  check(omni_connection_manager_count(&env.mgr) == 0u &&
            omni_connection_runtime_count(&env.crt) == 0u &&
            omni_connection_reactor_count(&env.adapter) == 0u,
        "manager stress ends with no live membership or registration");
  check(omni_connection_is_live(&connection) && fd_is_open(omni_connection_fd(&connection)) &&
            fd_is_open(client_fd),
        "manager stress preserves caller-owned connection and client descriptors");
  if (omni_connection_manager_find(&env.mgr, &connection) != NULL) {
    (void)omni_connection_manager_remove(&env.mgr, &connection);
  }
  omni_connection_destroy(&connection);
  omni_accepted_destroy(&accepted);
  (void)close(client_fd);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_fd_census(void) {
  struct test_manager_env env;
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
  check(bystander!=-1, "manager bystander fd opens");
  cf=open_client(port); accept_one(&listener,&a); make_open_connection(&c,cs,CONN_RECV_CAP,&a);
  struct omni_connection_manager_attach_config ac={&c,sr,SESS_RECV_CAP,ss,SESS_SEND_CAP};
  omni_connection_manager_add(&env.mgr,&ac);
  omni_connection_manager_remove(&env.mgr,&c);
  omni_connection_manager_destroy(&env.mgr);
  check(close(bystander)==0, "manager bystander survives lifecycle");
  omni_connection_destroy(&c);
  omni_accepted_destroy(&a);
  if (cf!=-1) close(cf);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_no_add_after_stopping(void) {
  struct test_manager_env env;
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
  struct omni_connection_manager_attach_config ac1={&c1,sr1,SESS_RECV_CAP,ss1,SESS_SEND_CAP};
  check(omni_connection_manager_add(&env.mgr,&ac1).status==OMNI_CONNECTION_MANAGER_OK, "stop test first add ok");
  struct omni_connection_manager_result sr = omni_connection_manager_stop(&env.mgr);
  check(sr.status==OMNI_CONNECTION_MANAGER_OK, "manager stop ok");
  check(env.mgr.state==OMNI_CONNECTION_MANAGER_STOPPING, "manager STOPPING after stop");
  cf2=open_client(port); accept_one(&listener,&a2); make_open_connection(&c2,cs2,CONN_RECV_CAP,&a2);
  struct omni_connection_manager_attach_config ac2={&c2,sr2,SESS_RECV_CAP,ss2,SESS_SEND_CAP};
  size_t count_before = omni_connection_manager_count(&env.mgr);
  struct omni_connection_manager_result r2 = omni_connection_manager_add(&env.mgr,&ac2);
  check(r2.status==OMNI_CONNECTION_MANAGER_ERR_STATE, "manager add after stopping rejected");
  check(omni_connection_manager_count(&env.mgr)==count_before, "manager count unchanged after add after stopping");
  /* remove should still be allowed in STOPPING */
  check(omni_connection_manager_remove(&env.mgr,&c1).status==OMNI_CONNECTION_MANAGER_OK, "manager remove allowed in STOPPING");
  check(omni_connection_manager_count(&env.mgr)==0u, "manager count 0 after remove in STOPPING");
  check(env.mgr.state==OMNI_CONNECTION_MANAGER_STOPPING, "manager stays STOPPING after last remove");
  omni_connection_destroy(&c1); omni_connection_destroy(&c2);
  omni_accepted_destroy(&a1); omni_accepted_destroy(&a2);
  if (cf1!=-1) close(cf1);
  if (cf2!=-1) close(cf2);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_failed_add_unchanged(void) {
  struct test_manager_env env;
  struct omni_listener listener={0};
  struct omni_accepted a={0};
  struct omni_connection c={0};
  unsigned char cs[CONN_RECV_CAP];
  unsigned char sr[SESS_RECV_CAP], ss[SESS_SEND_CAP];
  uint16_t port=0; int cf=-1;
  if (!init_env(&env)) return;
  if (!start_listener(&listener,&port)) { cleanup_env(&env); return; }
  cf=open_client(port); accept_one(&listener,&a); make_open_connection(&c,cs,CONN_RECV_CAP,&a);
  struct omni_connection_manager_attach_config ac={&c,sr,SESS_RECV_CAP,ss,SESS_SEND_CAP};
  check(omni_connection_manager_add(&env.mgr,&ac).status==OMNI_CONNECTION_MANAGER_OK, "failed add test first ok");
  size_t count_before = omni_connection_manager_count(&env.mgr);
  enum omni_connection_manager_state state_before = omni_connection_manager_state(&env.mgr);
  /* duplicate should leave state unchanged */
  struct omni_connection_manager_result dup = omni_connection_manager_add(&env.mgr,&ac);
  check(dup.status==OMNI_CONNECTION_MANAGER_ERR_DUPLICATE, "failed add duplicate leaves unchanged");
  check(omni_connection_manager_count(&env.mgr)==count_before, "count unchanged after failed duplicate manager");
  check(omni_connection_manager_state(&env.mgr)==state_before, "state unchanged after failed duplicate manager");
  /* invalid connection (INERT) should also leave unchanged */
  struct omni_connection bad={0};
  omni_connection_make_inert(&bad);
  struct omni_connection_manager_attach_config bad_ac={&bad,sr,SESS_RECV_CAP,ss,SESS_SEND_CAP};
  struct omni_connection_manager_result bad_r = omni_connection_manager_add(&env.mgr,&bad_ac);
  check(bad_r.status==OMNI_CONNECTION_MANAGER_ERR_INVALID, "failed add invalid leaves unchanged");
  check(omni_connection_manager_count(&env.mgr)==count_before, "count unchanged after invalid manager");
  check(omni_connection_manager_state(&env.mgr)==state_before, "state unchanged after invalid manager");
  omni_connection_manager_remove(&env.mgr,&c);
  omni_connection_destroy(&c);
  omni_accepted_destroy(&a);
  if (cf!=-1) close(cf);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_failed_runtime_attach_rollback(void) {
  struct test_manager_env env;
  struct omni_listener listener={0};
  struct omni_accepted accepted={0};
  struct omni_connection connection={0};
  unsigned char connection_storage[CONN_RECV_CAP];
  unsigned char receive_storage[SESS_RECV_CAP];
  unsigned char send_storage[SESS_SEND_CAP];
  uint16_t port=0;
  int client_fd=-1;
  uint64_t external_token=0u;

  if (!init_env(&env)) return;
  if (!start_listener(&listener, &port)) { cleanup_env(&env); return; }
  client_fd = open_client(port);
  if (client_fd < 0 || !accept_one(&listener, &accepted) ||
      !make_open_connection(&connection, connection_storage, CONN_RECV_CAP, &accepted)) {
    if (client_fd >= 0) (void)close(client_fd);
    omni_listener_destroy(&listener);
    cleanup_env(&env);
    return;
  }

  /* Force runtime attach to reach session setup, then fail on adapter duplicate. */
  struct omni_connection_reactor_result external =
      omni_connection_reactor_attach(&env.adapter, &connection);
  external_token = external.token;
  check(external.status == OMNI_CONNECTION_REACTOR_OK && external_token != 0u,
        "external registration created for runtime rollback test");
  if (external.status != OMNI_CONNECTION_REACTOR_OK || external_token == 0u) {
    omni_connection_destroy(&connection);
    omni_accepted_destroy(&accepted);
    (void)close(client_fd);
    omni_listener_destroy(&listener);
    cleanup_env(&env);
    return;
  }
  struct omni_connection_manager_attach_config attach = {
    &connection, receive_storage, SESS_RECV_CAP, send_storage, SESS_SEND_CAP
  };
  const size_t count_before = omni_connection_manager_count(&env.mgr);
  const enum omni_connection_manager_state state_before =
      omni_connection_manager_state(&env.mgr);
  struct omni_connection_manager_result result =
      omni_connection_manager_add(&env.mgr, &attach);
  check(result.status == OMNI_CONNECTION_MANAGER_ERR_DUPLICATE,
        "adapter duplicate reports failed manager attach");
  check(omni_connection_manager_count(&env.mgr) == count_before &&
            omni_connection_manager_state(&env.mgr) == state_before,
        "failed runtime attach leaves manager state and count unchanged");
  check(omni_connection_manager_find(&env.mgr, &connection) == NULL &&
            omni_connection_runtime_find_session(&env.crt, &connection) == NULL,
        "failed runtime attach publishes no manager membership or session");
  check(omni_connection_runtime_count(&env.crt) == 0u &&
            !env.rt_entries[0].occupied && env.rt_entries[0].connection == NULL &&
            omni_connection_session_state(&env.rt_entries[0].session) ==
                OMNI_CONNECTION_SESSION_NEW,
        "failed runtime attach rolls back its session and runtime slot");
  check(omni_connection_reactor_count(&env.adapter) == 1u &&
            omni_connection_registry_count(&env.registry) == 1u,
        "rollback preserves the pre-existing external registration");
  check(omni_connection_reactor_dispatch(&env.adapter, external_token,
                                         OMNI_POLLER_INTEREST_READ).status ==
            OMNI_CONNECTION_REACTOR_OK,
        "pre-existing external token remains live after rollback");

  check(omni_connection_reactor_detach(&env.adapter, &connection).status ==
            OMNI_CONNECTION_REACTOR_OK,
        "external registration detached after rollback assertions");
  check(omni_connection_manager_count(&env.mgr) == 0u &&
            omni_connection_runtime_count(&env.crt) == 0u &&
            omni_connection_reactor_count(&env.adapter) == 0u,
        "rollback cleanup leaves all membership counts zero");
  omni_connection_destroy(&connection);
  omni_accepted_destroy(&accepted);
  (void)close(client_fd);
  omni_listener_destroy(&listener);
  cleanup_env(&env);
}

static void test_memory_size(void) {
  struct omni_connection_manager mgr={0};
  printf("# sizeof(manager)=%zu # sizeof(manager_entry)=%zu # sizeof(config)=%zu\n",
         sizeof(mgr), sizeof(struct omni_connection_manager_entry),
         sizeof(struct omni_connection_manager_config));
  check(sizeof(mgr)>0 && sizeof(mgr)<1024, "manager size bounded <1K");
  check(sizeof(struct omni_connection_manager_entry) < 64, "manager entry small <64");
  check(sizeof(struct omni_connection_manager_entry) >= sizeof(void*)+sizeof(uint64_t), "manager entry contains connection+token");
  check(sizeof(struct omni_connection_manager_config) > 0u &&
            sizeof(struct omni_connection_manager_config) < 64u,
        "manager config size bounded <64");
  /* capacity formula check */
  size_t cap = MGR_CAP;
  size_t total = sizeof(mgr) + cap * sizeof(struct omni_connection_manager_entry);
  check(total == sizeof(mgr) + MGR_CAP * sizeof(struct omni_connection_manager_entry), "capacity formula holds");
  printf("# capacity %zu total %zu formula: sizeof(manager)+capacity*sizeof(entry)\n", cap, total);
}

int main(void) {
  test_inert_state();
  test_valid_init();
  test_invalid_init();
  test_add_remove_success();
  test_duplicate_add();
  test_capacity_full();
  test_stale_slot_reuse();
  test_destroy_cleanup();
  test_runtime_detach();
  test_reactor_registration_cleanup();
  test_descriptor_preservation();
  test_connection_ownership_preservation();
  test_repeated_cycles();
  test_thousand_operation_stress();
  test_fd_census();
  test_destroy_multiple_active();
  test_no_add_after_stopping();
  test_failed_add_unchanged();
  test_failed_runtime_attach_rollback();
  test_memory_size();
  printf("---\n%d checks, %d failures\n", check_count, failure_count);
  return failure_count==0?0:1;
}
