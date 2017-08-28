#ifndef ZHE_H
#define ZHE_H

#include <stddef.h>
#include "zhe-config.h"
#include "zhe-rid.h"

#if ZHE_MAX_PUBLICATIONS < 256
typedef struct { uint8_t idx; } zhe_pubidx_t;
#elif ZHE_MAX_PUBLICATIONS < 32768
typedef struct { uint16_t idx; } zhe_pubidx_t;
#else
#error "ZHE_MAX_PUBLICATIONS >= 32768 not implemented (cf bitset_findfirst)"
#endif

#if ZHE_MAX_SUBSCRIPTIONS < 256
typedef struct { uint8_t idx; } zhe_subidx_t;
#elif ZHE_MAX_SUBSCRIPTIONS < 32768
typedef struct { uint16_t idx; } zhe_subidx_t;
#else
#error "ZHE_MAX_SUBSCRIPTIONS >= 32768 not implemented (cf bitset_findfirst)"
#endif

typedef void (*zhe_subhandler_t)(zhe_rid_t rid, zhe_paysize_t size, const void *payload, void *arg);

struct zhe_address;
struct zhe_platform;

struct zhe_config {
    size_t idlen;
    const void *id;

    struct zhe_address *scoutaddr;

    size_t n_mcgroups_join;
    struct zhe_address *mcgroups_join;

    size_t n_mconduit_dstaddrs;
    struct zhe_address *mconduit_dstaddrs;
};

int zhe_init(const struct zhe_config *config, struct zhe_platform *pf, zhe_time_t tnow);
void zhe_start(zhe_time_t tnow);
void zhe_housekeeping(zhe_time_t tnow);
int zhe_input(const void * restrict buf, size_t sz, const struct zhe_address *src, zhe_time_t tnow);
void zhe_flush(void);

zhe_pubidx_t zhe_publish(zhe_rid_t rid, unsigned cid, int reliable);
zhe_subidx_t zhe_subscribe(zhe_rid_t rid, zhe_paysize_t xmitneed, unsigned cid, zhe_subhandler_t handler, void *arg);

int zhe_write(zhe_pubidx_t pubidx, zhe_paysize_t sz, const void *data, zhe_time_t tnow);

#endif
