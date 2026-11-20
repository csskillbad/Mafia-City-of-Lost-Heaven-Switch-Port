#pragma once
#include <sys/socket.h>
struct ifaddrs {
    struct ifaddrs *ifa_next;
    char *ifa_name;
    unsigned int ifa_flags;
    struct sockaddr *ifa_addr;
    struct sockaddr *ifa_netmask;
};
static inline int getifaddrs(struct ifaddrs **ifap) { *ifap = 0; return -1; }
static inline void freeifaddrs(struct ifaddrs *ifa) { (void)ifa; }
