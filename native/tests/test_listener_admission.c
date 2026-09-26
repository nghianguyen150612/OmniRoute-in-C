/*
 * OmniRoute native backend — bounded listener admission bridge tests (Task 030).
 *
 * Loopback-only tests cover registration, bounded Task 029 drains, stop and
 * destroy ownership, a real event-loop callback chain, and deterministic
 * admit/release stress. The production bridge source remains syscall-free.
 */

#define _POSIX_C_SOURCE 200809L

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
#include "omniroute/connection.h"
#include "omniroute/connection_admission.h"
#include "omniroute/connection_manager.h"
#include "omniroute/connection_reactor.h"
#include "omniroute/connection_runtime.h"
#include "omniroute/event_loop.h"
#include "omniroute/listener_admission.h"
#include "omniroute/runtime.h"

#define TEST_CAPACITY 4u
#define TEST_REGISTRY_CAPACITY 8u
#define TEST_REACTOR_CAPACITY 12u
#define TEST_BUFFER_CAPACITY 64u
#define TEST_STRESS_CYCLES 1000u
#define TEST_PIPE_TOKEN UINT64_C(0x0300A030)

static size_t check_count = 0u;
static size_t failure_count = 0u;

struct test_fixture {
  struct omni_runtime runtime;
  struct omni_listener runtime_listener;
  struct omni_connection_registry registry;
  struct omni_reactor reactor;
  struct omni_connection_registry_slot registry_slots[TEST_REGISTRY_CAPACITY];
  struct omni_reactor_registration reactor_registrations[TEST_REACTOR_CAPACITY];
  struct omni_poller_event reactor_events[TEST_REACTOR_CAPACITY];
  struct pollfd poller_fds[TEST_REACTOR_CAPACITY];
  uint64_t poller_tokens[TEST_REACTOR_CAPACITY];

  struct omni_event_loop event_loop;
  struct omni_connection_reactor connection_reactor;
  struct omni_connection_runtime connection_runtime;
  struct omni_connection_runtime_entry runtime_entries[TEST_CAPACITY];
  struct omni_connection_manager manager;
  struct omni_connection_manager_entry manager_entries[TEST_CAPACITY];

  struct omni_listener listener;
  struct omni_connection_admission admission;
  struct omni_connection_admission_slot admission_slots[TEST_CAPACITY];
  unsigned char connection_receive[TEST_CAPACITY * TEST_BUFFER_CAPACITY];
  unsigned char session_receive[TEST_CAPACITY * TEST_BUFFER_CAPACITY];
  unsigned char session_send[TEST_CAPACITY * TEST_BUFFER_CAPACITY];

  struct omni_listener_admission bridge;
  struct omni_connection_admission_identity identities[TEST_CAPACITY];
};

struct external_connection {
  struct omni_accepted accepted;
  struct omni_connection connection;
  unsigned char connection_receive[TEST_BUFFER_CAPACITY];
  unsigned char session_receive[TEST_BUFFER_CAPACITY];
  unsigned char session_send[TEST_BUFFER_CAPACITY];
  int read_fd;
  int write_fd;
  bool manager_added;
};

struct loop_stop_context {
  struct omni_event_loop *loop;
  struct omni_connection_manager *manager;
  size_t calls;
  enum omni_event_loop_status stop_status;
};

static void check(bool condition, const char *message) {
  ++check_count;
  if (condition) {
    (void)printf("ok - %s\n", message);
  } else {
    ++failure_count;
    (void)printf("NOT OK - %s\n", message);
  }
}

static void quiet_check(bool condition, const char *message) {
  ++check_count;
  if (!condition) {
    ++failure_count;
    (void)printf("NOT OK - %s\n", message);
  }
}

