#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "zhe-config-deriv.h"
#include "zhe-assert.h"
#include "zhe-bitset.h"
#include "zhe-tracing.h"
#include "zhe-uristore.h"

#if ZHE_MAX_URISPACE > 0

static union {
    uint8_t store[sizeof(struct icgcb) + ZHE_MAX_URISPACE];
    struct icgcb b;
} uris;

struct restable {
    zhe_rid_t rid;
    uripos_t uripos;
    uint8_t reliable: 1;
    uint8_t transient: 1;
    DECL_BITSET(peers, MAX_PEERS_1 + 1); /* self is MAX_PEERS_1 */
};
static struct restable ress[ZHE_MAX_RESOURCES];
static zhe_residx_t max_residx;

void zhe_uristore_init(void)
{
    zhe_icgcb_init(&uris.b, sizeof(uris));
    memset(&ress, 0, sizeof(ress));
}

static void set_props_one(struct restable * const r, const uint8_t *tag, size_t taglen)
{
    if (taglen == 9 && memcmp(tag, "transient", taglen) == 0) {
        r->transient = 1;
    } else if (taglen == 10 && memcmp(tag, "unreliable", taglen) == 0) {
        r->reliable = 0;
    }
}

static size_t scan_uri(const uint8_t *s, uint8_t a, uint8_t b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (*s == a && *s == b) {
            return i;
        } else {
            s++;
        }
    }
    return n;
}

static void set_props_list(struct restable * const r, const uint8_t *tag, size_t len)
{
    while (len > 0) {
        const size_t taglen = scan_uri(tag, ',', '}', len);
        zhe_assert(taglen <= len);
        if (taglen == len) {
            return;
        }
        set_props_one(r, tag, taglen);
        tag += taglen + 1;
        len -= taglen - 1;
    } while(tag[-1] == ',');
}

enum uristore_result zhe_uristore_store(peeridx_t peeridx, zhe_rid_t rid, const uint8_t *uri, size_t urilen_in)
{
    zhe_assert(peeridx <= MAX_PEERS_1); /* MAX_PEERS_1 is self */
    if (urilen_in > ZHE_MAX_URILENGTH) {
        return USR_OVERSIZE;
    }
    const uripos_t urilen = (uripos_t)urilen_in;
    zhe_residx_t free_idx = max_residx + 1;
    /* FIXME: shouldn't scan like this */
    /* FIXME: also: should ensure URI <=> RID relation is bijective */
    ZT(PUBSUB, "uristore_store: store %ju %*.*s", (uintmax_t)rid, (int)urilen, (int)urilen, (char*)uri);
    for (zhe_residx_t idx = 0; idx <= max_residx; idx++) {
        if (ress[idx].rid == rid) {
            const uripos_t sz = zhe_icgcb_getsize(&uris.b, uris.store + ress[idx].uripos);
            ZT(PUBSUB, "uristore_store: check against %u %*.*s", (unsigned)idx, (int)sz, (int)sz, (char*)uris.store + ress[idx].uripos);
            if (sz == urilen && memcmp(uri, uris.store + ress[idx].uripos, urilen) == 0) {
                zhe_bitset_set(ress[idx].peers, peeridx);
                ZT(PUBSUB, "uristore_store: match");
                return USR_OK;
            } else {
                ZT(PUBSUB, "uristore_store: mismatch");
                return USR_MISMATCH;
            }
        } else if (ress[idx].rid == 0 && free_idx == max_residx + 1) {
            free_idx = idx;
        }
    }
    if (free_idx == ZHE_MAX_RESOURCES) {
        ZT(PUBSUB, "uristore_store: no space (max res hit)");
        return USR_NOSPACE;
    }
    void *ptr;
    switch (zhe_icgcb_alloc(&ptr, &uris.b, urilen, free_idx)) {
        case IAR_OK:
            break;
        case IAR_AGAIN:
            ZT(PUBSUB, "uristore_store: again");
            return USR_AGAIN;
        case IAR_NOSPACE:
            ZT(PUBSUB, "uristore_store: no space");
            return USR_NOSPACE;
    }
    ress[free_idx].rid = rid;
    ress[free_idx].uripos = (uripos_t)((uint8_t *)ptr - uris.store);
    ress[free_idx].transient = 0;
    ress[free_idx].reliable = 1;
    memset(ress[free_idx].peers, 0, sizeof(ress[free_idx].peers));
    zhe_bitset_set(ress[free_idx].peers, peeridx);
    memcpy(ptr, uri, urilen_in);
    /* FIXME: should be more careful in checking URI */
    const uint8_t *hash = memchr(uri, '#', urilen_in);
    if (hash) {
        if (hash[1] == '{') {
            set_props_list(&ress[free_idx], hash+2, urilen_in - (size_t)(hash+2 - uri));
        } else {
            set_props_one(&ress[free_idx], hash+1, urilen_in - (size_t)(hash+1 - uri));
        }
    }
    if (free_idx > max_residx) {
        max_residx = free_idx;
    }
   ZT(PUBSUB, "uristore_store: ok, index %u", (unsigned)free_idx);
    return USR_OK;
}

void zhe_uristore_drop(peeridx_t peeridx, zhe_rid_t rid)
{
    for (zhe_residx_t idx = 0; idx <= max_residx; idx++) {
        if (ress[idx].rid == rid) {
            zhe_bitset_clear(ress[idx].peers, peeridx);
            if (zhe_bitset_count(ress[idx].peers, MAX_PEERS_1 + 1) == 0) {
                zhe_icgcb_free(&uris.b, uris.store + ress[idx].uripos);
                ress[idx].rid = 0;
            }
        }
    }
}

bool zhe_uristore_geturi(unsigned idx, zhe_rid_t *rid, zhe_paysize_t *sz, const uint8_t **uri)
{
    const struct restable * const r = &ress[idx];
    if (r->rid == 0) {
        return false;
    } else {
        *rid = r->rid;
        *uri = uris.store + r->uripos;
        *sz = zhe_icgcb_getsize(&uris.b, *uri);
        return true;
    }
}

void zhe_uristore_reset_peer(peeridx_t peeridx)
{
    /* FIXME: find a better way */
    for (zhe_residx_t idx = 0; idx <= max_residx; idx++) {
        if (ress[idx].rid !=0 && zhe_bitset_test(ress[idx].peers, peeridx)) {
            zhe_bitset_clear(ress[idx].peers, peeridx);
            if (zhe_bitset_count(ress[idx].peers, MAX_PEERS_1 + 1) == 0) {
                ZT(PUBSUB, "uristore_reset_peer: drop %ju", (uintmax_t)ress[idx].rid);
                zhe_icgcb_free(&uris.b, uris.store + ress[idx].uripos);
                ress[idx].rid = 0;
            }
        }
    }
}

static void move_cb(uripos_t ref, void *newptr, void *arg)
{
    ress[ref].uripos = (uripos_t)((uint8_t *)newptr - uris.store);
}

void zhe_uristore_gc(void)
{
    zhe_icgcb_gc(&uris.b, move_cb, NULL);
}

#endif /* ZHE_MAX_URISPACE > 0 */
