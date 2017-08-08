#if defined ARDUINO || defined __APPLE__

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

#endif

#endif
