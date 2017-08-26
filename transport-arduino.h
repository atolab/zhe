#if defined ARDUINO

#ifndef TRANSPORT_ARDUINO_H
#define TRANSPORT_ARDUINO_H

#include "transport.h"

typedef struct zhe_address {
    char dummy;
} zhe_address_t;

#define TRANSPORT_MTU        128u
#define TRANSPORT_MODE       TRANSPORT_STREAM
#define TRANSPORT_NAME       arduino
#define TRANSPORT_ADDRSTRLEN 1

struct zhe_transport *zhe_arduino_new(void);
int zhe_arduino_string2addr(const struct zhe_transport *tp, struct zhe_address * restrict addr, const char * restrict str);
int zhe_arduino_join(const struct zhe_transport * restrict tp, const struct zhe_address *addr);
int zhe_arduino_wait(const struct zhe_transport * restrict tp, zhe_timediff_t timeout);
ssize_t zhe_arduino_recv(struct zhe_transport * restrict tp, void * restrict buf, size_t size, zhe_address_t * restrict src);

#endif

#endif