static bool fd_is_open(int fd) {
  if (fd < 0) return false;
  return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

static size_t count_open_fds(void) {
  DIR *directory = opendir("/proc/self/fd");
  struct dirent *entry = NULL;
  size_t count = 0u;

  if (directory == NULL) return SIZE_MAX;
  while ((entry = readdir(directory)) != NULL) {
    if (entry->d_name[0] != '.') count += 1u;
  }
  (void)closedir(directory);
  return count;
}

static void reactor_noop(uint64_t token, uint32_t events, void *context) {
  (void)token;
  (void)events;
  (void)context;
}

static void connection_noop(struct omni_connection *connection,
                            uint64_t token,
                            uint32_t events,
                            void *context) {
  (void)connection;
  (void)token;
  (void)events;
  (void)context;
}

static void loop_stop_when_admitted(uint64_t token, uint32_t events, void *context) {
  struct loop_stop_context *stop = (struct loop_stop_context *)context;

  (void)token;
  (void)events;
  if (stop == NULL) return;
  stop->calls += 1u;
  if (omni_connection_manager_count(stop->manager) > 0u || stop->calls >= 10u) {
    stop->stop_status = omni_event_loop_stop(stop->loop).status;
  }
}

static void fixture_prepare(struct test_fixture *fixture) {
  if (fixture == NULL) return;
  (void)memset(fixture, 0, sizeof(*fixture));
  omni_runtime_make_inert(&fixture->runtime);
  omni_listener_make_inert(&fixture->runtime_listener);
  omni_connection_registry_make_inert(&fixture->registry);
  omni_reactor_make_inert(&fixture->reactor);
  omni_event_loop_make_inert(&fixture->event_loop);
  omni_connection_reactor_make_inert(&fixture->connection_reactor);
  omni_connection_runtime_make_inert(&fixture->connection_runtime);
  omni_connection_manager_make_inert(&fixture->manager);
  omni_listener_make_inert(&fixture->listener);
  omni_connection_admission_make_inert(&fixture->admission);
  omni_listener_admission_make_inert(&fixture->bridge);
  for (size_t i = 0u; i < TEST_CAPACITY; ++i) {
    omni_connection_admission_slot_make_inert(&fixture->admission_slots[i]);
  }
}

static bool fixture_init(struct test_fixture *fixture,
                         size_t budget,
                         size_t admission_capacity,
                         size_t manager_capacity,
                         bool start_bridge) {
  struct omni_runtime_config runtime_config = {0};
  struct omni_event_loop_config loop_config = {0};
  struct omni_connection_runtime_config connection_runtime_config = {0};
  struct omni_connection_manager_config manager_config = {0};
  struct omni_connection_admission_config admission_config = {0};
  struct omni_listener_admission_config bridge_config = {0};

  fixture_prepare(fixture);

  runtime_config.listener = &fixture->runtime_listener;
  runtime_config.registry = &fixture->registry;
  runtime_config.reactor = &fixture->reactor;
  runtime_config.registry_slots = fixture->registry_slots;
  runtime_config.registry_capacity = TEST_REGISTRY_CAPACITY;
  runtime_config.poller_fds = fixture->poller_fds;
  runtime_config.poller_tokens = fixture->poller_tokens;
  runtime_config.poller_capacity = TEST_REACTOR_CAPACITY;
  runtime_config.reactor_registrations = fixture->reactor_registrations;
  runtime_config.reactor_events = fixture->reactor_events;
  runtime_config.reactor_capacity = TEST_REACTOR_CAPACITY;
  runtime_config.listener_address = "127.0.0.1";
  runtime_config.listener_port = 0u;
  if (omni_runtime_init(&fixture->runtime, &runtime_config).status != OMNI_RUNTIME_OK) {
    return false;
  }

  loop_config.runtime = &fixture->runtime;
  loop_config.timeout_ms = 0;
  if (omni_event_loop_init(&fixture->event_loop, &loop_config).status !=
      OMNI_EVENT_LOOP_OK) {
    return false;
  }

  if (omni_connection_reactor_init(&fixture->connection_reactor,
                                   &fixture->reactor,
                                   &fixture->registry,
                                   connection_noop, NULL).status !=
      OMNI_CONNECTION_REACTOR_OK) {
    return false;
  }

  connection_runtime_config.registry = &fixture->registry;
  connection_runtime_config.adapter = &fixture->connection_reactor;
  connection_runtime_config.event_loop = &fixture->event_loop;
  connection_runtime_config.entries = fixture->runtime_entries;
  connection_runtime_config.capacity = TEST_CAPACITY;
  if (omni_connection_runtime_init(&fixture->connection_runtime,
                                   &connection_runtime_config).status !=
      OMNI_CONNECTION_RUNTIME_OK) {
    return false;
  }

  manager_config.runtime = &fixture->connection_runtime;
  manager_config.entries = fixture->manager_entries;
  manager_config.capacity = manager_capacity;
  if (omni_connection_manager_init(&fixture->manager, &manager_config).status !=
      OMNI_CONNECTION_MANAGER_OK) {
    return false;
  }

  if (omni_listener_init(&fixture->listener, "127.0.0.1", 0u).status !=
      OMNI_LISTENER_OK) {
    return false;
  }

  admission_config.listener = &fixture->listener;
  admission_config.manager = &fixture->manager;
  admission_config.slots = fixture->admission_slots;
  admission_config.slots_bytes = sizeof(fixture->admission_slots);
  admission_config.capacity = admission_capacity;
  admission_config.connection_receive.storage = fixture->connection_receive;
  admission_config.connection_receive.storage_bytes = sizeof(fixture->connection_receive);
  admission_config.connection_receive.per_connection_capacity = TEST_BUFFER_CAPACITY;
  admission_config.session_receive.storage = fixture->session_receive;
  admission_config.session_receive.storage_bytes = sizeof(fixture->session_receive);
  admission_config.session_receive.per_connection_capacity = TEST_BUFFER_CAPACITY;
  admission_config.session_send.storage = fixture->session_send;
  admission_config.session_send.storage_bytes = sizeof(fixture->session_send);
  admission_config.session_send.per_connection_capacity = TEST_BUFFER_CAPACITY;
  if (omni_connection_admission_init(&fixture->admission, &admission_config).status !=
      OMNI_CONNECTION_ADMISSION_OK) {
    return false;
  }

  if (omni_runtime_start(&fixture->runtime).status != OMNI_RUNTIME_OK) return false;

  if (start_bridge) {
    bridge_config.listener = &fixture->listener;
    bridge_config.reactor = &fixture->reactor;
    bridge_config.admission = &fixture->admission;
    bridge_config.max_admissions_per_dispatch = budget;
    bridge_config.identities = fixture->identities;
    bridge_config.identity_capacity = TEST_CAPACITY;
    if (omni_listener_admission_init(&fixture->bridge, &bridge_config).status !=
            OMNI_LISTENER_ADMISSION_OK ||
        omni_listener_admission_start(&fixture->bridge).status !=
            OMNI_LISTENER_ADMISSION_OK) {
      return false;
    }
  }
  return true;
}

static void fixture_cleanup(struct test_fixture *fixture) {
  if (fixture == NULL) return;
  (void)omni_listener_admission_destroy(&fixture->bridge);
  (void)omni_connection_admission_destroy(&fixture->admission);
  omni_connection_manager_destroy(&fixture->manager);
  omni_connection_runtime_destroy(&fixture->connection_runtime);
  omni_connection_reactor_destroy(&fixture->connection_reactor);
  omni_event_loop_destroy(&fixture->event_loop);
  omni_runtime_destroy(&fixture->runtime);
  omni_listener_destroy(&fixture->listener);
}

static struct omni_listener_admission_config bridge_config(
    struct test_fixture *fixture, size_t budget) {
  struct omni_listener_admission_config config = {0};

  config.listener = &fixture->listener;
  config.reactor = &fixture->reactor;
  config.admission = &fixture->admission;
  config.max_admissions_per_dispatch = budget;
  config.identities = fixture->identities;
  config.identity_capacity = TEST_CAPACITY;
  return config;
}

static int open_client(uint16_t port) {
  struct sockaddr_in peer;
  int fd = socket(AF_INET, SOCK_STREAM, 0);

  if (fd < 0) return -1;
  (void)memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port = htons(port);
  peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (const struct sockaddr *)&peer, (socklen_t)sizeof(peer)) != 0) {
    (void)close(fd);
    return -1;
  }
  return fd;
}

