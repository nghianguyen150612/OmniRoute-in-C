/*
 * OmniRoute native backend — executable entry point (Task 011 skeleton).
 *
 * The binary proves the runtime foundation only: parse a tiny CLI, report
 * startup metadata, optionally report memory diagnostics, shut down
 * deterministically. It is not an HTTP server: no port is bound, no network
 * is contacted, no database is opened, no catalog is loaded.
 *
 * Ownership: this skeleton performs zero heap allocation — output uses
 * string literals through stdio, diagnostics use stack buffers in meminfo.c.
 * (The C library itself may allocate internally; that is outside program
 * control and outside this claim.)
 *
 * CLI precedence is fixed and documented: --help wins over --version, which
 * wins over a normal run. Unknown options and positional arguments are usage
 * errors (exit 2) with a one-line stderr diagnostic.
 */

#include <stdio.h>
#include <string.h>

#include "omniroute/exit_code.h"
#include "omniroute/meminfo.h"
#include "omniroute/version.h"

#if defined(NDEBUG)
#define OMNI_BUILD_MODE "release"
#else
#define OMNI_BUILD_MODE "debug"
#endif

static void print_usage(void) {
  printf("Usage: %s [--help] [--version] [--meminfo]\n", OMNIROUTE_NATIVE_NAME);
  printf("\n");
  printf("OmniRoute native backend executable skeleton.\n");
  printf("\n");
  printf("Options:\n");
  printf("  --help     print this help and exit\n");
  printf("  --version  print the version and exit\n");
  printf("  --meminfo  print process memory diagnostics and exit\n");
}

static void print_version(void) {
  printf("%s %s\n", OMNIROUTE_NATIVE_NAME, OMNIROUTE_NATIVE_VERSION);
}

static void print_meminfo(void) {
  struct omni_meminfo info;

  if (!omni_meminfo_read(&info)) {
    printf("rss_kb: unavailable\n");
    printf("peak_rss_kb: unavailable\n");
    return;
  }
  if (info.rss_available) {
    printf("rss_kb: %llu\n", info.rss_kb);
  } else {
    printf("rss_kb: unavailable\n");
  }
  if (info.peak_available) {
    printf("peak_rss_kb: %llu\n", info.peak_rss_kb);
  } else {
    printf("peak_rss_kb: unavailable\n");
  }
}

static int run_normal(bool want_meminfo) {
  printf("%s %s ready (%s, c11)\n", OMNIROUTE_NATIVE_NAME, OMNIROUTE_NATIVE_VERSION,
         OMNI_BUILD_MODE);
  if (want_meminfo) {
    print_meminfo();
  }
  return OMNI_EXIT_OK;
}

int main(int argc, char *argv[]) {
  bool want_help = false;
  bool want_version = false;
  bool want_meminfo = false;
  int i = 0;

  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--help") == 0) {
      want_help = true;
    } else if (strcmp(argv[i], "--version") == 0) {
      want_version = true;
    } else if (strcmp(argv[i], "--meminfo") == 0) {
      want_meminfo = true;
    } else {
      fprintf(stderr, "%s: unknown option '%s'\n", OMNIROUTE_NATIVE_NAME, argv[i]);
      fprintf(stderr, "Usage: %s [--help] [--version] [--meminfo]\n", OMNIROUTE_NATIVE_NAME);
      return OMNI_EXIT_USAGE;
    }
  }

  if (want_help) {
    print_usage();
    return OMNI_EXIT_OK;
  }
  if (want_version) {
    print_version();
    return OMNI_EXIT_OK;
  }
  if (want_meminfo && argc == 2) {
    print_meminfo();
    return OMNI_EXIT_OK;
  }
  return run_normal(want_meminfo);
}
