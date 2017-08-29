#ifndef PLATFORM_ARDUINO_H
#define PLATFORM_ARDUINO_H

#include "zhe-platform.h"

typedef struct zhe_address {
    char dummy;
} zhe_address_t;

#define TRANSPORT_MTU        20u
#define TRANSPORT_MODE       TRANSPORT_STREAM
#define TRANSPORT_ADDRSTRLEN 1

struct zhe_platform *zhe_arduino_new(void);
size_t zhe_platform_addr2string(const struct zhe_platform *pf, char * restrict str, size_t size, const zhe_address_t * restrict addr);
int zhe_platform_string2addr(const struct zhe_platform *pf, struct zhe_address * restrict addr, const char * restrict str);
int zhe_platform_send(struct zhe_platform *pf, const void * restrict buf, size_t size, const zhe_address_t * restrict dst);
int zhe_platform_recv(struct zhe_platform *pf, void * restrict buf, size_t size, zhe_address_t * restrict src);
int zhe_platform_addr_eq(const struct zhe_address *a, const struct zhe_address *b);

#endif
