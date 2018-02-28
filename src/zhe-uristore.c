#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "zhe-config-deriv.h"
#include "zhe-assert.h"
#include "zhe-bitset.h"
#include "zhe-tracing.h"
#include "zhe-uristore.h"
#include "zhe-uri.h"

/* FIXME: get rid of these two -- or at least pubsub.h? */
#include "zhe-int.h"
#include "zhe-pubsub.h"

#if ZHE_MAX_URISPACE > 0

#define BSEARCH_THRESHOLD 32

static union {
    uint8_t store[sizeof(struct icgcb) + ZHE_MAX_URISPACE];
    struct icgcb b;
} uris;

struct restable {
    zhe_rid_t rid;
    uripos_t uripos;
    uint8_t reliable: 1;
    uint8_t transient: 1;
    uint8_t committed: 1; /* equivalent to (tentative == INVALID || count(peers) > 1), i.e., whether it has been committed */
    peeridx_t tentative; /* INVALID if not tentative, else index of "owning" peer */
    DECL_BITSET(peers, MAX_PEERS_1 + 1); /* self is MAX_PEERS_1, tracks which peers have declared this resource, peers[tentative] is the only tentative one in the set, all others are committed */
};
static zhe_residx_t nres; /* number of known URIs */
static struct restable ress[ZHE_MAX_RESOURCES]; /* contains nres entries where rid != 0 */

/* [0 .. nres-1] indices into ress where rid != 0, sorted in ascending order on rid;
   [nres .. ZHE_MAX_RESOURCES-1] indices into ress where rid == 0 */
/* FIXME: maybe should embed rid in table to reduce the number of cache misses */
static zhe_residx_t ress_idx[ZHE_MAX_RESOURCES];
static zhe_rid_t ress_rid[ZHE_MAX_RESOURCES];

