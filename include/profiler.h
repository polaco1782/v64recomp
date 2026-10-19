#ifndef RECOMP_PROFILER_H
#define RECOMP_PROFILER_H
#include <stdbool.h>

/* Optional, process-wide Linux CPU profiler. No work is done unless started.
 * The directory must be new. A zero duration records until normal process exit.
 * stop() joins the collector and writes reports; it is also registered at exit. */
bool recomp_profile_start(const char *directory, unsigned hz, unsigned delay_seconds, unsigned duration_seconds);
void recomp_profile_stop(void);
#endif
