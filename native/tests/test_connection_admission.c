/*
 * OmniRoute native backend — bounded connection admission tests (Task 029).
 *
 * Loopback-only integration tests exercise listener acceptance, ownership
 * transfer, manager attachment, release, bounded drain, and fixed storage.
 *
 * Tests may use client-side socket syscalls. The production admission source
 * is independently checked by the network/heap boundary gate.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "omniroute/accepted.h"
#include "omniroute/connection_admission.h"
#include "omniroute/connection_reactor.h"
#include "omniroute/connection_runtime.h"
#include "omniroute/event_loop.h"
#include "omniroute/listener.h"
#include "omniroute/poller.h"
#include "omniroute/reactor.h"
#include "omniroute/registry.h"
#include "omniroute/runtime.h"

#define ADMISSION_CAP 3u
#define MANAGER_CAP 4u
#define REG_CAP 8u
#define REACTOR_CAP 8u
#define POLLER_CAP 8u
#define BUFFER_CAP 64u

static size_t check_count = 0u;
static size_t failure_count = 0u;
static size_t callback_count = 0u;

static void check(bool condition, const char *name) {
  ++check_count;
  if (condition) {
    printf("ok - %s\n", name);
  } else {
    ++failure_count;
    printf("NOT OK - %s\n", name);
  }
}

static void quiet_check(bool condition, const char *name) {
  ++check_count;
  if (!condition) {
    ++failure_count;
    printf("NOT OK - %s\n", name);
  }
}

struct test_env {
  struct omni_poller poller;
  struct pollfd poller_fds[POLLER_CAP];
  uint64_t poller_tokens[POLLER_CAP];
  struct omni_reactor reactor;
  struct omni_reactor_registration reactor_registrations[REACTOR_CAP];
  struct omni_poller_event reactor_events[REACTOR_CAP];
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot registry_slots[REG_CAP];
  struct omni_connection_reactor adapter;

  struct omni_runtime loop_runtime;
  struct omni_listener loop_listener;
  struct omni_connection_registry loop_registry;
  struct omni_reactor loop_reactor;
  struct pollfd loop_poller_fds[POLLER_CAP];
  uint64_t loop_poller_tokens[POLLER_CAP];
  struct omni_connection_registry_slot loop_registry_slots[REG_CAP];
  struct omni_reactor_registration loop_reactor_registrations[REACTOR_CAP];
  struct omni_poller_event loop_events[REACTOR_CAP];
  struct omni_event_loop event_loop;

  struct omni_connection_runtime connection_runtime;
  struct omni_connection_runtime_entry runtime_entries[MANAGER_CAP];
  struct omni_connection_manager manager;
  struct omni_connection_manager_entry manager_entries[MANAGER_CAP];

  struct omni_listener listener;
  struct omni_connection_admission admission;
  struct omni_connection_admission_slot admission_slots[ADMISSION_CAP];
  unsigned char connection_receive[ADMISSION_CAP * BUFFER_CAP];
  unsigned char session_receive[ADMISSION_CAP * BUFFER_CAP];
  unsigned char session_send[ADMISSION_CAP * BUFFER_CAP];
};

struct external_connection {
  struct omni_connection connection;
  unsigned char connection_receive[BUFFER_CAP];
  unsigned char session_receive[BUFFER_CAP];
  unsigned char session_send[BUFFER_CAP];
  int client_fd;
  int server_fd;
  uint64_t token;
};

static void readiness_callback(struct omni_connection *connection, uint64_t token,
                               uint32_t events, void *context) {
  (void)connection;
  (void)token;
  (void)events;
  (void)context;
  callback_count += 1u;
}

static bool init_env(struct test_env *env, size_t runtime_capacity,
                     size_t manager_capacity) {
  struct omni_runtime_config runtime_config = {0};
  struct omni_event_loop_config event_loop_config = {0};
  struct omni_connection_runtime_config connection_runtime_config = {0};
  struct omni_connection_manager_config manager_config = {0};

  memset(env, 0, sizeof(*env));
  omni_reactor_make_inert(&env->reactor);
  omni_connection_registry_make_inert(&env->registry);
  omni_connection_reactor_make_inert(&env->adapter);
  omni_runtime_make_inert(&env->loop_runtime);
  omni_event_loop_make_inert(&env->event_loop);
  omni_connection_runtime_make_inert(&env->connection_runtime);
  omni_connection_manager_make_inert(&env->manager);
  omni_listener_make_inert(&env->listener);
  omni_connection_admission_make_inert(&env->admission);
  for (size_t i = 0u; i < ADMISSION_CAP; ++i) {
    omni_connection_admission_slot_make_inert(&env->admission_slots[i]);
  }

  if (omni_poller_init_borrowed(&env->poller, env->poller_fds,
                                env->poller_tokens, POLLER_CAP).status != OMNI_POLLER_OK) {
    return false;
  }
  if (omni_reactor_init(&env->reactor, &env->poller, env->reactor_registrations,
                        env->reactor_events, REACTOR_CAP).status != OMNI_REACTOR_OK) {
    return false;
  }
  if (omni_connection_registry_init(&env->registry, env->registry_slots,
                                    REG_CAP).status != OMNI_CONNECTION_REGISTRY_OK) {
    return false;
  }
  if (omni_connection_reactor_init(&env->adapter, &env->reactor, &env->registry,
                                   readiness_callback, NULL).status !=
      OMNI_CONNECTION_REACTOR_OK) {
    return false;
  }

  omni_listener_make_inert(&env->loop_listener);
  omni_connection_registry_make_inert(&env->loop_registry);
  omni_reactor_make_inert(&env->loop_reactor);
  runtime_config.listener = &env->loop_listener;
  runtime_config.registry = &env->loop_registry;
  runtime_config.reactor = &env->loop_reactor;
  runtime_config.registry_slots = env->loop_registry_slots;
  runtime_config.registry_capacity = REG_CAP;
  runtime_config.poller_fds = env->loop_poller_fds;
  runtime_config.poller_tokens = env->loop_poller_tokens;
  runtime_config.poller_capacity = POLLER_CAP;
  runtime_config.reactor_registrations = env->loop_reactor_registrations;
  runtime_config.reactor_events = env->loop_events;
  runtime_config.reactor_capacity = REACTOR_CAP;
  runtime_config.listener_address = "127.0.0.1";
  runtime_config.listener_port = 0u;
  if (omni_runtime_init(&env->loop_runtime, &runtime_config).status != OMNI_RUNTIME_OK) {
    return false;
  }
  event_loop_config.runtime = &env->loop_runtime;
  event_loop_config.timeout_ms = 0;
  if (omni_event_loop_init(&env->event_loop, &event_loop_config).status !=
      OMNI_EVENT_LOOP_OK) {
    return false;
  }

  connection_runtime_config.registry = &env->registry;
  connection_runtime_config.adapter = &env->adapter;
  connection_runtime_config.event_loop = &env->event_loop;
  connection_runtime_config.entries = env->runtime_entries;
  connection_runtime_config.capacity = runtime_capacity;
  if (omni_connection_runtime_init(&env->connection_runtime,
                                   &connection_runtime_config).status !=
      OMNI_CONNECTION_RUNTIME_OK) {
    return false;
  }
  manager_config.runtime = &env->connection_runtime;
  manager_config.entries = env->manager_entries;
  manager_config.capacity = manager_capacity;
  if (omni_connection_manager_init(&env->manager, &manager_config).status !=
      OMNI_CONNECTION_MANAGER_OK) {
    return false;
  }
  if (omni_listener_init(&env->listener, "127.0.0.1", 0u).status != OMNI_LISTENER_OK) {
    return false;
  }
  return true;
}

static struct omni_connection_admission_config admission_config(
    struct test_env *env, size_t capacity) {
  struct omni_connection_admission_config config = {0};
  config.listener = &env->listener;
  config.manager = &env->manager;
  config.slots = env->admission_slots;
  config.slots_bytes = sizeof(env->admission_slots);
  config.capacity = capacity;
  config.connection_receive.storage = env->connection_receive;
  config.connection_receive.storage_bytes = sizeof(env->connection_receive);
  config.connection_receive.per_connection_capacity = BUFFER_CAP;
  config.session_receive.storage = env->session_receive;
  config.session_receive.storage_bytes = sizeof(env->session_receive);
  config.session_receive.per_connection_capacity = BUFFER_CAP;
  config.session_send.storage = env->session_send;
  config.session_send.storage_bytes = sizeof(env->session_send);
  config.session_send.per_connection_capacity = BUFFER_CAP;
  return config;
}

static bool init_admission(struct test_env *env, size_t capacity) {
  struct omni_connection_admission_config config = admission_config(env, capacity);
  return omni_connection_admission_init(&env->admission, &config).status ==
         OMNI_CONNECTION_ADMISSION_OK;
}

static void cleanup_env(struct test_env *env) {
  (void)omni_connection_admission_destroy(&env->admission);
  omni_connection_manager_destroy(&env->manager);
  omni_connection_runtime_destroy(&env->connection_runtime);
  omni_event_loop_destroy(&env->event_loop);
  omni_runtime_destroy(&env->loop_runtime);
  omni_connection_reactor_destroy(&env->adapter);
  omni_connection_registry_destroy(&env->registry);
  omni_reactor_destroy(&env->reactor);
  omni_poller_destroy(&env->poller);
  omni_listener_destroy(&env->listener);
}

static int open_client(uint16_t port) {
  struct sockaddr_in peer;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port = htons(port);
  peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (const struct sockaddr *)&peer, (socklen_t)sizeof(peer)) != 0) {
    (void)close(fd);
    return -1;
  }
  return fd;
}

static bool fd_is_open(int fd) {
  if (fd < 0) return false;
  errno = 0;
  return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

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

static bool add_external_connection(struct test_env *env,
                                    struct external_connection *external) {
  struct omni_accepted accepted;
  struct omni_accept_result accept_result;
  struct omni_connection_config connection_config = {0};
  struct omni_connection_result connection_result;
  struct omni_connection_manager_attach_config attach_config = {0};
  struct omni_connection_manager_result manager_result;

  memset(external, 0, sizeof(*external));
  external->client_fd = open_client(omni_listener_port(&env->listener));
  if (external->client_fd < 0) return false;
  omni_accepted_make_inert(&accepted);
  accept_result = omni_accept_once(&env->listener, &accepted);
  if (accept_result.status != OMNI_ACCEPT_OK) {
    (void)close(external->client_fd);
    external->client_fd = -1;
    return false;
  }
  omni_connection_make_inert(&external->connection);
  connection_config.receive_storage = external->connection_receive;
  connection_config.receive_capacity = sizeof(external->connection_receive);
  connection_config.poller_token = 0u;
  connection_config.poller_interests = OMNI_POLLER_INTEREST_READ;
  connection_result = omni_connection_init(&external->connection, &connection_config);
  if (connection_result.status != OMNI_CONNECTION_OK) {
    omni_accepted_destroy(&accepted);
    (void)close(external->client_fd);
    external->client_fd = -1;
    return false;
  }
  connection_result = omni_connection_from_accepted(&external->connection, &accepted);
  if (connection_result.status != OMNI_CONNECTION_OK ||
      omni_accepted_is_live(&accepted)) {
    omni_accepted_destroy(&accepted);
    omni_connection_destroy(&external->connection);
    (void)close(external->client_fd);
    external->client_fd = -1;
    return false;
  }
  external->server_fd = omni_connection_fd(&external->connection);
  attach_config.connection = &external->connection;
  attach_config.receive_storage = external->session_receive;
  attach_config.receive_capacity = sizeof(external->session_receive);
  attach_config.send_storage = external->session_send;
  attach_config.send_capacity = sizeof(external->session_send);
  manager_result = omni_connection_manager_add(&env->manager, &attach_config);
  if (manager_result.status != OMNI_CONNECTION_MANAGER_OK) {
    omni_connection_destroy(&external->connection);
    (void)close(external->client_fd);
    external->client_fd = -1;
    return false;
  }
  external->token = manager_result.token;
  return true;
}

static bool remove_external_connection(struct test_env *env,
                                       struct external_connection *external) {
  bool okay = omni_connection_manager_remove(&env->manager,
                                             &external->connection).status ==
              OMNI_CONNECTION_MANAGER_OK;
  omni_connection_destroy(&external->connection);
  if (external->client_fd >= 0) {
    okay = close(external->client_fd) == 0 && okay;
    external->client_fd = -1;
  }
  return okay;
}

static void test_empty_and_single_admission(void) {
  struct test_env env;
  struct omni_connection_admission_result result;
  int baseline;
  int client;
  int server_fd;
  struct omni_connection_admission_identity identity;

  check(init_env(&env, MANAGER_CAP, MANAGER_CAP), "initialize admission test dependencies");
  omni_connection_admission_make_inert(NULL);
  check(omni_connection_admission_state(NULL) == OMNI_CONNECTION_ADMISSION_NEW,
        "NULL admission reports NEW");
  check(omni_connection_admission_once(&env.admission).status ==
            OMNI_CONNECTION_ADMISSION_ERR_STATE,
        "once before init fails explicitly");
  check(init_admission(&env, ADMISSION_CAP), "initialize admission slots and buffers");
  check(omni_connection_admission_state(&env.admission) ==
            OMNI_CONNECTION_ADMISSION_INITIALIZED,
        "admission begins INITIALIZED");
  check(omni_connection_admission_capacity(&env.admission) == ADMISSION_CAP,
        "admission reports fixed capacity");

  baseline = fd_census();
  result = omni_connection_admission_once(&env.admission);
  check(result.status == OMNI_CONNECTION_ADMISSION_DRAINED && result.attempts == 1u &&
            result.admitted == 0u,
        "empty listener returns DRAINED after one accept attempt");
  check(env.admission_slots[0].connection.state == OMNI_CONNECTION_INERT &&
            !env.admission_slots[0].occupied &&
            omni_connection_admission_count(&env.admission) == 0u,
        "drained listener leaves slot untouched");
  check(fd_census() == baseline, "drained listener creates no descriptor leak");

  client = open_client(omni_listener_port(&env.listener));
  check(client >= 0, "loopback client connects to admission listener");
  result = omni_connection_admission_once(&env.admission);
  check(result.status == OMNI_CONNECTION_ADMISSION_OK && result.admitted == 1u &&
            result.attempts == 1u && result.identity.slot_index == 0u &&
            result.identity.token != 0u,
        "once admits one client and returns manager identity");
  identity = result.identity;
  server_fd = omni_connection_fd(&env.admission_slots[identity.slot_index].connection);
  check(omni_connection_state(&env.admission_slots[identity.slot_index].connection) ==
            OMNI_CONNECTION_OPEN && fd_is_open(server_fd) && fd_is_open(client),
        "admitted connection owns a live descriptor");
  check(omni_connection_admission_count(&env.admission) == 1u &&
            omni_connection_manager_count(&env.manager) == 1u &&
            omni_connection_runtime_count(&env.connection_runtime) == 1u &&
            omni_reactor_count(&env.reactor) == 1u,
        "admission publishes only after manager and reactor attach");
  check(omni_connection_manager_find(&env.manager,
                                    &env.admission_slots[identity.slot_index].connection) != NULL,
        "manager contains the admitted connection");

  result = omni_connection_admission_release(&env.admission, identity);
  check(result.status == OMNI_CONNECTION_ADMISSION_OK &&
            omni_connection_admission_count(&env.admission) == 0u &&
            omni_connection_manager_count(&env.manager) == 0u &&
            omni_reactor_count(&env.reactor) == 0u,
        "release detaches manager and reactor before clearing slot");
  check(!fd_is_open(server_fd) && fd_is_open(client),
        "release closes only the accepted connection descriptor");
  result = omni_connection_admission_release(&env.admission, identity);
  check(result.status == OMNI_CONNECTION_ADMISSION_ERR_NOT_FOUND,
        "repeated stale release returns NOT_FOUND");
  check(close(client) == 0, "client descriptor remains caller-owned");

  client = open_client(omni_listener_port(&env.listener));
  result = omni_connection_admission_once(&env.admission);
  check(client >= 0 && result.status == OMNI_CONNECTION_ADMISSION_OK,
        "admit one connection for destroy-with-one test");
  server_fd = omni_connection_fd(
      &env.admission_slots[result.identity.slot_index].connection);
  check(omni_connection_admission_destroy(&env.admission).status ==
            OMNI_CONNECTION_ADMISSION_OK && !fd_is_open(server_fd) && fd_is_open(client),
        "destroy with one active connection releases only the accepted FD");
  check(close(client) == 0, "close caller client after one-connection destroy");

  cleanup_env(&env);
  check(omni_connection_admission_state(&env.admission) ==
            OMNI_CONNECTION_ADMISSION_CLOSED,
        "destroy leaves admission CLOSED");
  check(omni_connection_admission_destroy(&env.admission).status ==
            OMNI_CONNECTION_ADMISSION_OK,
        "repeated destroy is safe");
}

static void test_bounded_multiple_admission_and_slot_capacity(void) {
  struct test_env env;
  struct omni_connection_admission_identity identities[ADMISSION_CAP];
  struct omni_connection_admission_identity initial_identity;
  struct omni_connection_admission_result result;
  int clients[ADMISSION_CAP];

  check(init_env(&env, MANAGER_CAP, MANAGER_CAP), "initialize bounded-drain dependencies");
  check(init_admission(&env, ADMISSION_CAP), "initialize capacity-three admission");
  for (size_t i = 0u; i < 2u; ++i) {
    clients[i] = open_client(omni_listener_port(&env.listener));
    check(clients[i] >= 0, "queue loopback client for bounded drain");
  }
  result = omni_connection_admission_drain(&env.admission, 1u, identities,
                                           ADMISSION_CAP);
  check(result.status == OMNI_CONNECTION_ADMISSION_LIMIT_REACHED &&
            result.attempts == 1u && result.admitted == 1u,
        "drain admits no more than its finite attempt limit");
  check(omni_connection_admission_count(&env.admission) == 1u &&
            omni_connection_manager_count(&env.manager) == 1u &&
            identities[0].slot_index == 0u,
        "drain returns identities for every successful admission");
  initial_identity = identities[0];
  result = omni_connection_admission_drain(&env.admission, ADMISSION_CAP,
                                           identities, ADMISSION_CAP);
  check(result.status == OMNI_CONNECTION_ADMISSION_DRAINED &&
            result.attempts == 2u && result.admitted == 1u,
        "later drain preserves progress then reports empty listener");
  check(omni_connection_admission_count(&env.admission) == 2u &&
            omni_connection_manager_count(&env.manager) == 2u,
        "multiple admissions remain within both capacities");
  check(omni_connection_admission_release(&env.admission, identities[0]).status ==
            OMNI_CONNECTION_ADMISSION_OK,
        "release identity from later bounded drain");
  check(omni_connection_admission_release(&env.admission, initial_identity).status ==
            OMNI_CONNECTION_ADMISSION_OK,
        "release identity from first bounded drain");
  check(close(clients[0]) == 0 && close(clients[1]) == 0,
        "close caller-owned bounded-drain clients");
  cleanup_env(&env);

  check(init_env(&env, MANAGER_CAP, MANAGER_CAP), "initialize slot-full dependencies");
  check(init_admission(&env, 1u), "initialize one-slot admission");
  clients[0] = open_client(omni_listener_port(&env.listener));
  clients[1] = open_client(omni_listener_port(&env.listener));
  check(clients[0] >= 0 && clients[1] >= 0, "queue two clients for one admission slot");
  result = omni_connection_admission_drain(&env.admission, 2u, identities, 2u);
  check(result.status == OMNI_CONNECTION_ADMISSION_SLOT_FULL &&
            result.attempts == 1u && result.admitted == 1u,
        "slot exhaustion stops drain before accepting another client");
  check(omni_connection_admission_release(&env.admission, identities[0]).status ==
            OMNI_CONNECTION_ADMISSION_OK,
        "release frees admission slot");
  result = omni_connection_admission_once(&env.admission);
  check(result.status == OMNI_CONNECTION_ADMISSION_OK && result.identity.slot_index == 0u,
        "queued client is admitted after slot reuse");
  check(omni_connection_admission_release(&env.admission, result.identity).status ==
            OMNI_CONNECTION_ADMISSION_OK,
        "release reused slot connection");
  check(close(clients[0]) == 0 && close(clients[1]) == 0,
        "caller closes both one-slot clients");
  cleanup_env(&env);
}

static void test_invalid_configuration_and_lifecycle(void) {
  struct test_env env;
  struct omni_connection_admission_config config;
  struct omni_connection_admission_result result;

  check(init_env(&env, MANAGER_CAP, MANAGER_CAP), "initialize invalid-config dependencies");
  config = admission_config(&env, ADMISSION_CAP);
  check(omni_connection_admission_init(NULL, &config).status ==
            OMNI_CONNECTION_ADMISSION_ERR_INVALID,
        "NULL admission init rejected");
  check(omni_connection_admission_init(&env.admission, NULL).status ==
            OMNI_CONNECTION_ADMISSION_ERR_INVALID,
        "NULL admission config rejected");

  config.listener = NULL;
  check(omni_connection_admission_init(&env.admission, &config).status ==
            OMNI_CONNECTION_ADMISSION_ERR_INVALID,
        "NULL listener rejected");
  config = admission_config(&env, ADMISSION_CAP);
  config.manager = NULL;
  check(omni_connection_admission_init(&env.admission, &config).status ==
            OMNI_CONNECTION_ADMISSION_ERR_INVALID,
        "NULL manager rejected");
  config = admission_config(&env, 0u);
  check(omni_connection_admission_init(&env.admission, &config).status ==
            OMNI_CONNECTION_ADMISSION_ERR_INVALID,
        "zero admission capacity rejected");
  config = admission_config(&env, ADMISSION_CAP);
  config.capacity = MANAGER_CAP + 1u;
  check(omni_connection_admission_init(&env.admission, &config).status ==
            OMNI_CONNECTION_ADMISSION_ERR_INVALID,
        "admission capacity above manager capacity rejected");
  config = admission_config(&env, ADMISSION_CAP);
  config.slots_bytes = 1u;
  check(omni_connection_admission_init(&env.admission, &config).status ==
            OMNI_CONNECTION_ADMISSION_ERR_INVALID,
        "undersized slot storage rejected");
  config = admission_config(&env, ADMISSION_CAP);
  config.connection_receive.storage = NULL;
  check(omni_connection_admission_init(&env.admission, &config).status ==
            OMNI_CONNECTION_ADMISSION_ERR_INVALID,
        "NULL connection receive storage rejected");
  config = admission_config(&env, ADMISSION_CAP);
  config.session_send.storage_bytes = 1u;
  check(omni_connection_admission_init(&env.admission, &config).status ==
            OMNI_CONNECTION_ADMISSION_ERR_INVALID,
        "undersized session send pool rejected");
  config = admission_config(&env, 2u);
  config.connection_receive.per_connection_capacity = SIZE_MAX / 2u + 1u;
  config.connection_receive.storage_bytes = SIZE_MAX;
  check(omni_connection_admission_init(&env.admission, &config).status ==
            OMNI_CONNECTION_ADMISSION_ERR_INVALID,
        "receive-pool multiplication overflow rejected");
  config = admission_config(&env, ADMISSION_CAP);
  config.session_send.storage = config.session_receive.storage;
  check(omni_connection_admission_init(&env.admission, &config).status ==
            OMNI_CONNECTION_ADMISSION_ERR_INVALID,
        "overlapping receive and send pools rejected");
  config = admission_config(&env, ADMISSION_CAP);
  env.admission_slots[0].occupied = true;
  check(omni_connection_admission_init(&env.admission, &config).status ==
            OMNI_CONNECTION_ADMISSION_ERR_INVALID,
        "occupied caller slot rejected during init");
  env.admission_slots[0].occupied = false;
  check(omni_connection_admission_state(&env.admission) ==
            OMNI_CONNECTION_ADMISSION_NEW,
        "failed init preserves NEW admission state");

  check(omni_connection_admission_init(&env.admission, &config).status ==
            OMNI_CONNECTION_ADMISSION_OK,
        "valid init succeeds after rejected configurations");
  check(omni_connection_admission_init(&env.admission, &config).status ==
            OMNI_CONNECTION_ADMISSION_ERR_STATE,
        "initializing an already-live admission fails");
  check(omni_connection_admission_drain(&env.admission, 0u, NULL, 0u).status ==
            OMNI_CONNECTION_ADMISSION_ERR_INVALID,
        "zero drain budget rejected");
  check(omni_connection_admission_drain(&env.admission, 2u, NULL, 2u).status ==
            OMNI_CONNECTION_ADMISSION_ERR_INVALID,
        "missing drain identity output rejected");
  check(omni_connection_admission_drain(&env.admission, 2u,
                                        (struct omni_connection_admission_identity[1]){{0}},
                                        1u).status == OMNI_CONNECTION_ADMISSION_ERR_INVALID,
        "undersized drain identity output rejected");
  result = omni_connection_admission_destroy(&env.admission);
  check(result.status == OMNI_CONNECTION_ADMISSION_OK &&
            omni_connection_admission_state(&env.admission) ==
                OMNI_CONNECTION_ADMISSION_CLOSED,
        "destroy empty initialized admission closes it");
  check(omni_connection_admission_once(&env.admission).status ==
            OMNI_CONNECTION_ADMISSION_ERR_STATE,
        "admission after terminal destroy is rejected");
  check(omni_connection_admission_destroy(&env.admission).status ==
            OMNI_CONNECTION_ADMISSION_OK,
        "destroy empty/closed admission remains safe");
  cleanup_env(&env);
}

static void test_manager_full_and_attach_rollback(void) {
  struct test_env env;
  struct external_connection external;
  struct omni_connection_admission_result result;
  struct omni_accepted pending;
  struct omni_accept_result accept_result;
  int client;
  int descriptors_before;

  check(init_env(&env, 1u, 1u), "initialize one-entry manager capacity test");
  check(init_admission(&env, 1u), "initialize one-entry admission capacity test");
  check(add_external_connection(&env, &external), "fill manager with external connection");
  client = open_client(omni_listener_port(&env.listener));
  check(client >= 0, "queue client while manager is full");
  result = omni_connection_admission_once(&env.admission);
  check(result.status == OMNI_CONNECTION_ADMISSION_MANAGER_FULL &&
            result.attempts == 0u && result.admitted == 0u,
        "manager full stops before accepting pending client");
  omni_accepted_make_inert(&pending);
  accept_result = omni_accept_once(&env.listener, &pending);
  check(accept_result.status == OMNI_ACCEPT_OK,
        "client remains queued when manager was full");
  omni_accepted_destroy(&pending);
  check(remove_external_connection(&env, &external),
        "remove external full-capacity connection");
  check(close(client) == 0, "close pending manager-full client");
  cleanup_env(&env);

  check(init_env(&env, 1u, MANAGER_CAP),
        "initialize smaller runtime for attachment rollback test");
  check(init_admission(&env, 2u), "initialize admission above runtime capacity");
  check(add_external_connection(&env, &external), "occupy sole runtime attachment");
  client = open_client(omni_listener_port(&env.listener));
  check(client >= 0, "queue client for runtime attach failure");
  descriptors_before = fd_census();
  result = omni_connection_admission_once(&env.admission);
  check(result.status == OMNI_CONNECTION_ADMISSION_MANAGER_FULL &&
            result.attempts == 1u && result.admitted == 0u &&
            result.manager_status == OMNI_CONNECTION_MANAGER_ERR_FULL,
        "runtime-full attach failure is surfaced after one accept");
  check(omni_connection_admission_count(&env.admission) == 0u &&
            omni_connection_manager_count(&env.manager) == 1u &&
            omni_connection_runtime_count(&env.connection_runtime) == 1u &&
            omni_reactor_count(&env.reactor) == 1u &&
            !env.admission_slots[0].occupied &&
            env.admission_slots[0].connection.state == OMNI_CONNECTION_CLOSED,
        "failed attach rolls back slot/session/registration publication");
  check(fd_census() == descriptors_before && fd_is_open(external.server_fd) &&
            fd_is_open(client),
        "failed attach closes only its newly adopted descriptor");
  check(remove_external_connection(&env, &external),
        "remove external connection after attach rollback");
  check(close(client) == 0, "close caller client after attach rollback");
  cleanup_env(&env);

  {
    struct omni_connection dummy_connections[REG_CAP];
    struct omni_connection_registry_handle handles[REG_CAP];
    size_t registered = 0u;

    check(init_env(&env, MANAGER_CAP, MANAGER_CAP),
          "initialize registry-full session rollback dependencies");
    check(init_admission(&env, ADMISSION_CAP),
          "initialize admission for registry-full attach rollback");
    for (size_t i = 0u; i < REG_CAP; ++i) {
      struct omni_connection_registry_result registry_result;
      omni_connection_make_inert(&dummy_connections[i]);
      registry_result = omni_connection_registry_add(&env.registry, &dummy_connections[i]);
      if (registry_result.status != OMNI_CONNECTION_REGISTRY_OK) break;
      handles[registered] = registry_result.handle;
      ++registered;
    }
    check(registered == REG_CAP && omni_connection_registry_count(&env.registry) == REG_CAP,
          "fill registry while leaving manager/runtime/reactor slots free");
    if (registered == REG_CAP) {
      client = open_client(omni_listener_port(&env.listener));
      check(client >= 0, "queue client for post-session adapter failure");
      descriptors_before = fd_census();
      result = omni_connection_admission_once(&env.admission);
      check(result.status == OMNI_CONNECTION_ADMISSION_MANAGER_FULL &&
                result.manager_status == OMNI_CONNECTION_MANAGER_ERR_FULL &&
                result.attempts == 1u && result.admitted == 0u,
            "registry capacity failure after session setup is reported");
      check(omni_connection_admission_count(&env.admission) == 0u &&
                omni_connection_manager_count(&env.manager) == 0u &&
                omni_connection_runtime_count(&env.connection_runtime) == 0u &&
                omni_connection_registry_count(&env.registry) == REG_CAP &&
                omni_reactor_count(&env.reactor) == 0u &&
                env.manager.state == OMNI_CONNECTION_MANAGER_INITIALIZED &&
                !env.admission_slots[0].occupied &&
                env.admission_slots[0].connection.state == OMNI_CONNECTION_CLOSED,
            "session/setup rollback preserves only the independent registry entries");
      for (size_t i = 0u; i < MANAGER_CAP; ++i) {
        check(!env.runtime_entries[i].occupied &&
              env.runtime_entries[i].session.state == OMNI_CONNECTION_SESSION_NEW,
              "failed adapter attach releases its runtime session slot");
      }
      check(fd_census() == descriptors_before && fd_is_open(client),
            "post-session rollback closes only the newly accepted descriptor");
      check(close(client) == 0, "close client after post-session rollback");
    }
    for (size_t i = 0u; i < registered; ++i) {
      check(omni_connection_registry_remove(&env.registry, handles[i]).status ==
                OMNI_CONNECTION_REGISTRY_OK,
            "remove synthetic registry-capacity entry");
    }
    cleanup_env(&env);
  }
}

static void test_connection_preparation_rollback(void) {
  struct test_env env;
  struct omni_connection_admission_result result;
  void *receive_storage;
  int client;
  int before;

  check(init_env(&env, MANAGER_CAP, MANAGER_CAP),
        "initialize connection-preparation rollback dependencies");
  check(init_admission(&env, ADMISSION_CAP),
        "initialize connection-preparation rollback admission");
  receive_storage = env.admission.connection_receive_storage;
  env.admission.connection_receive_storage = NULL;
  client = open_client(omni_listener_port(&env.listener));
  before = fd_census();
  result = omni_connection_admission_once(&env.admission);
  check(result.status == OMNI_CONNECTION_ADMISSION_ERR_CONNECTION &&
            result.attempts == 1u && result.admitted == 0u &&
            result.connection_status == OMNI_CONNECTION_ERR_INVALID,
        "connection preparation failure is reported after acceptance");
  check(omni_connection_admission_count(&env.admission) == 0u &&
            omni_connection_manager_count(&env.manager) == 0u &&
            omni_reactor_count(&env.reactor) == 0u &&
            !env.admission_slots[0].occupied &&
            env.admission_slots[0].connection.state == OMNI_CONNECTION_CLOSED &&
            fd_census() == before,
        "preparation rollback destroys accepted owner and leaves no partial membership");
  env.admission.connection_receive_storage = receive_storage;
  check(client >= 0 && close(client) == 0,
        "client endpoint remains caller-owned after preparation rollback");
  client = open_client(omni_listener_port(&env.listener));
  result = omni_connection_admission_once(&env.admission);
  check(client >= 0 && result.status == OMNI_CONNECTION_ADMISSION_OK &&
            result.identity.slot_index == 0u,
        "connection slot is reusable after preparation rollback");
  check(omni_connection_admission_release(&env.admission, result.identity).status ==
            OMNI_CONNECTION_ADMISSION_OK && close(client) == 0,
        "release connection admitted after rollback");
  cleanup_env(&env);
}

static void test_slot_reuse_and_stale_identity(void) {
  struct test_env env;
  struct omni_connection_admission_result first;
  struct omni_connection_admission_result second;
  struct omni_connection_admission_result released;
  size_t callbacks_before;
  int client_a;
  int client_b;

  check(init_env(&env, MANAGER_CAP, MANAGER_CAP), "initialize stale-identity dependencies");
  check(init_admission(&env, 1u), "initialize single reusable slot");
  client_a = open_client(omni_listener_port(&env.listener));
  first = omni_connection_admission_once(&env.admission);
  check(client_a >= 0 && first.status == OMNI_CONNECTION_ADMISSION_OK,
        "admit first connection for stale-identity check");
  check(omni_connection_admission_release(&env.admission, first.identity).status ==
            OMNI_CONNECTION_ADMISSION_OK,
        "release first generation");
  check(close(client_a) == 0, "close first stale-identity client");

  client_b = open_client(omni_listener_port(&env.listener));
  second = omni_connection_admission_once(&env.admission);
  check(client_b >= 0 && second.status == OMNI_CONNECTION_ADMISSION_OK &&
            second.identity.slot_index == first.identity.slot_index &&
            second.identity.token != first.identity.token,
        "reused slot gets a different existing generation token");
  callbacks_before = callback_count;
  check(omni_connection_reactor_dispatch(&env.adapter, first.identity.token, 0u).status ==
            OMNI_CONNECTION_REACTOR_IGNORED && callback_count == callbacks_before,
        "stale token cannot dispatch to reused connection");
  released = omni_connection_admission_release(&env.admission, first.identity);
  check(released.status == OMNI_CONNECTION_ADMISSION_ERR_NOT_FOUND &&
            omni_connection_admission_count(&env.admission) == 1u &&
            fd_is_open(omni_connection_fd(&env.admission_slots[second.identity.slot_index].connection)),
        "stale identity cannot release the new slot occupant");
  check(omni_connection_admission_release(&env.admission, second.identity).status ==
            OMNI_CONNECTION_ADMISSION_OK,
        "release current slot generation");
  check(close(client_b) == 0, "close second stale-identity client");
  cleanup_env(&env);
}

static void test_stop_destroy_and_borrowed_dependencies(void) {
  struct test_env env;
  struct external_connection external;
  struct omni_connection_admission_identity first;
  struct omni_connection_admission_result result;
  struct omni_accepted queued;
  struct omni_accept_result accept_result;
  int clients[4];
  int listener_fd;

  check(init_env(&env, MANAGER_CAP, MANAGER_CAP), "initialize stop/destroy dependencies");
  check(init_admission(&env, ADMISSION_CAP), "initialize stop/destroy admission");
  check(add_external_connection(&env, &external), "add independent bystander connection");
  clients[0] = open_client(omni_listener_port(&env.listener));
  result = omni_connection_admission_once(&env.admission);
  first = result.identity;
  check(result.status == OMNI_CONNECTION_ADMISSION_OK && clients[0] >= 0,
        "admit first client before stop");
  clients[1] = open_client(omni_listener_port(&env.listener));
  result = omni_connection_admission_once(&env.admission);
  check(result.status == OMNI_CONNECTION_ADMISSION_OK && clients[1] >= 0,
        "admit second client before stop");
  clients[2] = open_client(omni_listener_port(&env.listener));
  result = omni_connection_admission_once(&env.admission);
  check(result.status == OMNI_CONNECTION_ADMISSION_OK && clients[2] >= 0 &&
            omni_connection_admission_count(&env.admission) == ADMISSION_CAP,
        "admit third client before stop and fill admission slots");
  clients[3] = open_client(omni_listener_port(&env.listener));
  check(clients[3] >= 0, "queue client before admission stop");
  listener_fd = omni_listener_fd(&env.listener);

  check(omni_connection_admission_stop(&env.admission).status ==
            OMNI_CONNECTION_ADMISSION_OK &&
            omni_connection_admission_state(&env.admission) ==
                OMNI_CONNECTION_ADMISSION_STOPPING,
        "stop transitions admission to STOPPING");
  check(omni_connection_admission_stop(&env.admission).status ==
            OMNI_CONNECTION_ADMISSION_OK,
        "stop is idempotent");
  result = omni_connection_admission_once(&env.admission);
  check(result.status == OMNI_CONNECTION_ADMISSION_ERR_STATE && result.attempts == 0u,
        "STOPPING rejects new admission before accept");
  check(omni_connection_admission_release(&env.admission, first).status ==
            OMNI_CONNECTION_ADMISSION_OK &&
            omni_connection_admission_count(&env.admission) == 2u,
        "release remains available while STOPPING and updates count");
  check(omni_connection_admission_destroy(&env.admission).status ==
            OMNI_CONNECTION_ADMISSION_OK,
        "destroy releases remaining active membership");
  check(omni_connection_admission_state(&env.admission) ==
            OMNI_CONNECTION_ADMISSION_CLOSED &&
            omni_connection_admission_count(&env.admission) == 0u,
        "destroy clears admission lifecycle and count");
  check(!env.admission_slots[0].occupied && !env.admission_slots[1].occupied &&
            !env.admission_slots[2].occupied &&
            env.admission_slots[0].connection.state == OMNI_CONNECTION_CLOSED &&
            env.admission_slots[1].connection.state == OMNI_CONNECTION_CLOSED &&
            env.admission_slots[2].connection.state == OMNI_CONNECTION_CLOSED,
        "destroy clears multiple active admission slots after detach");
  check(omni_connection_manager_count(&env.manager) == 1u &&
            omni_connection_runtime_count(&env.connection_runtime) == 1u &&
            omni_connection_registry_count(&env.registry) == 1u &&
            omni_reactor_count(&env.reactor) == 1u && fd_is_open(external.server_fd),
        "admission destroy leaves bystander and manager dependencies alive");
  check(fd_is_open(listener_fd) &&
            omni_event_loop_state(&env.event_loop) == OMNI_EVENT_LOOP_INITIALIZED &&
            omni_runtime_state(&env.loop_runtime) == OMNI_RUNTIME_INITIALIZED,
        "admission destroy preserves listener and event-loop dependencies");
  check(close(clients[0]) == 0 && close(clients[1]) == 0 && close(clients[2]) == 0,
        "admitted client descriptors remain caller-owned after destroy");
  omni_accepted_make_inert(&queued);
  accept_result = omni_accept_once(&env.listener, &queued);
  check(accept_result.status == OMNI_ACCEPT_OK,
        "listener still accepts its pending client after admission destroy");
  omni_accepted_destroy(&queued);
  check(close(clients[3]) == 0, "close queued caller-owned client");
  check(remove_external_connection(&env, &external), "remove bystander after admission destroy");
  cleanup_env(&env);
}

static void test_thousand_admit_release_cycles(void) {
  struct test_env env;
  struct omni_connection_admission_identity previous = {
    OMNI_CONNECTION_ADMISSION_SLOT_INVALID, 0u
  };
  int baseline;
  bool stress_ok = true;
  const size_t cycles = 1000u;

  check(init_env(&env, MANAGER_CAP, MANAGER_CAP), "initialize admission stress dependencies");
  check(init_admission(&env, 1u), "initialize one-slot admission stress pool");
  baseline = fd_census();
  for (size_t i = 0u; i < cycles; ++i) {
    int client = open_client(omni_listener_port(&env.listener));
    struct omni_connection_admission_result admitted =
        omni_connection_admission_once(&env.admission);
    bool current_ok = client >= 0 && admitted.status == OMNI_CONNECTION_ADMISSION_OK &&
                      admitted.identity.slot_index == 0u &&
                      omni_connection_admission_count(&env.admission) <=
                          omni_connection_admission_capacity(&env.admission) &&
                      omni_connection_manager_count(&env.manager) <=
                          omni_connection_manager_capacity(&env.manager) &&
                      fd_is_open(omni_connection_fd(
                          &env.admission_slots[admitted.identity.slot_index].connection));
    if (previous.token != 0u) {
      size_t callbacks_before = callback_count;
      current_ok = current_ok &&
          omni_connection_reactor_dispatch(&env.adapter, previous.token, 0u).status ==
              OMNI_CONNECTION_REACTOR_IGNORED && callback_count == callbacks_before;
    }
    if (admitted.status == OMNI_CONNECTION_ADMISSION_OK) {
      struct omni_connection_admission_result released =
          omni_connection_admission_release(&env.admission, admitted.identity);
      current_ok = current_ok && released.status == OMNI_CONNECTION_ADMISSION_OK &&
                   omni_connection_admission_count(&env.admission) == 0u &&
                   omni_connection_manager_count(&env.manager) == 0u &&
                   fd_census() == baseline + 1;
      previous = admitted.identity;
    }
    if (client >= 0) {
      current_ok = close(client) == 0 && current_ok;
    }
    current_ok = current_ok && fd_census() == baseline;
    if (!current_ok) stress_ok = false;
    quiet_check(current_ok, "stress iteration preserves bounded counts and descriptor census");
  }
  if (!stress_ok) {
    ++failure_count;
    printf("NOT OK - repeated admission/release invariants and FD census\n");
  }
  check(stress_ok, "1000 admission/release operations preserve counts, tokens, and FD census");
  cleanup_env(&env);
}

int main(void) {
  printf("connection admission unit tests\n");
  test_empty_and_single_admission();
  test_bounded_multiple_admission_and_slot_capacity();
  test_invalid_configuration_and_lifecycle();
  test_manager_full_and_attach_rollback();
  test_connection_preparation_rollback();
  test_slot_reuse_and_stale_identity();
  test_stop_destroy_and_borrowed_dependencies();
  test_thousand_admit_release_cycles();
  printf("sizeof admission=%zu slot=%zu config=%zu result=%zu\n",
         sizeof(struct omni_connection_admission),
         sizeof(struct omni_connection_admission_slot),
         sizeof(struct omni_connection_admission_config),
         sizeof(struct omni_connection_admission_result));
  printf("checks: %zu, failures: %zu\n", check_count, failure_count);
  return failure_count == 0u ? 0 : 1;
}