void zhe_uristore_init(void)
{
    zhe_icgcb_init(&uris.b, sizeof(uris));
    memset(&ress, 0, sizeof(ress));
    for (zhe_residx_t i = 0; i < ZHE_MAX_RESOURCES; i++) {
        ress[i].tentative = PEERIDX_INVALID;
        ress_idx[i] = i;
        ress_rid[i] = 0;
    }
    nres = 0;
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

static peeridx_t zhe_uristore_record_tentative(peeridx_t peeridx, zhe_residx_t idx)
{
    if (peeridx == ress[idx].tentative) {
        /* same peer again: that's no problem */
        ZT(PUBSUB, "zhe_uristore_record_tentative: peeridx %u residx %u: repeat", (unsigned)peeridx, (unsigned)idx);
        return PEERIDX_INVALID;
    } else if (ress[idx].tentative == PEERIDX_INVALID) {
        ZT(PUBSUB, "zhe_uristore_record_tentative: peeridx %u residx %u: currently not tentative", (unsigned)peeridx, (unsigned)idx);
        ress[idx].tentative = peeridx;
        return PEERIDX_INVALID;
    } else {
        /* currently tentative for some peer, one with the lowest peer id (not index) wins */
        peeridx_t loser;
        ZT(PUBSUB, "zhe_uristore_record_tentative: peeridx %u residx %u: tentative for peeridx %u", (unsigned)peeridx, (unsigned)idx, (unsigned)ress[idx].tentative);
        if (zhe_compare_peer_ids_for_peeridx(peeridx, ress[idx].tentative) < 0) {
            /* old one doesn't count anymore ... note error for old one so that it will get a "try again" response */
            ZT(PUBSUB, "zhe_uristore_record_tentative: peeridx %u residx %u: taking over", (unsigned)peeridx, (unsigned)idx);
            loser = ress[idx].tentative;
            ress[idx].tentative = peeridx;
        } else {
            ZT(PUBSUB, "zhe_uristore_record_tentative: peeridx %u residx %u: losing", (unsigned)peeridx, (unsigned)idx);
            loser = peeridx;
        }
        zhe_bitset_clear(ress[idx].peers, loser);
        return loser;
    }
}

static enum uristore_result zhe_uristore_store_new(zhe_residx_t free_idx, peeridx_t peeridx, zhe_rid_t rid, const uint8_t *uri, uripos_t urilen, bool tentative)
{
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
    ress[free_idx].committed = !tentative;
    ress[free_idx].tentative = tentative ? peeridx : PEERIDX_INVALID;
    ress[free_idx].transient = 0;
    ress[free_idx].reliable = 1;
    memset(ress[free_idx].peers, 0, sizeof(ress[free_idx].peers));
    zhe_bitset_set(ress[free_idx].peers, peeridx);
    memcpy(ptr, uri, urilen);
    const uint8_t *hash = memchr(uri, '#', urilen);
    if (hash) {
        if (hash[1] == '{') {
            set_props_list(&ress[free_idx], hash+2, urilen - (size_t)(hash+2 - uri));
        } else {
            set_props_one(&ress[free_idx], hash+1, urilen - (size_t)(hash+1 - uri));
        }
    }
    /* update index - it is sorted on ascending RID, move those with RIDs greater than RID */
    zhe_residx_t i;
    for (i = 0; i < nres; i++) {
        if (ress[ress_idx[i]].rid > rid) {
            break;
        }
    }
    memmove(&ress_idx[i+1], &ress_idx[i], (nres-i) * sizeof(ress_idx[i]));
    memmove(&ress_rid[i+1], &ress_rid[i], (nres-i) * sizeof(ress_rid[i]));
    ress_idx[i] = free_idx;
    ress_rid[i] = rid;
    nres++;
    ZT(PUBSUB, "uristore_store: ok, index %u", (unsigned)free_idx);
    return USR_OK;
}

static enum uristore_result zhe_uristore_store_dup(zhe_residx_t idx, peeridx_t peeridx, bool tentative, peeridx_t *loser)
{
    const bool was_committed = ress[idx].committed;
    const bool was_known_for_peer = zhe_bitset_test(ress[idx].peers, peeridx);
    zhe_bitset_set(ress[idx].peers, peeridx);
    if (tentative && was_known_for_peer) {
        /* a tentative declaration for a resource already declared by this peer doesn't change the status and can be ignored */
        return USR_DUPLICATE;
    } else if (tentative) {
        /* a new tentative declaration needs to be recorded, which may result in some peer (perhaps this one) losing its lock */
        *loser = zhe_uristore_record_tentative(peeridx, idx);
        return USR_OK;
    } else {
        /* a non-tentative declaration always clears a possible tentative one from the same peer, but is otherwise a no-op if it was already a committed definition */
        if (ress[idx].tentative == peeridx) {
            ress[idx].tentative = PEERIDX_INVALID;
        }
        if (was_committed) {
            return USR_DUPLICATE;
        } else {
            ress[idx].committed = 1;
            return USR_OK;
        }
    }
}

#if ZHE_MAX_RESOURCES > BSEARCH_THRESHOLD
static int ress_rid_cmp(const void *vk, const void *ve)
{
    const zhe_rid_t *k = vk;
    const zhe_rid_t *e = ve;
    return (*k == *e) ? 0 : (*k < *e) ? -1 : 1;
}
#endif

static zhe_residx_t lookup_rid_ress_idx(zhe_rid_t rid)
{
#if ZHE_MAX_RESOURCES > BSEARCH_THRESHOLD
    if (nres > 32) {
        const zhe_rid_t *e = bsearch(&rid, ress_rid, nres, sizeof(ress_rid[0]), ress_rid_cmp);
        return (e == NULL) ? RESIDX_INVALID : (zhe_residx_t)(e - ress_rid);
    } else {
#endif
    zhe_residx_t i;
    for (i = 0; i < nres; i++) {
        if (ress_rid[i] == rid) {
            return i;
        }
    }
    return RESIDX_INVALID;
#if ZHE_MAX_RESOURCES > BSEARCH_THRESHOLD
    }
#endif
}

static zhe_residx_t lookup_rid(zhe_rid_t rid)
{
    const zhe_residx_t i = lookup_rid_ress_idx(rid);
    return (i == RESIDX_INVALID) ? i : ress_idx[i];
}

enum uristore_result zhe_uristore_store(zhe_residx_t *res_idx, peeridx_t peeridx, zhe_rid_t rid, const uint8_t *uri, size_t urilen_in, bool tentative, peeridx_t *loser)
{
    zhe_assert(peeridx <= MAX_PEERS_1); /* MAX_PEERS_1 is self */
    /* only have to set loser if tentative & result is OK, but initialising it always is actually simpler */
    *loser = PEERIDX_INVALID;
    if (!zhe_urivalid(uri, urilen_in)) {
        return USR_INVALID;
    }
    const uripos_t urilen = (uripos_t)urilen_in;
    ZT(PUBSUB, "uristore_store: store %ju %*.*s", (uintmax_t)rid, (int)urilen, (int)urilen, (char*)uri);
    const zhe_residx_t idx = lookup_rid(rid);
    if (idx != RESIDX_INVALID) {
        const uripos_t sz = zhe_icgcb_getsize(&uris.b, uris.store + ress[idx].uripos);
        ZT(PUBSUB, "uristore_store: check against %u %*.*s", (unsigned)idx, (int)sz, (int)sz, (char*)uris.store + ress[idx].uripos);
        if (sz == urilen && memcmp(uri, uris.store + ress[idx].uripos, urilen) == 0) {
            ZT(PUBSUB, "uristore_store: match");
            *res_idx = idx;
            return zhe_uristore_store_dup(idx, peeridx, tentative, loser);
        } else {
            ZT(PUBSUB, "uristore_store: mismatch");
            return USR_MISMATCH;
        }
    } else if (nres == ZHE_MAX_RESOURCES) {
        ZT(PUBSUB, "uristore_store: no space (max res hit)");
        return USR_NOSPACE;
    } else {
        const zhe_residx_t free_idx = ress_idx[nres];
        *res_idx = free_idx;
        return zhe_uristore_store_new(free_idx, peeridx, rid, uri, urilen, tentative);
    }
}

static void zhe_uristore_drop_idx(peeridx_t peeridx, zhe_residx_t ress_idx_idx)
{
    const zhe_residx_t idx = ress_idx[ress_idx_idx];
    unsigned count;
    zhe_bitset_clear(ress[idx].peers, peeridx);
    count = zhe_bitset_count(ress[idx].peers, MAX_PEERS_1 + 1);
    if (ress[idx].tentative == peeridx) {
        ress[idx].tentative = PEERIDX_INVALID;
    }
    if (ress[idx].tentative != PEERIDX_INVALID && count == 1) {
        ress[idx].committed = 0;
    }
    if (count == 0) {
        ZT(PUBSUB, "uristore_reset_peer: drop %ju", (uintmax_t)ress[idx].rid);
        zhe_icgcb_free(&uris.b, uris.store + ress[idx].uripos);
        ress[idx].rid = 0;
        memmove(&ress_idx[ress_idx_idx], &ress_idx[ress_idx_idx+1], (nres - (ress_idx_idx+1)) * sizeof(ress_idx[0]));
        memmove(&ress_rid[ress_idx_idx], &ress_rid[ress_idx_idx+1], (nres - (ress_idx_idx+1)) * sizeof(ress_rid[0]));
        ress_idx[--nres] = idx;
    }
}

void zhe_uristore_drop(peeridx_t peeridx, zhe_rid_t rid)
{
    const zhe_residx_t ress_idx_idx = lookup_rid_ress_idx(rid);
    if (ress_idx_idx != RESIDX_INVALID) {
        zhe_uristore_drop_idx(peeridx, ress_idx_idx);
    }
}

bool zhe_uristore_geturi_for_idx(zhe_residx_t idx, zhe_rid_t *rid, zhe_paysize_t *sz, const uint8_t **uri, bool *islocal)
{
    const struct restable * const r = &ress[idx];
    if (r->rid == 0 || !r->committed) {
        return false;
    } else {
        *rid = r->rid;
        *uri = uris.store + r->uripos;
        *sz = zhe_icgcb_getsize(&uris.b, *uri);
        *islocal = zhe_bitset_test(r->peers, MAX_PEERS_1);
        return true;
    }
}

void zhe_uristore_iter_init(uristore_iter_t *it)
{
    it->cursor = 0;
}

bool zhe_uristore_iter_next(uristore_iter_t *it, zhe_rid_t *rid, zhe_paysize_t *sz, const uint8_t **uri)
{
    while (it->cursor < nres) {
        bool dummy;
        if (zhe_uristore_geturi_for_idx(ress_idx[it->cursor++], rid, sz, uri, &dummy)) {
            return true;
        }
    }
    return false;
}

bool zhe_uristore_geturi_for_rid(zhe_rid_t rid, zhe_paysize_t *sz, const uint8_t **uri)
{
    const zhe_residx_t idx = lookup_rid(rid);
    if (idx == RESIDX_INVALID) {
        return false;
    } else {
        bool dummy;
        zhe_rid_t dummyrid;
        return zhe_uristore_geturi_for_idx(idx, &dummyrid, sz, uri, &dummy);
    }
}

bool zhe_uristore_getidx_for_rid(zhe_rid_t rid, zhe_residx_t *ret_idx)
{
    const zhe_residx_t idx = lookup_rid(rid);
    if (idx == RESIDX_INVALID) {
        return false;
    } else {
        *ret_idx = idx;
        return true;
    }
}

void zhe_uristore_reset_peer(peeridx_t peeridx)
{
    zhe_residx_t i = 0;
    while (i < nres) {
        const zhe_residx_t idx = ress_idx[i];
        if (zhe_bitset_test(ress[idx].peers, peeridx)) {
            zhe_uristore_drop_idx(peeridx, i);
        } else {
            i++;
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

zhe_residx_t zhe_uristore_nres(void)
{
    return nres;
}

void zhe_uristore_abort_tentative(peeridx_t peeridx)
{
    zhe_residx_t i = 0;
    while (i < nres) {
        const zhe_residx_t idx = ress_idx[i];
        if (ress[idx].tentative == peeridx) {
            zhe_uristore_drop_idx(peeridx, i);
        } else {
            i++;
        }
    }
}

void zhe_uristore_commit_tentative(peeridx_t peeridx)
{
    for (zhe_residx_t i = 0; i < nres; i++) {
        const zhe_residx_t idx = ress_idx[i];
        if (ress[idx].tentative == peeridx) {
            ress[idx].tentative = PEERIDX_INVALID;
            if (!ress[idx].committed) {
                ress[idx].committed = 1;
#if ZHE_MAX_URISPACE > 0 && MAX_PEERS > 0
                /* FIXME: trying so hard to keep uristore free of strange dependencies, this call shouldn't be here */
                zhe_update_subs_for_resource_decl(ress[idx].rid);
#endif
            }
        }
    }
}

#endif /* ZHE_MAX_URISPACE > 0 */
