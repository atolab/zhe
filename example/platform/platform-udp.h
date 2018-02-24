#ifndef TRANSPORT_UDP_H
#define TRANSPORT_UDP_H

#include <netinet/in.h>
#include "zhe-platform.h"

typedef struct zhe_address {
    struct sockaddr_in a;
} zhe_address_t;

#define TRANSPORT_MTU        1472u
#define TRANSPORT_MODE       TRANSPORT_PACKET
#define TRANSPORT_ADDRSTRLEN (4 + INET_ADDRSTRLEN + 6) /* udp/IP:PORT -- udp/ is 4, colon is 1, PORT in [1,5] */

zhe_time_t zhe_platform_time(void);
struct zhe_platform *zhe_platform_new(uint16_t port, int drop_pct);
int zhe_platform_string2addr(const struct zhe_platform *pf, struct zhe_address * restrict addr, const char * restrict str);
int zhe_platform_join(const struct zhe_platform *pf, const struct zhe_address *addr);
int zhe_platform_wait(const struct zhe_platform *pf, zhe_timediff_t timeout);
int zhe_platform_recv(struct zhe_platform *pf, void * restrict buf, size_t size, zhe_address_t * restrict src);

#endif