static void close_fd(int *fd) {
  if (fd != NULL && *fd >= 0) {
    (void)close(*fd);
    *fd = -1;
  }
}

static struct omni_reactor_registration *find_bridge_registration(
    struct test_fixture *fixture) {
  if (fixture == NULL) return NULL;
  for (size_t i = 0u; i < fixture->reactor.count; ++i) {
    if (fixture->reactor.registrations[i].token ==
        OMNI_LISTENER_ADMISSION_REACTOR_TOKEN) {
      return &fixture->reactor.registrations[i];
    }
  }
  return NULL;
}

static void fire_bridge_callback(struct test_fixture *fixture, uint32_t events) {
  struct omni_reactor_registration *registration =
      find_bridge_registration(fixture);

  if (registration == NULL || registration->callback == NULL) {
    check(false, "bridge registration exists for callback injection");
    return;
  }
  registration->callback(registration->token, events, registration->context);
}

static void test_initialization(void) {
  struct test_fixture fixture = {0};
  struct omni_listener dead_listener = {0};
  struct omni_reactor unavailable_reactor = {0};
  struct omni_connection_admission unavailable_admission = {0};
  struct omni_listener_admission_config config;
  struct omni_listener_admission_result result;

  check(fixture_init(&fixture, 2u, TEST_CAPACITY, TEST_CAPACITY, false),
        "initialize dependencies for bridge configuration tests");
  config = bridge_config(&fixture, 2u);
  omni_listener_admission_make_inert(&fixture.bridge);
  check(omni_listener_admission_state(&fixture.bridge) ==
            OMNI_LISTENER_ADMISSION_INERT,
        "fresh bridge is inert");

  config.listener = NULL;
  result = omni_listener_admission_init(&fixture.bridge, &config);
  check(result.status == OMNI_LISTENER_ADMISSION_ERR_INVALID,
        "NULL listener rejected");

  omni_listener_make_inert(&dead_listener);
  config = bridge_config(&fixture, 2u);
  config.listener = &dead_listener;
  result = omni_listener_admission_init(&fixture.bridge, &config);
  check(result.status == OMNI_LISTENER_ADMISSION_ERR_INVALID,
        "dead listener rejected");

  config = bridge_config(&fixture, 2u);
  config.reactor = NULL;
  result = omni_listener_admission_init(&fixture.bridge, &config);
  check(result.status == OMNI_LISTENER_ADMISSION_ERR_INVALID,
        "NULL reactor rejected");

  omni_reactor_make_inert(&unavailable_reactor);
  config = bridge_config(&fixture, 2u);
  config.reactor = &unavailable_reactor;
  result = omni_listener_admission_init(&fixture.bridge, &config);
  check(result.status == OMNI_LISTENER_ADMISSION_ERR_INVALID,
        "unavailable reactor rejected");

  config = bridge_config(&fixture, 2u);
  config.admission = NULL;
  result = omni_listener_admission_init(&fixture.bridge, &config);
  check(result.status == OMNI_LISTENER_ADMISSION_ERR_INVALID,
        "NULL admission dependency rejected");

  omni_connection_admission_make_inert(&unavailable_admission);
  config = bridge_config(&fixture, 2u);
  config.admission = &unavailable_admission;
  result = omni_listener_admission_init(&fixture.bridge, &config);
  check(result.status == OMNI_LISTENER_ADMISSION_ERR_INVALID,
        "unavailable admission dependency rejected");

  config = bridge_config(&fixture, 0u);
  result = omni_listener_admission_init(&fixture.bridge, &config);
  check(result.status == OMNI_LISTENER_ADMISSION_ERR_INVALID,
        "zero dispatch budget rejected");

  config = bridge_config(&fixture, 2u);
  config.identity_capacity = 1u;
  result = omni_listener_admission_init(&fixture.bridge, &config);
  check(result.status == OMNI_LISTENER_ADMISSION_ERR_INVALID,
        "insufficient identity output storage rejected");

  config = bridge_config(&fixture, 2u);
  config.identities = NULL;
  result = omni_listener_admission_init(&fixture.bridge, &config);
  check(result.status == OMNI_LISTENER_ADMISSION_ERR_INVALID,
        "NULL identity output storage rejected");

  config = bridge_config(&fixture, 2u);
  result = omni_listener_admission_init(&fixture.bridge, &config);
  check(result.status == OMNI_LISTENER_ADMISSION_OK &&
            omni_listener_admission_state(&fixture.bridge) ==
                OMNI_LISTENER_ADMISSION_INITIALIZED,
        "valid dependencies initialize bridge");
  check(omni_listener_admission_destroy(&fixture.bridge).status ==
            OMNI_LISTENER_ADMISSION_OK &&
            omni_listener_admission_state(&fixture.bridge) ==
                OMNI_LISTENER_ADMISSION_CLOSED,
        "destroy before start closes only bridge state");
  check(omni_listener_admission_destroy(&fixture.bridge).status ==
            OMNI_LISTENER_ADMISSION_OK,
        "destroy before start is repeatable");
  check(fixture.listener.live && fixture.reactor.live &&
            fixture.admission.live,
        "destroy before start preserves borrowed dependencies");

  fixture_cleanup(&fixture);
}

