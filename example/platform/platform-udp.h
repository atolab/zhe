#ifndef TRANSPORT_UDP_H
#define TRANSPORT_UDP_H

#include <netinet/in.h>
#include "zhe-platform.h"

#define TRANSPORT_MTU        1472u
#define TRANSPORT_MODE       TRANSPORT_PACKET
#define TRANSPORT_ADDRSTRLEN (4 + INET_ADDRSTRLEN + 6) /* udp/IP:PORT -- udp/ is 4, colon is 1, PORT in [1,5] */

typedef struct zhe_address {
    struct sockaddr_in a;
} zhe_address_t;

typedef struct zhe_recvbuf {
    uint8_t buf[TRANSPORT_MTU];
} zhe_recvbuf_t;

zhe_time_t zhe_platform_time(void);
struct zhe_platform *zhe_platform_new(uint16_t port, int drop_pct);
int zhe_platform_string2addr(const struct zhe_platform *pf, struct zhe_address * restrict addr, const char * restrict str);
int zhe_platform_join(const struct zhe_platform *pf, const struct zhe_address *addr);
int zhe_platform_wait(const struct zhe_platform *pf, zhe_timediff_t timeout);
int zhe_platform_recv(struct zhe_platform *pf, zhe_recvbuf_t *buf, zhe_address_t * restrict src);
#define zhe_platform_advance(pf_,src_,cnt_) ((void)(cnt_))

#endif
