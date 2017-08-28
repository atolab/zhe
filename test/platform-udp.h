#ifndef TRANSPORT_UDP_H
#define TRANSPORT_UDP_H

#include <netinet/in.h>
#include "platform.h"

typedef struct zhe_address {
    struct sockaddr_in a;
} zhe_address_t;

#define TRANSPORT_MTU        1472u
#define TRANSPORT_MODE       TRANSPORT_PACKET
#define TRANSPORT_ADDRSTRLEN (INET_ADDRSTRLEN + 6)

zhe_time_t zhe_platform_time(void);
void zhe_platform_trace(struct zhe_platform *pf, const char *fmt, ...);
struct zhe_platform *zhe_platform_new(uint16_t port, int drop_pct);
size_t zhe_platform_addr2string(const struct zhe_platform *pf, char * restrict str, size_t size, const zhe_address_t * restrict addr);
int zhe_platform_string2addr(const struct zhe_platform *pf, struct zhe_address * restrict addr, const char * restrict str);
int zhe_platform_join(const struct zhe_platform *pf, const struct zhe_address *addr);
int zhe_platform_wait(const struct zhe_platform *pf, zhe_timediff_t timeout);
int zhe_platform_recv(struct zhe_platform *pf, void * restrict buf, size_t size, zhe_address_t * restrict src);
int zhe_platform_send(struct zhe_platform *pf, const void * restrict buf, size_t size, const zhe_address_t * restrict dst);
int zhe_platform_addr_eq(const struct zhe_address *a, const struct zhe_address *b);

#endif
