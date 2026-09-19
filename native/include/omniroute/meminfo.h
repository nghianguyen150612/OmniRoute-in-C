/*
 * OmniRoute native backend — minimal process memory instrumentation.
 *
 * Reports the process's own current RSS and (where reliably available) peak
 * RSS without dependencies, threads, or polling. Meant for one-shot
 * diagnostics (`--meminfo`) and future regression baselines, not for
 * allocator-level accounting.
 *
 * On Linux the numbers are the kernel's VmRSS / VmHWM from /proc/self/status
 * (same semantics as the Task 002 reference tooling: whole-process resident
 * set including shared mappings; lifetime high-water mark). Elsewhere the
 * call reports unavailable. Unavailable is always distinguishable from zero
 * via the `*_available` flags.
 */
#ifndef OMNIROUTE_MEMINFO_H
#define OMNIROUTE_MEMINFO_H

#include <stdbool.h>

struct omni_meminfo {
  bool rss_available;
  unsigned long long rss_kb;
  bool peak_available;
  unsigned long long peak_rss_kb;
};

/*
 * Fill *out. Returns true when at least one metric was read; returns false
 * (with both flags cleared) when measurement is unavailable. Never fails
 * loudly: an unreadable /proc entry is "unavailable", not a crash. No heap
 * allocation. NULL input is rejected with false.
 */
bool omni_meminfo_read(struct omni_meminfo *out);

#endif /* OMNIROUTE_MEMINFO_H */