static void test_start_registration_and_lifecycle(void) {
  struct test_fixture fixture = {0};
  struct omni_reactor_result blocker_result;
  struct omni_reactor_result restore_result;
  struct omni_listener_admission_result result;
  struct omni_reactor_registration saved_registration;
  int pipe_fds[2] = {-1, -1};
  size_t reactor_count = 0u;

  check(fixture_init(&fixture, 2u, TEST_CAPACITY, TEST_CAPACITY, false),
        "initialize bridge lifecycle fixture");
  {
    struct omni_listener_admission_config config = bridge_config(&fixture, 2u);
    check(omni_listener_admission_init(&fixture.bridge, &config).status ==
              OMNI_LISTENER_ADMISSION_OK,
          "initialize bridge before registration");
  }

  check(pipe(pipe_fds) == 0, "create registration-failure descriptor");
  blocker_result = omni_reactor_add(&fixture.reactor, pipe_fds[0],
                                   OMNI_LISTENER_ADMISSION_REACTOR_TOKEN,
                                   OMNI_POLLER_INTEREST_READ, reactor_noop, NULL);
  check(blocker_result.status == OMNI_REACTOR_OK,
        "reserve bridge-owned token to force registration failure");
  reactor_count = omni_reactor_count(&fixture.reactor);
  result = omni_listener_admission_start(&fixture.bridge);
  check(result.status == OMNI_LISTENER_ADMISSION_ERR_REACTOR_REGISTER &&
            result.reactor_status == OMNI_REACTOR_ERR_DUPLICATE &&
            omni_reactor_count(&fixture.reactor) == reactor_count &&
            omni_listener_admission_state(&fixture.bridge) ==
                OMNI_LISTENER_ADMISSION_INITIALIZED,
        "failed start rolls back without partial listener registration");
  check(omni_reactor_remove(&fixture.reactor,
                            OMNI_LISTENER_ADMISSION_REACTOR_TOKEN).status ==
            OMNI_REACTOR_OK,
        "remove temporary token blocker");
  close_fd(&pipe_fds[0]);
  close_fd(&pipe_fds[1]);

  reactor_count = omni_reactor_count(&fixture.reactor);
  result = omni_listener_admission_start(&fixture.bridge);
  check(result.status == OMNI_LISTENER_ADMISSION_OK &&
            omni_listener_admission_state(&fixture.bridge) ==
                OMNI_LISTENER_ADMISSION_ACTIVE &&
            omni_reactor_count(&fixture.reactor) == reactor_count + 1u,
        "start registers listener exactly once");
  check(omni_listener_admission_start(&fixture.bridge).status ==
            OMNI_LISTENER_ADMISSION_ERR_STATE &&
            omni_reactor_count(&fixture.reactor) == reactor_count + 1u,
        "duplicate start is rejected without duplicate registration");

  {
    struct omni_reactor_registration *registration =
        find_bridge_registration(&fixture);
    check(registration != NULL, "find active listener registration for removal test");
    if (registration != NULL) {
      saved_registration = *registration;
    } else {
      (void)memset(&saved_registration, 0, sizeof(saved_registration));
    }
  }
  check(omni_reactor_remove(&fixture.reactor,
                            OMNI_LISTENER_ADMISSION_REACTOR_TOKEN).status ==
            OMNI_REACTOR_OK,
        "remove bridge token externally to exercise removal failure");
  result = omni_listener_admission_stop(&fixture.bridge);
  check(result.status == OMNI_LISTENER_ADMISSION_ERR_REACTOR_REMOVE &&
            result.reactor_status == OMNI_REACTOR_ERR_NOT_FOUND &&
            result.state == OMNI_LISTENER_ADMISSION_ACTIVE,
        "failed stop preserves active state and reports reactor removal failure");
  restore_result = omni_reactor_add(&fixture.reactor, saved_registration.fd,
                                    saved_registration.token,
                                    saved_registration.interests,
                                    saved_registration.callback,
                                    saved_registration.context);
  check(restore_result.status == OMNI_REACTOR_OK,
        "restore listener registration after removal-failure injection");

  result = omni_listener_admission_stop(&fixture.bridge);
  check(result.status == OMNI_LISTENER_ADMISSION_OK &&
            result.state == OMNI_LISTENER_ADMISSION_STOPPED &&
            omni_reactor_count(&fixture.reactor) == reactor_count,
        "stop removes listener registration");
  check(omni_listener_admission_stop(&fixture.bridge).status ==
            OMNI_LISTENER_ADMISSION_OK,
        "repeated stop is safe");
  check(omni_listener_admission_start(&fixture.bridge).status ==
            OMNI_LISTENER_ADMISSION_ERR_STATE,
        "stopped bridge cannot register again");
  check(omni_listener_admission_destroy(&fixture.bridge).status ==
            OMNI_LISTENER_ADMISSION_OK,
        "destroy stopped bridge");
  check(omni_listener_admission_destroy(&fixture.bridge).status ==
            OMNI_LISTENER_ADMISSION_OK,
        "repeated bridge destroy is safe");
  check(fixture.listener.live && fixture.reactor.live &&
            fixture.admission.live,
        "stop and destroy preserve listener, reactor, and admission");

  fixture_cleanup(&fixture);
}

