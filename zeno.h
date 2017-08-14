#ifndef ZENO_H
#define ZENO_H

#include "zeno-config.h"

typedef struct zeno_transport_ops zeno_transport_ops_t;
typedef struct { uint8_t idx; } pubidx_t;
typedef struct { uint8_t idx; } subidx_t;
typedef void (*subhandler_t)(rid_t rid, uint16_t size, const void *payload, void *arg);

int zeno_init(zpsize_t idlen, const void *id);
void zeno_loop_init(void);
ztime_t zeno_loop(void);

#endif
