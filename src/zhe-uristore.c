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
    DECL_BITSET(peers, MAX_PEERS_1 + 1); /* self is MAX_PEERS_1, tracks which peers have declared this resource */
};
static zhe_residx_t nres; /* number of known URIs */
static struct restable ress[ZHE_MAX_RESOURCES]; /* contains nres entries where rid != 0 */

/* [0 .. nres-1] indices into ress where rid != 0, sorted in ascending order on rid;
   [nres .. ZHE_MAX_RESOURCES-1] indices into ress where rid == 0 */
/* FIXME: maybe should embed rid in table to reduce the number of cache misses */
static zhe_residx_t rid2idx[ZHE_MAX_RESOURCES];

void zhe_uristore_init(void)
{
    zhe_icgcb_init(&uris.b, sizeof(uris));
    memset(&ress, 0, sizeof(ress));
    for (zhe_residx_t i = 0; i < ZHE_MAX_RESOURCES; i++) {
        ress[i].tentative = PEERIDX_INVALID;
        rid2idx[i] = i;
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

static int rid2idx_rid_cmp(const void *vk, const void *ve)
{
    const zhe_rid_t *k = vk;
    const zhe_residx_t *e = ve;
    return (*k == ress[*e].rid) ? 0 : (*k < ress[*e].rid) ? -1 : 1;
}

static int rid2idx_cmp(const void *va, const void *vb)
{
    const zhe_residx_t *a = va;
    const zhe_residx_t *b = vb;
    return (ress[*a].rid == ress[*b].rid) ? 0 : (ress[*a].rid < ress[*b].rid) ? -1 : 1;
}

enum uristore_result zhe_uristore_store(zhe_residx_t *res_idx, peeridx_t peeridx, zhe_rid_t rid, const uint8_t *uri, size_t urilen_in, bool tentative)
{
    zhe_assert(peeridx <= MAX_PEERS_1); /* MAX_PEERS_1 is self */
    if (!zhe_urivalid(uri, urilen_in)) {
        return USR_INVALID;
    }
    const uripos_t urilen = (uripos_t)urilen_in;
    ZT(PUBSUB, "uristore_store: store %ju %*.*s", (uintmax_t)rid, (int)urilen, (int)urilen, (char*)uri);
    const zhe_residx_t *e = bsearch(&rid, rid2idx, nres, sizeof(rid2idx[0]), rid2idx_rid_cmp);
    if (e != NULL) {
        const zhe_residx_t idx = *e;
        const uripos_t sz = zhe_icgcb_getsize(&uris.b, uris.store + ress[idx].uripos);
        ZT(PUBSUB, "uristore_store: check against %u %*.*s", (unsigned)idx, (int)sz, (int)sz, (char*)uris.store + ress[idx].uripos);
        if (sz == urilen && memcmp(uri, uris.store + ress[idx].uripos, urilen) == 0) {
            ZT(PUBSUB, "uristore_store: match");
            *res_idx = idx;
            if (zhe_bitset_test(ress[idx].peers, peeridx)) {
                return USR_DUPLICATE;
            } else {
                zhe_bitset_set(ress[idx].peers, peeridx);
                if (!tentative) {
                    ress[idx].committed = 1;
                }
                return USR_OK;
            }
        } else {
            ZT(PUBSUB, "uristore_store: mismatch");
            return USR_MISMATCH;
        }
    } else if (nres == ZHE_MAX_RESOURCES) {
        ZT(PUBSUB, "uristore_store: no space (max res hit)");
        return USR_NOSPACE;
    } else {
        const zhe_residx_t free_idx = rid2idx[nres];
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
        ress[free_idx].transient = 0;
        ress[free_idx].reliable = 1;
        memset(ress[free_idx].peers, 0, sizeof(ress[free_idx].peers));
        zhe_bitset_set(ress[free_idx].peers, peeridx);
        memcpy(ptr, uri, urilen_in);
        const uint8_t *hash = memchr(uri, '#', urilen_in);
        if (hash) {
            if (hash[1] == '{') {
                set_props_list(&ress[free_idx], hash+2, urilen_in - (size_t)(hash+2 - uri));
            } else {
                set_props_one(&ress[free_idx], hash+1, urilen_in - (size_t)(hash+1 - uri));
            }
        }
        nres++;
        qsort(rid2idx, nres, sizeof(rid2idx[0]), rid2idx_cmp);
        *res_idx = free_idx;
        ZT(PUBSUB, "uristore_store: ok, index %u", (unsigned)free_idx);
    }
    return USR_OK;
}

static void zhe_uristore_drop_idx(peeridx_t peeridx, zhe_residx_t rid2idx_idx)
{
    const zhe_residx_t idx = rid2idx[rid2idx_idx];
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
        memmove(&rid2idx[rid2idx_idx], &rid2idx[rid2idx_idx+1], (nres - (rid2idx_idx+1)) * sizeof(rid2idx[0]));
        rid2idx[--nres] = idx;
    }
}

void zhe_uristore_drop(peeridx_t peeridx, zhe_rid_t rid)
{
    zhe_residx_t *e = bsearch(&rid, rid2idx, nres, sizeof(rid2idx[0]), rid2idx_rid_cmp);
    if (e != NULL) {
        zhe_uristore_drop_idx(peeridx, (zhe_residx_t)(e - rid2idx));
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
        if (zhe_uristore_geturi_for_idx(rid2idx[it->cursor++], rid, sz, uri, &dummy)) {
            return true;
        }
    }
    return false;
}

bool zhe_uristore_geturi_for_rid(zhe_rid_t rid, zhe_paysize_t *sz, const uint8_t **uri)
{
    const zhe_residx_t *e = bsearch(&rid, rid2idx, nres, sizeof(rid2idx[0]), rid2idx_rid_cmp);
    if (e == NULL) {
        return false;
    } else {
        bool dummy;
        zhe_rid_t dummyrid;
        return zhe_uristore_geturi_for_idx(rid2idx[*e], &dummyrid, sz, uri, &dummy);
    }
}

bool zhe_uristore_getidx_for_rid(zhe_rid_t rid, zhe_residx_t *idx)
{
    const zhe_residx_t *e = bsearch(&rid, rid2idx, nres, sizeof(rid2idx[0]), rid2idx_rid_cmp);
    if (e == NULL) {
        return false;
    } else {
        *idx = rid2idx[*e];
        return true;
    }
}

void zhe_uristore_reset_peer(peeridx_t peeridx)
{
    zhe_residx_t i = 0;
    while (i < nres) {
        const zhe_residx_t idx = rid2idx[i];
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

peeridx_t zhe_uristore_record_tentative(peeridx_t peeridx, zhe_residx_t idx)
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

void zhe_uristore_abort_tentative(peeridx_t peeridx)
{
    zhe_residx_t i = 0;
    while (i < nres) {
        const zhe_residx_t idx = rid2idx[i];
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
        const zhe_residx_t idx = rid2idx[i];
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