static void test_readiness_filtering_and_drain_status(void) {
  struct test_fixture fixture = {0};
  struct omni_listener_admission_result result;
  const uint32_t error_masks[] = {
      OMNI_POLLER_READY_READ | OMNI_POLLER_READY_ERROR,
      OMNI_POLLER_READY_READ | OMNI_POLLER_READY_HANGUP,
      OMNI_POLLER_READY_READ | OMNI_POLLER_READY_INVALID};

  check(fixture_init(&fixture, 2u, TEST_CAPACITY, TEST_CAPACITY, true),
        "initialize readiness fixture");
  fire_bridge_callback(&fixture, OMNI_POLLER_READY_WRITE);
  result = omni_listener_admission_last_result(&fixture.bridge);
  check(result.status == OMNI_LISTENER_ADMISSION_OK &&
            !result.admission_attempted &&
            result.readiness_events == OMNI_POLLER_READY_WRITE &&
            omni_connection_admission_count(&fixture.admission) == 0u,
        "WRITE readiness is recorded without admission");

  for (size_t i = 0u; i < sizeof(error_masks) / sizeof(error_masks[0]); ++i) {
    fire_bridge_callback(&fixture, error_masks[i]);
    result = omni_listener_admission_last_result(&fixture.bridge);
    check(result.status == OMNI_LISTENER_ADMISSION_LISTENER_ERROR &&
              !result.admission_attempted &&
              result.readiness_events == error_masks[i] &&
              omni_connection_admission_count(&fixture.admission) == 0u,
          "ERROR/HANGUP/INVALID readiness is surfaced without admission");
  }

  fire_bridge_callback(&fixture, OMNI_POLLER_READY_READ);
  result = omni_listener_admission_last_result(&fixture.bridge);
  check(result.status == OMNI_LISTENER_ADMISSION_ADMISSION_DRAINED &&
            result.admission_attempted &&
            result.admission.status == OMNI_CONNECTION_ADMISSION_DRAINED &&
            omni_listener_admission_state(&fixture.bridge) ==
                OMNI_LISTENER_ADMISSION_ACTIVE,
        "drained listener result preserves Task 029 status and bridge state");
  check(omni_listener_admission_dispatches(&fixture.bridge) == 5u,
        "readiness dispatch counter includes filtered callbacks");

  fixture_cleanup(&fixture);
}

static bool release_identity(struct test_fixture *fixture,
                             struct omni_connection_admission_identity identity) {
  return omni_connection_admission_release(&fixture->admission, identity).status ==
         OMNI_CONNECTION_ADMISSION_OK;
}

static void test_bounded_multi_client_admission(void) {
  struct test_fixture fixture = {0};
  struct omni_connection_admission_identity first_dispatch[2];
  struct omni_connection_admission_identity second_dispatch;
  struct omni_reactor_result step_result;
  struct omni_listener_admission_result bridge_result;
  int clients[3] = {-1, -1, -1};

  check(fixture_init(&fixture, 2u, TEST_CAPACITY, TEST_CAPACITY, true),
        "initialize bounded-drain fixture");
  for (size_t i = 0u; i < 3u; ++i) {
    clients[i] = open_client(omni_listener_port(&fixture.listener));
  }
  check(clients[0] >= 0 && clients[1] >= 0 && clients[2] >= 0,
        "queue three loopback clients before reactor dispatch");

  step_result = omni_reactor_step(&fixture.reactor, 0);
  bridge_result = omni_listener_admission_last_result(&fixture.bridge);
  first_dispatch[0] = fixture.identities[0];
  first_dispatch[1] = fixture.identities[1];
  check(step_result.status == OMNI_REACTOR_OK && step_result.count == 1u &&
            bridge_result.status ==
                OMNI_LISTENER_ADMISSION_ADMISSION_LIMIT_REACHED &&
            bridge_result.admission.admitted == 2u &&
            bridge_result.admission.attempts == 2u &&
            omni_connection_manager_count(&fixture.manager) == 2u,
        "one listener dispatch admits exactly the configured budget");
  check(omni_connection_admission_count(&fixture.admission) == 2u &&
            first_dispatch[0].token != 0u && first_dispatch[1].token != 0u,
        "bounded drain publishes two caller-owned identities");

  step_result = omni_reactor_step(&fixture.reactor, 0);
  bridge_result = omni_listener_admission_last_result(&fixture.bridge);
  second_dispatch = fixture.identities[0];
  check(step_result.status == OMNI_REACTOR_OK && step_result.count == 1u &&
            bridge_result.status ==
                OMNI_LISTENER_ADMISSION_ADMISSION_DRAINED &&
            bridge_result.admission.admitted == 1u &&
            bridge_result.admission.attempts == 2u &&
            omni_connection_manager_count(&fixture.manager) == 3u,
        "remaining queued client is admitted on a later readiness dispatch");
  check(omni_listener_admission_total_admitted(&fixture.bridge) == 3u &&
            omni_listener_admission_dispatches(&fixture.bridge) == 2u,
        "admission and dispatch accounting matches bounded callbacks");

  check(release_identity(&fixture, first_dispatch[0]) &&
            release_identity(&fixture, first_dispatch[1]) &&
            release_identity(&fixture, second_dispatch) &&
            omni_connection_manager_count(&fixture.manager) == 0u,
        "all event-driven admissions release through Task 029 identities");
  for (size_t i = 0u; i < 3u; ++i) close_fd(&clients[i]);
  fixture_cleanup(&fixture);
}

static void test_capacity_backpressure(void) {
  struct test_fixture fixture = {0};
  struct omni_connection_admission_identity first;
  struct omni_connection_admission_identity second;
  struct omni_listener_admission_result result;
  int clients[2] = {-1, -1};

  check(fixture_init(&fixture, 2u, 1u, TEST_CAPACITY, true),
        "initialize admission-slot capacity fixture");
  clients[0] = open_client(omni_listener_port(&fixture.listener));
  clients[1] = open_client(omni_listener_port(&fixture.listener));
  check(clients[0] >= 0 && clients[1] >= 0,
        "queue clients for admission-slot backpressure");
  (void)omni_reactor_step(&fixture.reactor, 0);
  result = omni_listener_admission_last_result(&fixture.bridge);
  first = fixture.identities[0];
  check(result.status == OMNI_LISTENER_ADMISSION_ADMISSION_CAPACITY &&
            result.admission.status == OMNI_CONNECTION_ADMISSION_SLOT_FULL &&
            result.admission.admitted == 1u &&
            omni_connection_manager_count(&fixture.manager) == 1u &&
            omni_listener_admission_state(&fixture.bridge) ==
                OMNI_LISTENER_ADMISSION_ACTIVE,
        "full admission slot stops drain without spinning or unregistering");

  check(release_identity(&fixture, first),
        "release slot to relieve admission backpressure");
  (void)omni_reactor_step(&fixture.reactor, 0);
  result = omni_listener_admission_last_result(&fixture.bridge);
  second = fixture.identities[0];
  check(result.admission.admitted == 1u && second.token != 0u &&
            omni_connection_manager_count(&fixture.manager) == 1u,
        "later readiness admits pending client after capacity release");
  check(release_identity(&fixture, second), "release admission after retry");
  close_fd(&clients[0]);
  close_fd(&clients[1]);
  fixture_cleanup(&fixture);
}

