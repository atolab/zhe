#ifndef ARDUINO

#ifndef TRANSPORT_UDP_H
#define TRANSPORT_UDP_H

#include <netinet/in.h>
#include "transport.h"

typedef struct zeno_address {
    struct sockaddr_in a;
} zeno_address_t;

#define TRANSPORT_MTU        1472u
#define TRANSPORT_MODE       TRANSPORT_PACKET
#define TRANSPORT_NAME       udp
#define TRANSPORT_ADDRSTRLEN (INET_ADDRSTRLEN + 6)

struct zeno_transport *udp_new(uint16_t port, int drop_pct);
int udp_string2addr(const struct zeno_transport *tp, struct zeno_address * restrict addr, const char * restrict str);
int udp_join(const struct zeno_transport * restrict tp, const struct zeno_address *addr);
int udp_wait(const struct zeno_transport * restrict tp, ztimediff_t timeout);
ssize_t udp_recv(struct zeno_transport * restrict tp, void * restrict buf, size_t size, zeno_address_t * restrict src);

#endif
#endif
