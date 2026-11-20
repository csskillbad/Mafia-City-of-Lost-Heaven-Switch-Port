/* Linux syscall() is only used for gettid; on Horizon use the thread id. */
#pragma once
#include <stdint.h>
#define SYS_gettid 178
long nx_gettid(void);
#define syscall(n, ...) ((n) == SYS_gettid ? nx_gettid() : -1L)
