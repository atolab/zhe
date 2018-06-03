#ifndef PLATFORM_SERIAL_H
#define PLATFORM_SERIAL_H

#include "zhe-platform.h"

typedef struct zhe_address {
    char dummy;
} zhe_address_t;

#define TRANSPORT_MTU        20u
#define TRANSPORT_MODE       TRANSPORT_STREAM
#define TRANSPORT_ADDRSTRLEN 1

#endif
