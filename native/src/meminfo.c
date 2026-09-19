/*
 * OmniRoute native backend — memory instrumentation implementation.
 *
 * Linux path parses /proc/self/status with a bounded stack buffer; the only
 * owned resource is the FILE* and it is closed on every path, including the
 * parse-finds-nothing path. No heap allocation anywhere in this file.
 *
 * Non-Linux builds compile the same translation unit to the explicit
 * unavailable stub, so generic callers never need platform #ifdefs. The
 * narrow Linux boundary splits into platform/linux/ once iOS work begins.
 */

#include "omniroute/meminfo.h"

#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#define OMNI_MEMINFO_HAVE_PROC_SELF 1
#else
#define OMNI_MEMINFO_HAVE_PROC_SELF 0
#endif

/* Longest /proc/self/status line is far shorter; 256 leaves wide margin. */
#define OMNI_STATUS_LINE_MAX 256

#if OMNI_MEMINFO_HAVE_PROC_SELF

static bool read_proc_self_status(struct omni_meminfo *out) {
  FILE *stream = fopen("/proc/self/status", "r");
  char line[OMNI_STATUS_LINE_MAX];
  bool saw_rss = false;
  bool saw_peak = false;

  if (stream == NULL) {
    return false;
  }
  while (fgets(line, (int)sizeof(line), stream) != NULL) {
    /* sscanf targets are unsigned long long with %llu: no narrowing. */
    if (strncmp(line, "VmRSS:", 6) == 0) {
      unsigned long long value = 0;
      if (sscanf(line + 6, "%llu", &value) == 1) {
        out->rss_kb = value;
        out->rss_available = true;
        saw_rss = true;
      }
    } else if (strncmp(line, "VmHWM:", 6) == 0) {
      unsigned long long value = 0;
      if (sscanf(line + 6, "%llu", &value) == 1) {
        out->peak_rss_kb = value;
        out->peak_available = true;
        saw_peak = true;
      }
    }
    if (saw_rss && saw_peak) {
      break;
    }
  }
  fclose(stream);
  return saw_rss || saw_peak;
}

#endif /* OMNI_MEMINFO_HAVE_PROC_SELF */

bool omni_meminfo_read(struct omni_meminfo *out) {
  if (out == NULL) {
    return false;
  }
  out->rss_available = false;
  out->rss_kb = 0;
  out->peak_available = false;
  out->peak_rss_kb = 0;
#if OMNI_MEMINFO_HAVE_PROC_SELF
  return read_proc_self_status(out);
#else
  return false;
#endif
}