static bool external_connection_add(struct test_fixture *fixture,
                                    struct external_connection *external) {
  struct omni_connection_config connection_config = {0};
  struct omni_connection_manager_attach_config manager_config = {0};
  int pipe_fds[2] = {-1, -1};

  (void)memset(external, 0, sizeof(*external));
  external->read_fd = -1;
  external->write_fd = -1;
  omni_accepted_make_inert(&external->accepted);
  omni_connection_make_inert(&external->connection);
  if (pipe(pipe_fds) != 0) return false;
  external->read_fd = pipe_fds[0];
  external->write_fd = pipe_fds[1];
  external->accepted.fd = external->read_fd;
  external->accepted.live = true;

  connection_config.receive_storage = external->connection_receive;
  connection_config.receive_capacity = sizeof(external->connection_receive);
  connection_config.poller_token = 0u;
  connection_config.poller_interests = OMNI_POLLER_INTEREST_READ;
  if (omni_connection_init(&external->connection, &connection_config).status !=
      OMNI_CONNECTION_OK) {
    omni_accepted_destroy(&external->accepted);
    external->read_fd = -1;
    return false;
  }
  if (omni_connection_from_accepted(&external->connection, &external->accepted).status !=
      OMNI_CONNECTION_OK) {
    omni_connection_destroy(&external->connection);
    omni_accepted_destroy(&external->accepted);
    external->read_fd = -1;
    return false;
  }
  /* Connection now owns the read descriptor; only it may close that FD. */
  external->read_fd = -1;

  manager_config.connection = &external->connection;
  manager_config.receive_storage = external->session_receive;
  manager_config.receive_capacity = sizeof(external->session_receive);
  manager_config.send_storage = external->session_send;
  manager_config.send_capacity = sizeof(external->session_send);
  if (omni_connection_manager_add(&fixture->manager, &manager_config).status !=
      OMNI_CONNECTION_MANAGER_OK) {
    return false;
  }
  external->manager_added = true;
  return true;
}

static void external_connection_destroy(struct test_fixture *fixture,
                                        struct external_connection *external) {
  if (external == NULL) return;
  if (external->manager_added) {
    (void)omni_connection_manager_remove(&fixture->manager, &external->connection);
    external->manager_added = false;
  }
  omni_connection_destroy(&external->connection);
  omni_accepted_destroy(&external->accepted);
  close_fd(&external->read_fd);
  close_fd(&external->write_fd);
}

static void test_manager_capacity_backpressure(void) {
  struct test_fixture fixture = {0};
  struct external_connection external = {0};
  struct omni_connection_admission_identity first;
  struct omni_connection_admission_identity second;
  struct omni_listener_admission_result result;
  int clients[2] = {-1, -1};

  check(fixture_init(&fixture, 2u, 2u, 2u, true),
        "initialize manager-capacity fixture");
  clients[0] = open_client(omni_listener_port(&fixture.listener));
  check(clients[0] >= 0, "queue first client for manager-capacity fixture");
  (void)omni_reactor_step(&fixture.reactor, 0);
  first = fixture.identities[0];
  check(omni_connection_manager_count(&fixture.manager) == 1u,
        "first listener client admitted before manager fills");

  check(external_connection_add(&fixture, &external) &&
            omni_connection_manager_count(&fixture.manager) == 2u,
        "fill remaining manager entry with a separately owned connection");
  clients[1] = open_client(omni_listener_port(&fixture.listener));
  check(clients[1] >= 0, "queue client while manager is full");
  (void)omni_reactor_step(&fixture.reactor, 0);
  result = omni_listener_admission_last_result(&fixture.bridge);
  check(result.status == OMNI_LISTENER_ADMISSION_ADMISSION_CAPACITY &&
            result.admission.status == OMNI_CONNECTION_ADMISSION_MANAGER_FULL &&
            result.admission.admitted == 0u &&
            omni_listener_admission_state(&fixture.bridge) ==
                OMNI_LISTENER_ADMISSION_ACTIVE,
        "manager-full result is preserved and leaves bridge active");

  external_connection_destroy(&fixture, &external);
  check(release_identity(&fixture, first),
        "release listener admission and manager slot");
  (void)omni_reactor_step(&fixture.reactor, 0);
  second = fixture.identities[0];
  result = omni_listener_admission_last_result(&fixture.bridge);
  check(result.admission.admitted == 1u && second.token != 0u &&
            omni_connection_manager_count(&fixture.manager) == 1u,
        "pending client is admitted after manager capacity returns");
  check(release_identity(&fixture, second), "release retried manager admission");
  close_fd(&clients[0]);
  close_fd(&clients[1]);
  fixture_cleanup(&fixture);
}

