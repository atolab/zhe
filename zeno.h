#ifndef ZENO_H
#define ZENO_H

#include "zeno-config.h"

typedef struct zeno_transport_ops zeno_transport_ops_t;
typedef struct { uint8_t idx; } pubidx_t;
typedef struct { uint8_t idx; } subidx_t;
typedef void (*subhandler_t)(rid_t rid, zpsize_t size, const void *payload, void *arg);

int zeno_init(zpsize_t idlen, const void *id);
void zeno_loop_init(void);
ztime_t zeno_loop(void);
void zeno_wait_input(ztimediff_t timeout);

pubidx_t publish(rid_t rid, unsigned cid, int reliable);
subidx_t subscribe(rid_t rid, zpsize_t xmitneed, subhandler_t handler, void *arg);

int zeno_write(pubidx_t pubidx, zpsize_t sz, const void *data);

#endif
