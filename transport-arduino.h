#if defined ARDUINO

#ifndef TRANSPORT_ARDUINO_H
#define TRANSPORT_ARDUINO_H

#include "transport.h"

typedef struct zeno_address {
    char dummy;
} zeno_address_t;

#define TRANSPORT_MTU        128u
#define TRANSPORT_MODE       TRANSPORT_STREAM
#define TRANSPORT_NAME       arduino
#define TRANSPORT_ADDRSTRLEN 1

struct zeno_transport *arduino_new(void);
int arduino_string2addr(const struct zeno_transport *tp, struct zeno_address * restrict addr, const char * restrict str);
int arduino_join(const struct zeno_transport * restrict tp, const struct zeno_address *addr);
int arduino_wait(const struct zeno_transport * restrict tp, ztimediff_t timeout);
ssize_t arduino_recv(struct zeno_transport * restrict tp, void * restrict buf, size_t size, zeno_address_t * restrict src);

#endif

#endif
