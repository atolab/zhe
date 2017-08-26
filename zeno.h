#ifndef ZENO_H
#define ZENO_H

#include <stddef.h>
#include "zeno-config.h"
#include "zeno-rid.h"

#if MAX_PUBLICATIONS < 256
typedef struct { uint8_t idx; } pubidx_t;
#elif MAX_PUBLICATIONS < 32768
typedef struct { uint16_t idx; } pubidx_t;
#else
#error "MAX_PUBLICATIONS >= 32768 not implemented (cf bitset_findfirst)"
#endif

#if MAX_SUBSCRIPTIONS < 256
typedef struct { uint8_t idx; } subidx_t;
#elif MAX_SUBSCRIPTIONS < 32768
typedef struct { uint16_t idx; } subidx_t;
#else
#error "MAX_SUBSCRIPTIONS >= 32768 not implemented (cf bitset_findfirst)"
#endif

typedef void (*subhandler_t)(rid_t rid, zpsize_t size, const void *payload, void *arg);

struct zeno_address;
struct zeno_transport;

struct zeno_config {
    size_t idlen;
    const void *id;

    struct zeno_address *scoutaddr;

    size_t n_mcgroups_join;
    struct zeno_address *mcgroups_join;

    size_t n_mconduit_dstaddrs;
    struct zeno_address *mconduit_dstaddrs;
};

int zeno_init(const struct zeno_config *config, struct zeno_transport *tp, ztime_t tnow);
void zeno_start(ztime_t tnow);
void zeno_housekeeping(ztime_t tnow);
ssize_t zeno_input(const void * restrict buf, size_t sz, const struct zeno_address *src, ztime_t tnow);
void zeno_flush(void);

pubidx_t publish(rid_t rid, unsigned cid, int reliable);
subidx_t subscribe(rid_t rid, zpsize_t xmitneed, unsigned cid, subhandler_t handler, void *arg);

int zeno_write(pubidx_t pubidx, zpsize_t sz, const void *data, ztime_t tnow);

#endif