static void test_stopped_admission_result(void) {
  struct test_fixture fixture = {0};
  struct omni_listener_admission_result result;
  int client = -1;

  check(fixture_init(&fixture, 2u, TEST_CAPACITY, TEST_CAPACITY, true),
        "initialize stopped-admission fixture");
  check(omni_connection_admission_stop(&fixture.admission).status ==
            OMNI_CONNECTION_ADMISSION_OK,
        "stop underlying admission while bridge remains registered");
  client = open_client(omni_listener_port(&fixture.listener));
  check(client >= 0, "queue client for stopped admission");
  (void)omni_reactor_step(&fixture.reactor, 0);
  result = omni_listener_admission_last_result(&fixture.bridge);
  check(result.status == OMNI_LISTENER_ADMISSION_ADMISSION_STOPPED &&
            result.admission.status == OMNI_CONNECTION_ADMISSION_ERR_STATE &&
            result.admission.admitted == 0u &&
            omni_connection_admission_count(&fixture.admission) == 0u,
        "stopped admission is surfaced without accepting a client");
  close_fd(&client);
  fixture_cleanup(&fixture);
}

static void test_saturating_accounting(void) {
  struct test_fixture fixture = {0};
  struct omni_connection_admission_identity identity;
  int client = -1;

  check(fixture_init(&fixture, 1u, TEST_CAPACITY, TEST_CAPACITY, true),
        "initialize saturating-accounting fixture");
  fixture.bridge.dispatches = UINT64_MAX - UINT64_C(1);
  fixture.bridge.total_admitted = UINT64_MAX - UINT64_C(1);
  client = open_client(omni_listener_port(&fixture.listener));
  check(client >= 0, "queue client for saturating counters");
  (void)omni_reactor_step(&fixture.reactor, 0);
  identity = fixture.identities[0];
  check(omni_listener_admission_dispatches(&fixture.bridge) == UINT64_MAX &&
            omni_listener_admission_total_admitted(&fixture.bridge) == UINT64_MAX,
        "dispatch and admission counters saturate instead of wrapping");
  check(release_identity(&fixture, identity),
        "release saturated-counter admission normally");
  close_fd(&client);
  fixture_cleanup(&fixture);
}

static void test_event_loop_integration(void) {
  struct test_fixture fixture = {0};
  struct loop_stop_context stop = {0};
  struct omni_reactor_result pipe_result;
  struct omni_event_loop_result loop_result;
  struct omni_connection_admission_identity identity;
  unsigned char wake_byte = 0x5Au;
  int pipe_fds[2] = {-1, -1};
  int client = -1;
  int admitted_fd = -1;
  size_t registrations_before_destroy = 0u;

  check(fixture_init(&fixture, 1u, TEST_CAPACITY, TEST_CAPACITY, true),
        "initialize event-loop integration fixture");
  stop.loop = &fixture.event_loop;
  stop.manager = &fixture.manager;
  stop.stop_status = OMNI_EVENT_LOOP_ERR_STATE;
  check(pipe(pipe_fds) == 0, "create separate test loop-stop readiness source");
  pipe_result = omni_reactor_add(&fixture.reactor, pipe_fds[0],
                                TEST_PIPE_TOKEN, OMNI_POLLER_INTEREST_READ,
                                loop_stop_when_admitted, &stop);
  check(pipe_result.status == OMNI_REACTOR_OK,
        "register test loop-stop callback after listener bridge");
  check(write(pipe_fds[1], &wake_byte, sizeof(wake_byte)) ==
            (ssize_t)sizeof(wake_byte),
        "make test loop-stop callback ready");
  client = open_client(omni_listener_port(&fixture.listener));
  check(client >= 0, "queue client before event-loop run");

  loop_result = omni_event_loop_run(&fixture.event_loop);
  identity = fixture.identities[0];
  check(loop_result.status == OMNI_EVENT_LOOP_OK &&
            omni_event_loop_state(&fixture.event_loop) == OMNI_EVENT_LOOP_STOPPED &&
            stop.stop_status == OMNI_EVENT_LOOP_OK,
        "event_loop_run exits through a separate test stop callback");
  check(omni_connection_manager_count(&fixture.manager) == 1u &&
            omni_connection_admission_count(&fixture.admission) == 1u &&
            identity.token != 0u && identity.slot_index < TEST_CAPACITY &&
            omni_connection_is_live(
                &fixture.admission_slots[identity.slot_index].connection),
        "event loop readiness callback admits a live managed connection");
  admitted_fd = omni_connection_fd(
      &fixture.admission_slots[identity.slot_index].connection);
  check(omni_listener_admission_dispatches(&fixture.bridge) >= 1u &&
            omni_listener_admission_total_admitted(&fixture.bridge) == 1u &&
            stop.calls >= 1u,
        "full listener-reactor-event-loop-admission chain ran synchronously");

  (void)omni_event_loop_destroy(&fixture.event_loop);
  check(omni_reactor_remove(&fixture.reactor, TEST_PIPE_TOKEN).status ==
            OMNI_REACTOR_OK,
        "remove test-only event-loop stop registration");
  registrations_before_destroy = omni_reactor_count(&fixture.reactor);
  check(omni_listener_admission_destroy(&fixture.bridge).status ==
            OMNI_LISTENER_ADMISSION_OK &&
            omni_reactor_count(&fixture.reactor) ==
                registrations_before_destroy - 1u,
        "destroy while active unregisters bridge before clearing context");
  check(fixture.listener.live && fixture.reactor.live &&
            fixture.admission.live && fixture.manager.live &&
            fixture.connection_runtime.live &&
            fixture.runtime.state == OMNI_RUNTIME_RUNNING &&
            omni_connection_manager_count(&fixture.manager) == 1u &&
            fd_is_open(admitted_fd),
        "active bridge destroy preserves dependencies and managed connection FD");
  check(release_identity(&fixture, identity),
        "admitted connection remains releasable after bridge destruction");
  close_fd(&client);
  close_fd(&pipe_fds[0]);
  close_fd(&pipe_fds[1]);
  fixture_cleanup(&fixture);
}

