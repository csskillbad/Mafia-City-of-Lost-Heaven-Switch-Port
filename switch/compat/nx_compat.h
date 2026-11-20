/* Force-included (-include) into the runtime and win32hle on Switch:
 * small POSIX/glibc pieces newlib + libnx do not provide. */
#pragma once
#include <time.h>
#include <sys/types.h>
#define mallinfo2 mallinfo                  /* glibc 2.33+ name; same fields */
#ifndef UTIME_OMIT
#define UTIME_OMIT ((1l << 30) - 2l)
#define UTIME_NOW  ((1l << 30) - 1l)
#endif
#ifndef PRIO_PROCESS
#define PRIO_PROCESS 0
#endif
#ifdef __cplusplus
extern "C" {
#endif
time_t timegm(struct tm *tm);
int setpriority(int which, id_t who, int prio);
#ifdef __cplusplus
}
#endif
