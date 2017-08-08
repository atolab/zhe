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

#endif
#endif
