/* Switch (Horizon) has no mmap. Guest memory has its own backend
 * (guest_mem.c, __SWITCH__ branch); these definitions only let the
 * remaining host-side users compile. Anonymous mappings fall back to the
 * heap; file mappings and protection changes are not supported. */
#pragma once
#include <stddef.h>
#include <sys/types.h>
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_FAILED ((void *)-1)
#define MADV_DONTNEED 4
#define MADV_WILLNEED 3
#define MS_SYNC 4
#ifdef __cplusplus
extern "C" {
#endif
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
int madvise(void *addr, size_t len, int advice);
int msync(void *addr, size_t len, int flags);
#ifdef __cplusplus
}
#endif