static void test_stop_preserves_existing_connection(void) {
  struct test_fixture fixture = {0};
  struct omni_connection_admission_identity identity;
  struct omni_listener_admission_result stop_result;
  struct omni_reactor_result step_result;
  int clients[2] = {-1, -1};
  int accepted_fd = -1;
  uint64_t dispatches_before_stop = 0u;

  check(fixture_init(&fixture, 1u, TEST_CAPACITY, TEST_CAPACITY, true),
        "initialize stop ownership fixture");
  clients[0] = open_client(omni_listener_port(&fixture.listener));
  clients[1] = open_client(omni_listener_port(&fixture.listener));
  check(clients[0] >= 0 && clients[1] >= 0,
        "queue one admitted and one pending client");
  (void)omni_reactor_step(&fixture.reactor, 0);
  identity = fixture.identities[0];
  accepted_fd = omni_connection_fd(
      &fixture.admission_slots[identity.slot_index].connection);
  dispatches_before_stop = omni_listener_admission_dispatches(&fixture.bridge);
  check(omni_connection_manager_count(&fixture.manager) == 1u &&
            omni_connection_admission_count(&fixture.admission) == 1u &&
            fd_is_open(accepted_fd),
        "one accepted connection is manager-owned before stop");

  stop_result = omni_listener_admission_stop(&fixture.bridge);
  check(stop_result.status == OMNI_LISTENER_ADMISSION_OK &&
            find_bridge_registration(&fixture) == NULL,
        "stop removes callback registration while leaving queued client pending");
  step_result = omni_reactor_step(&fixture.reactor, 0);
  check(step_result.status == OMNI_REACTOR_OK && step_result.count == 0u &&
            omni_listener_admission_dispatches(&fixture.bridge) ==
                dispatches_before_stop &&
            omni_connection_admission_count(&fixture.admission) == 1u,
        "no listener callback or new admission occurs after successful stop");
  check(fd_is_open(omni_listener_fd(&fixture.listener)) &&
            fd_is_open(clients[0]) && fd_is_open(accepted_fd) &&
            fixture.reactor.live && fixture.admission.live &&
            omni_connection_manager_count(&fixture.manager) == 1u,
        "stop preserves listener, reactor, client, admission, and managed FD");

  check(release_identity(&fixture, identity) &&
            omni_connection_manager_count(&fixture.manager) == 0u,
        "existing connection remains releasable after stop");
  close_fd(&clients[0]);
  close_fd(&clients[1]);
  fixture_cleanup(&fixture);
}

static void test_bounded_stress_and_fd_census(void) {
  struct test_fixture fixture = {0};
  size_t baseline_fds = SIZE_MAX;
  size_t successful_cycles = 0u;

  check(fixture_init(&fixture, 1u, TEST_CAPACITY, TEST_CAPACITY, true),
        "initialize bounded stress fixture");
  baseline_fds = count_open_fds();
  check(baseline_fds != SIZE_MAX, "capture initial Linux FD census");

  for (size_t i = 0u; i < TEST_STRESS_CYCLES; ++i) {
    int client = open_client(omni_listener_port(&fixture.listener));
    struct omni_reactor_result step_result;
    struct omni_connection_admission_identity identity = {
        OMNI_CONNECTION_ADMISSION_SLOT_INVALID, 0u};
    bool cycle_ok = client >= 0;

    if (cycle_ok) {
      step_result = omni_reactor_step(&fixture.reactor, 0);
      identity = fixture.identities[0];
      cycle_ok = step_result.status == OMNI_REACTOR_OK &&
                 omni_connection_manager_count(&fixture.manager) == 1u &&
                 omni_connection_admission_count(&fixture.admission) == 1u &&
                 identity.token != 0u;
      if (cycle_ok) {
        cycle_ok = release_identity(&fixture, identity);
      }
      close_fd(&client);
    }

    quiet_check(cycle_ok, "stress cycle admits and releases one managed connection");
    quiet_check(omni_connection_admission_count(&fixture.admission) <=
                    omni_connection_admission_capacity(&fixture.admission) &&
                    omni_connection_manager_count(&fixture.manager) <=
                    omni_connection_manager_capacity(&fixture.manager),
                "stress cycle respects admission and manager capacities");
    quiet_check(count_open_fds() == baseline_fds,
                "stress cycle returns to baseline FD count");
    if (cycle_ok) successful_cycles += 1u;
  }

  check(successful_cycles == TEST_STRESS_CYCLES &&
            omni_listener_admission_total_admitted(&fixture.bridge) ==
                (uint64_t)TEST_STRESS_CYCLES &&
            omni_listener_admission_dispatches(&fixture.bridge) ==
                (uint64_t)TEST_STRESS_CYCLES,
        "1,000 readiness-driven admission/release cycles complete exactly");
  check(count_open_fds() == baseline_fds &&
            omni_connection_manager_count(&fixture.manager) == 0u &&
            omni_connection_admission_count(&fixture.admission) == 0u,
        "stress ends with stable FD census and empty bounded managers");
  fixture_cleanup(&fixture);
}

int main(void) {
  struct omni_listener_admission bridge_size = {0};

  test_initialization();
  test_start_registration_and_lifecycle();
  test_readiness_filtering_and_drain_status();
  test_bounded_multi_client_admission();
  test_capacity_backpressure();
  test_manager_capacity_backpressure();
  test_stopped_admission_result();
  test_saturating_accounting();
  test_event_loop_integration();
  test_stop_preserves_existing_connection();
  test_bounded_stress_and_fd_census();

  (void)printf("sizeof(struct omni_listener_admission)=%zu\n", sizeof(bridge_size));
  (void)printf("sizeof(struct omni_listener_admission_config)=%zu\n",
               sizeof(struct omni_listener_admission_config));
  (void)printf("sizeof(struct omni_listener_admission_result)=%zu\n",
               sizeof(struct omni_listener_admission_result));
  (void)printf("sizeof(struct omni_connection_admission_identity)=%zu\n",
               sizeof(struct omni_connection_admission_identity));
  (void)printf("checks=%zu failures=%zu\n", check_count, failure_count);
  return failure_count == 0u ? 0 : 1;
}
