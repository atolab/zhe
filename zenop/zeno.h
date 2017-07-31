#ifndef ZENO_H
#define ZENO_H

#include "zeno-config.h"

typedef struct { uint8_t idx; } pubidx_t;
typedef struct { uint8_t idx; } subidx_t;
typedef void (*subhandler_t)(rid_t rid, uint16_t size, const void *payload, void *arg);

#endif
