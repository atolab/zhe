#ifndef ARDUINO

#ifndef TRANSPORT_UDP_H
#define TRANSPORT_UDP_H

#include <netinet/in.h>
#include "transport.h"

typedef struct zhe_address {
    struct sockaddr_in a;
} zhe_address_t;

#define TRANSPORT_MTU        1472u
#define TRANSPORT_MODE       TRANSPORT_PACKET
#define TRANSPORT_NAME       udp
#define TRANSPORT_ADDRSTRLEN (INET_ADDRSTRLEN + 6)

struct zhe_transport *zhe_udp_new(uint16_t port, int drop_pct);
int zhe_udp_string2addr(const struct zhe_transport *tp, struct zhe_address * restrict addr, const char * restrict str);
int zhe_udp_join(const struct zhe_transport * restrict tp, const struct zhe_address *addr);
int zhe_udp_wait(const struct zhe_transport * restrict tp, zhe_timediff_t timeout);
ssize_t zhe_udp_recv(struct zhe_transport * restrict tp, void * restrict buf, size_t size, zhe_address_t * restrict src);

#endif
#endif
