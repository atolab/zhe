#include <string.h>
#include <limits.h>
#include <stdlib.h>

#include "zhe-config-deriv.h"
#include "zhe-tracing.h"
#include "zhe-assert.h"
#include "zhe-msg.h"
#include "zhe-int.h"
#include "zhe-pack.h"
#include "zhe-pubsub.h"
#include "zhe-bitset.h"
#include "zhe-uristore.h"
#include "zhe-uri.h"
#include "zhe-simpleset.h"
#include "zhe-arylist.h"

struct subtable {
    /* ID of the resource subscribed to (could also be a SID, actually) */
    zhe_rid_t rid;
    zhe_subidx_t next; /* circular */
    /* Minimum number of bytes that must be available in transmit window in the given conduit
     before calling, must include message overhead (for writing SDATA -- that is, no PRID
     present -- worst case is 9 bytes with a payload limit of 127 bytes and 32-bit RIDs) */
    struct out_conduit *oc;
    zhe_paysize_t xmitneed;

    /* */
    void *arg;
    zhe_subhandler_t handler;
};
static struct subtable subs[ZHE_MAX_SUBSCRIPTIONS];
typedef struct rid2subtable {
    zhe_rid_t rid;
    zhe_subidx_t subidx;
} rid2subtable_t;
/* FIXME: should support deleting pubs, subs, &c., and then a we need a linked list instead of a simple maximum; also use max_subidx for number of entries in use also fails at that point*/
static zhe_subidx_t max_subidx;

#define RID2SUB_RID_CMP(key, elem) ((key) == (elem) ? 0 : ((key) < (elem) ? -1 : 1))
#define RID2SUB_RID(elem) ((elem).rid)
MAKE_PACKAGE_SPEC(SIMPLESET, (static, zhe_rid2sub, zhe_rid_t, struct rid2subtable, zhe_subidx_t, ZHE_MAX_SUBSCRIPTIONS), type)
MAKE_PACKAGE_BODY(SIMPLESET, (static, zhe_rid2sub, zhe_rid_t, struct rid2subtable, zhe_subidx_t, .idx, RID2SUB_RID_CMP, RID2SUB_RID, ZHE_MAX_SUBSCRIPTIONS), init, search, insert)
static zhe_rid2sub_t rid2sub;

#if ZHE_MAX_URISPACE > 0 && MAX_PEERS > 0
MAKE_PACKAGE_SPEC(ARYLIST, (static, zhe_residx2sub, zhe_subidx_t, zhe_subidx_t, ZHE_MAX_SUBSCRIPTIONS), type, iter_type)
MAKE_PACKAGE_BODY(ARYLIST, (static, zhe_residx2sub, zhe_subidx_t, zhe_subidx_t, .idx, ZHE_MAX_SUBSCRIPTIONS), init, insert, count, iter_init, iter_next)
static zhe_residx2sub_t residx2sub[ZHE_MAX_RESOURCES];
#endif

struct pubtable {
    struct out_conduit *oc;
    zhe_rid_t rid;
};
static struct pubtable pubs[ZHE_MAX_PUBLICATIONS];
/* FIXME: should support deleting pubs, subs, &c., and then a we need a linked list instead of a simple maximum */
static zhe_pubidx_t max_pubidx;

/* FIXME: should switch from publisher determines reliability to subscriber determines
 reliability, i.e., publisher reliability bit gets set to
 (foldr or False $ map isReliableSub subs).  Keeping the reliability information
 separate from pubs has the advantage of saving quite a few bytes. */
static DECL_BITSET(pubs_isrel, ZHE_MAX_PUBLICATIONS);

/* Without URIs, the only matching rule is on numerical equality, and in that case a single bit suffices (and saves a lot of space).  Otherwise, we count the number of remote subs for each pub (but how many remote subs can I have? In principle ZHE_MAX_RESOURCES*MAX_PEERS */
#if ZHE_MAX_URISPACE > 0 && MAX_PEERS > 0
static zhe_rsubcount_t pubs_rsubcounts[ZHE_MAX_PUBLICATIONS];
#else
static DECL_BITSET(pubs_rsubs, ZHE_MAX_PUBLICATIONS);
#endif

#if MAX_PEERS > 0
typedef struct {
#if ZHE_MAX_SUBSCRIPTIONS_PER_PEER <= UINT8_MAX
    uint8_t rididx;
#elif ZHE_MAX_SUBSCRIPTIONS_PER_PEER <= UINT16_MAX
    uint16_t rididx;
#elif ZHE_MAX_SUBSCRIPTIONS_PER_PEER <= UINT32_MAX
    uint32_t rididx;
#elif ZHE_MAX_SUBSCRIPTIONS_PER_PEER <= UINT64_MAX
    uint64_t rididx;
#endif
} zhe_rsubidx_t;
#define RID_CMP(key, elem) ((key) == (elem) ? 0 : ((key) < (elem) ? -1 : 1))
#define RID_RID(elem) ((elem))
MAKE_PACKAGE_SPEC(SIMPLESET, (static, zhe_ridtable, zhe_rid_t, zhe_rid_t, zhe_rsubidx_t, ZHE_MAX_SUBSCRIPTIONS_PER_PEER), type, iter_type)
MAKE_PACKAGE_BODY(SIMPLESET, (static, zhe_ridtable, zhe_rid_t, zhe_rid_t, zhe_rsubidx_t, .rididx, RID_CMP, RID_RID, ZHE_MAX_SUBSCRIPTIONS_PER_PEER), search, count, insert, iter_init, iter_next)
#if ZHE_MAX_URISPACE == 0
MAKE_PACKAGE_BODY(SIMPLESET, (static, zhe_ridtable, zhe_rid_t, zhe_rid_t, zhe_rsubidx_t, .rididx, RID_CMP, RID_RID, ZHE_MAX_SUBSCRIPTIONS_PER_PEER), contains)
#endif
#endif

/* FIXME: at some point #rsubs in precommit + #rsubs in peers_rsubs get added and limited, but as overlap between the two sets is allowed, it can reject a valid declaration */

struct precommit {
#if MAX_PEERS == 0
    DECL_BITSET(rsubs, ZHE_MAX_PUBLICATIONS);
#else
    zhe_ridtable_t rsubs; /* FIXME: this should be limited by accepting a limited transaction size */
#endif
    uint8_t result;
    zhe_rid_t invalid_rid;
};

#if MAX_PEERS > 0
struct peer_rsubs {
    zhe_ridtable_t rsubs;
};
static struct peer_rsubs peers_rsubs[MAX_PEERS];
#endif

static struct precommit precommit[MAX_PEERS_1];
static struct precommit precommit_curpkt;

#if ZHE_MAX_URISPACE > 0 && MAX_PEERS > 0
static bool pub_sub_match(zhe_rid_t a, zhe_rid_t b)
{
    /* Matching is symmetric */
    if (a == b) {
        return true;
    } else {
        zhe_paysize_t asz, bsz;
        const uint8_t *auri, *buri;
        if (!zhe_uristore_geturi_for_rid(a, &asz, &auri) || !zhe_uristore_geturi_for_rid(b, &bsz, &buri)) {
            return false;
        } else {
            return zhe_urimatch(auri, asz, buri, bsz);
        }
    }
}
#endif

void zhe_pubsub_init(void)
{
    memset(subs, 0, sizeof(subs));
    max_subidx.idx = 0;
    zhe_rid2sub_init(&rid2sub);
#if ZHE_MAX_URISPACE > 0 && MAX_PEERS > 0
    for (zhe_residx_t i = 0; i < ZHE_MAX_RESOURCES; i++) {
        zhe_residx2sub_init(&residx2sub[i]);
    }
#endif
    memset(pubs, 0, sizeof(pubs));
    max_pubidx.idx = 0;
    memset(pubs_isrel, 0, sizeof(pubs_isrel));
#if ZHE_MAX_URISPACE > 0 && MAX_PEERS > 0
    memset(pubs_rsubcounts, 0, sizeof(pubs_rsubcounts));
#else
    memset(pubs_rsubs, 0, sizeof(pubs_rsubs));
#endif
#if MAX_PEERS > 0
    memset(peers_rsubs, 0, sizeof(peers_rsubs));
#endif
    memset(&precommit_curpkt, 0, sizeof(precommit_curpkt));
    memset(precommit, 0, sizeof(precommit));
}

void zhe_decl_note_error_curpkt(enum zhe_declstatus status, zhe_rid_t rid)
{
    zhe_assert(status != ZHE_DECL_OK && (unsigned)status < UINT8_MAX);
    ZT(PUBSUB, "decl_note_error: status %u rid %ju", (unsigned)status, (uintmax_t)rid);
    if (precommit_curpkt.result == (uint8_t)ZHE_DECL_OK || precommit_curpkt.result == (uint8_t)ZHE_DECL_AGAIN) {
        precommit_curpkt.invalid_rid = rid;
        precommit_curpkt.result = (uint8_t)status;
    }
}

void zhe_decl_note_error_somepeer(peeridx_t peeridx, enum zhe_declstatus status, zhe_rid_t rid)
{
    zhe_assert(status != ZHE_DECL_OK && (unsigned)status < UINT8_MAX);
    ZT(PUBSUB, "decl_note_error_somepeer: peeridx %u status %u rid %ju", (unsigned)peeridx, (unsigned)status, (uintmax_t)rid);
    if (precommit[peeridx].result == (uint8_t)ZHE_DECL_OK || precommit[peeridx].result == (uint8_t)ZHE_DECL_AGAIN) {
        precommit[peeridx].invalid_rid = rid;
        precommit[peeridx].result = (uint8_t)status;
    }
}

static void rsub_register_committed(peeridx_t peeridx, zhe_rid_t rid, uint8_t submode)
{
    ZT(PUBSUB, "zhe_rsub_register_committed peeridx %u rid %ju", peeridx, (uintmax_t)rid);
#if MAX_PEERS == 0
    zhe_pubidx_t pubidx;
    zhe_assert(rid != 0);
    for (pubidx.idx = 0; pubidx.idx < ZHE_MAX_PUBLICATIONS; pubidx.idx++) {
        if (pubs[pubidx.idx].rid == rid) {
            break;
        }
    }
    if (submode != SUBMODE_PUSH) {
        zhe_decl_note_error_curpkt(ZHE_DECL_UNSUPPORTED, rid);
    } else if (pubidx.idx >= ZHE_MAX_PUBLICATIONS) {
        zhe_decl_note_error_curpkt(ZHE_DECL_INVALID, rid);
    } else {
        zhe_bitset_set(pubs_rsubs, pubidx.idx);
    }
#else
    if (submode != SUBMODE_PUSH) {
        zhe_decl_note_error_curpkt(ZHE_DECL_UNSUPPORTED, rid);
    } else if (rid >= ZHE_MAX_RID) {
        zhe_decl_note_error_curpkt(ZHE_DECL_INVALID, rid);
    } else {
        switch (zhe_ridtable_insert(&peers_rsubs[peeridx].rsubs, rid)) {
            case SSIR_EXISTS:
                ZT(PUBSUB, "zhe_rsub_register_committed rid %ju - already known", (uintmax_t)rid);
                break;
            case SSIR_NOSPACE:
                zhe_decl_note_error_curpkt(ZHE_DECL_NOSPACE, rid);
                break;
            case SSIR_SUCCESS:
                ZT(PUBSUB, "zhe_rsub_register_committed rid %ju - adding", (uintmax_t)rid);
                for (zhe_pubidx_t pubidx = (zhe_pubidx_t){ 0 }; pubidx.idx < ZHE_MAX_PUBLICATIONS; pubidx.idx++) {
                    /* FIXME: can/should cache URI for "rid" */
#if ZHE_MAX_URISPACE == 0
                    if (pubs[pubidx.idx].rid == rid) {
                        if (!zhe_bitset_test(pubs_rsubs, pubidx.idx)) {
                            ZT(PUBSUB, "pub %u rid %ju: now have remote subs", (unsigned)pubidx.idx, (uintmax_t)rid);
                        }
                        zhe_bitset_set(pubs_rsubs, pubidx.idx);
                        break;
                    }
#else
                    if (pub_sub_match(pubs[pubidx.idx].rid, rid)) {
                        if (pubs_rsubcounts[pubidx.idx] == 0) {
                            ZT(PUBSUB, "pub %u rid %ju: now have remote subs", (unsigned)pubidx.idx, (uintmax_t)rid);
                        }
                        pubs_rsubcounts[pubidx.idx]++;
                        ZT(DEBUG, "zhe_rsub_register_committed: pub %u rid %ju: rsubcount now %u", (unsigned)pubidx.idx, (uintmax_t)rid, (unsigned)pubs_rsubcounts[pubidx.idx]);
                    }
#endif
                }
                break;
        }
    }
#endif
}

static void rsub_register_tentative(peeridx_t peeridx, zhe_rid_t rid, uint8_t submode)
{
#if MAX_PEERS == 0
    zhe_pubidx_t pubidx;
    zhe_assert(rid != 0);
    for (pubidx.idx = 0; pubidx.idx < ZHE_MAX_PUBLICATIONS; pubidx.idx++) {
        if (pubs[pubidx.idx].rid == rid) {
            break;
        }
    }
    if (submode != SUBMODE_PUSH) {
        zhe_decl_note_error_curpkt(ZHE_DECL_UNSUPPORTED, rid);
    } else if (pubidx.idx >= ZHE_MAX_PUBLICATIONS) {
        zhe_decl_note_error_curpkt(ZHE_DECL_INVALID, rid);
    } else {
        zhe_bitset_set(precommit_curpkt.rsubs, pubidx.idx);
    }
#else
    if (submode != SUBMODE_PUSH) {
        zhe_decl_note_error_curpkt(ZHE_DECL_UNSUPPORTED, rid);
    } else if (rid >= ZHE_MAX_RID) {
        zhe_decl_note_error_curpkt(ZHE_DECL_INVALID, rid);
    } else {
        switch (zhe_ridtable_insert(&precommit_curpkt.rsubs, rid)) {
            case SSIR_EXISTS:
            case SSIR_SUCCESS:
                break;
            case SSIR_NOSPACE:
                zhe_decl_note_error_curpkt(ZHE_DECL_NOSPACE, rid);
                break;
        }
    }
#endif
}

void zhe_rsub_register(peeridx_t peeridx, zhe_rid_t rid, uint8_t submode, bool tentative)
{
    if (tentative) {
        rsub_register_tentative(peeridx, rid, submode);
    } else {
        rsub_register_committed(peeridx, rid, submode);
    }
}

uint8_t zhe_rsub_precommit_status_for_Cflag(peeridx_t peeridx, zhe_rid_t *err_rid)
{
    zhe_assert (precommit_curpkt.result == 0);
    if (precommit[peeridx].result != 0) {
        uint8_t result = precommit[peeridx].result;
        ZT(PUBSUB, "rsub_precommit_status peeridx %u result %u", peeridx, result);
        *err_rid = precommit[peeridx].invalid_rid;
        return result;
    } else {
        ZT(PUBSUB, "rsub_precommit_status peeridx %u ok", peeridx);
        return 0;
    }
}

uint8_t zhe_rsub_precommit(peeridx_t peeridx, zhe_rid_t *err_rid)
{
    zhe_assert (precommit_curpkt.result == 0);
    if (precommit[peeridx].result != 0) {
        uint8_t result = precommit[peeridx].result;
        ZT(PUBSUB, "rsub_precommit peeridx %u result %u", peeridx, result);
        *err_rid = precommit[peeridx].invalid_rid;
        memset(&precommit[peeridx], 0, sizeof(precommit[peeridx]));
        return result;
#if MAX_PEERS > 0
    } else if (zhe_ridtable_count(&precommit[peeridx].rsubs).rididx > ZHE_MAX_SUBSCRIPTIONS_PER_PEER - zhe_ridtable_count(&peers_rsubs[peeridx].rsubs).rididx) {
        ZT(PUBSUB, "rsub_precommit peeridx %u failure because precommit set not guaranteed to fit", peeridx);
        *err_rid = precommit[peeridx].rsubs.elems[0]; /* FIXME: shouldn't peek inside; and should perhaps choose the RID with more care */
        memset(&precommit[peeridx], 0, sizeof(precommit[peeridx]));
        return (uint8_t)ZHE_DECL_NOSPACE;
#endif
    } else {
        ZT(PUBSUB, "rsub_precommit peeridx %u ok", peeridx);
        return 0;
    }
}

void zhe_rsub_precommit_curpkt_abort(peeridx_t peeridx)
{
    memset(&precommit_curpkt, 0, sizeof(precommit_curpkt));
}

void zhe_rsub_commit(peeridx_t peeridx)
{
    ZT(PUBSUB, "rsub_commit peeridx %u", peeridx);
    zhe_assert(precommit[peeridx].result == 0);
#if MAX_PEERS == 0
    for (size_t i = 0; i < sizeof(pubs_rsubs); i++) {
        pubs_rsubs[i] |= precommit[peeridx].rsubs[i];
    }
#else
    zhe_ridtable_iter_t it;
    zhe_rid_t rid;
    zhe_ridtable_iter_init(&it, &precommit[peeridx].rsubs);
    while (zhe_ridtable_iter_next(&it, &rid)) {
        switch (zhe_ridtable_insert(&peers_rsubs[peeridx].rsubs, rid)) {
            case SSIR_EXISTS:
                ZT(PUBSUB, "zhe_rsub_commit rid %ju - already known", (uintmax_t)rid);
                break;
            case SSIR_NOSPACE:
                /* precommit to avoid this case by checking whether peers_rsubs has sufficient space for all entries in precommit */
                zhe_assert(0);
                break;
            case SSIR_SUCCESS:
                ZT(PUBSUB, "zhe_rsub_commit rid %ju - adding", (uintmax_t)rid);
                for (zhe_pubidx_t pubidx = (zhe_pubidx_t){ 0 }; pubidx.idx < ZHE_MAX_PUBLICATIONS; pubidx.idx++) {
                    /* FIXME: can/should cache URI for "rid" */
#if ZHE_MAX_URISPACE == 0
                    if (pubs[pubidx.idx].rid == rid) {
                        if (!zhe_bitset_test(pubs_rsubs, pubidx.idx)) {
                            ZT(PUBSUB, "pub %u rid %ju: now have remote subs", (unsigned)pubidx.idx, (uintmax_t)rid);
                        }
                        zhe_bitset_set(pubs_rsubs, pubidx.idx);
                        break;
                    }
#else
                    if (pub_sub_match(pubs[pubidx.idx].rid, rid)) {
                        if (pubs_rsubcounts[pubidx.idx] == 0) {
                            ZT(PUBSUB, "pub %u rid %ju: now have remote subs", (unsigned)pubidx.idx, (uintmax_t)rid);
                        }
                        pubs_rsubcounts[pubidx.idx]++;
                        ZT(DEBUG, "rsub_commit: pub %u rid %ju: rsubcount now %u", (unsigned)pubidx.idx, (uintmax_t)rid, (unsigned)pubs_rsubcounts[pubidx.idx]);
                    }
#endif
                }
        }
    }
#endif
    zhe_rsub_precommit_curpkt_abort(peeridx);
}

void zhe_rsub_precommit_curpkt_done(peeridx_t peeridx)
{
#if MAX_PEERS == 0
    for (size_t i = 0; i < sizeof(precommit[peeridx].rsubs); i++) {
        precommit[peeridx].rsubs[i] |= precommit_curpkt.rsubs[i];
    }
#else
    /* FIXME: this can be done FAR MORE EFFICIENTLY without any trouble; then again, perhaps one shouldn't even treat the curpkt as a special thing in this manner */
    zhe_ridtable_iter_t it;
    zhe_rid_t rid;
    zhe_ridtable_iter_init(&it, &precommit[peeridx].rsubs);
    while (zhe_ridtable_iter_next(&it, &rid)) {
        switch (zhe_ridtable_insert(&precommit[peeridx].rsubs, rid)) {
            case SSIR_EXISTS:
            case SSIR_SUCCESS:
                break;
            case SSIR_NOSPACE:
                /* setting error on current packet will propagate it a few lines down to a precommit error */
                zhe_decl_note_error_curpkt(ZHE_DECL_NOSPACE, rid);
                break;
        }
    }
#endif
    if (precommit_curpkt.result != (uint8_t)ZHE_DECL_OK) {
        zhe_decl_note_error_somepeer(peeridx, precommit_curpkt.result, precommit_curpkt.invalid_rid);
    }
    zhe_rsub_precommit_curpkt_abort(peeridx);
}

void zhe_reset_peer_rsubs(peeridx_t peeridx)
{
#if MAX_PEERS == 0
    memset(&pubs_rsubs, 0, sizeof(pubs_rsubs));
#elif ZHE_MAX_URISPACE == 0
    memset(&peers_rsubs[peeridx], 0, sizeof(peers_rsubs[peeridx]));
    zhe_pubidx_t pubidx;
    for (pubidx.idx = 0; pubidx.idx < ZHE_MAX_PUBLICATIONS; pubidx.idx++) {
        const zhe_rid_t rid = pubs[pubidx.idx].rid;
        zhe_assert(rid <= ZHE_MAX_RID);
        if (rid != 0 && zhe_bitset_test(pubs_rsubs, pubidx.idx)) {
            peeridx_t i;
            for (i = 0; i < MAX_PEERS_1; i++) {
                if (zhe_ridtable_contains(&peers_rsubs[i].rsubs, rid)) {
                    break;
                }
            }
            if (i == MAX_PEERS_1) {
                ZT(PUBSUB, "pub %u rid %ju: no more remote subs", (unsigned)pubidx.idx, (uintmax_t)rid);
                zhe_bitset_clear(pubs_rsubs, pubidx.idx);
            }
        }
    }
#else
    zhe_ridtable_iter_t it;
    zhe_rid_t rid;
    zhe_ridtable_iter_init(&it, &precommit[peeridx].rsubs);
    while (zhe_ridtable_iter_next(&it, &rid)) {
        zhe_pubidx_t pubidx;
        for (pubidx.idx = 0; pubidx.idx < ZHE_MAX_PUBLICATIONS; pubidx.idx++) {
            /* FIXME: can/should cache URI for "rid" */
            if (pub_sub_match(pubs[pubidx.idx].rid, rid)) {
                pubs_rsubcounts[pubidx.idx]--;
                if (pubs_rsubcounts[pubidx.idx] == 0) {
                    ZT(PUBSUB, "pub %u rid %ju: no more remote subs", (unsigned)pubidx.idx, (uintmax_t)rid);
                }
                ZT(DEBUG, "zhe_rsub_clear: pub %u rid %ju: rsubcounts now %u", (unsigned)pubidx.idx, (uintmax_t)rid, (unsigned)pubs_rsubcounts[pubidx.idx]--);
            }
        }
    }
    memset(&peers_rsubs[peeridx], 0, sizeof(peers_rsubs[peeridx]));
#endif
    memset(&precommit[peeridx], 0, sizeof(precommit[peeridx]));
    zhe_rsub_precommit_curpkt_abort(peeridx);
}

/////////////////////////////////////////////////////////////////////////////

#if ZHE_MAX_URISPACE > 0
/* A WriteData should be delivered to all matching subscriptions or to none (and then retried later) -- delivering to some but not all seems like a really bad idea! -- but that means we first need to check the the available space in transmit windows.  Obviously doing the URI matching more often than strictly necessary is not a good idea -- indeed it is bad enough with caching ... perhaps so bad that it would be best to handle this as part of housekeeping, bit by bit ...  FIXME: for now, let's just cache. */
static zhe_subidx_t zhe_handle_mwdata_matches[ZHE_MAX_SUBSCRIPTIONS];

int zhe_handle_mwdata_deliver(zhe_paysize_t urisz, const uint8_t *uri, zhe_paysize_t paysz, const void *pay)
{
    zhe_subidx_t nm = { 0 };
    for (zhe_subidx_t k = { 0 }; k.idx < ZHE_MAX_SUBSCRIPTIONS; k.idx++) {
        const struct subtable * const s = &subs[k.idx];
        zhe_paysize_t suburisz;
        const uint8_t *suburi;
        if (zhe_uristore_geturi_for_rid(s->rid, &suburisz, &suburi) && zhe_urimatch(uri, urisz, suburi, suburisz)) {
            zhe_handle_mwdata_matches[nm.idx++] = k;
        }
    }
    /* FIXME: this doesn't work for unicast conduits; perhaps should speed things up in the trivial cases */
    zhe_paysize_t xmitneed[N_OUT_CONDUITS];
    memset(xmitneed, 0, sizeof(xmitneed));
    for (zhe_subidx_t k = { 0 }; k.idx < nm.idx; k.idx++) {
        const struct subtable *s = &subs[k.idx];
        if (s->xmitneed > 0) {
            xmitneed[zhe_oc_get_cid(s->oc)] += s->xmitneed;
        }
    }
    for (cid_t cid = 0; cid < N_OUT_CONDUITS; cid++) {
        if (xmitneed[cid] > 0 && !zhe_xmitw_hasspace(zhe_out_conduit_from_cid(0, cid), xmitneed[cid])) {
            return 0;
        }
    }
    for (zhe_subidx_t k = { 0 }; k.idx < nm.idx; k.idx++) {
        const struct subtable *s = &subs[k.idx];
        /* FIXME: which resource id should we pass to the handler? 0 is not a valid one, so that's kinda reasonable */
        s->handler(0, pay, paysz, s->arg);
    }
    return 1;
}
#endif

static int zhe_handle_msdata_deliver_anon(zhe_rid_t prid, zhe_paysize_t paysz, const void *pay)
{
    zhe_subidx_t subidx;
    if (!zhe_rid2sub_search(&rid2sub, prid, &subidx)) {
        return 1;
    }
    const struct subtable *s = &subs[subidx.idx];
    if (s->next.idx == subidx.idx) {
        if (s->xmitneed == 0 || zhe_xmitw_hasspace(s->oc, s->xmitneed)) {
            /* Do note that "xmitneed" had better include overhead! */
            s->handler(prid, pay, paysz, s->arg);
            return 1;
        } else {
            return 0;
        }
    } else {
        /* FIXME: this doesn't work for unicast conduits */
        zhe_paysize_t xmitneed[N_OUT_CONDUITS];
        memset(xmitneed, 0, sizeof(xmitneed));
        const struct subtable *t;
        t = s;
        do {
            if (t->xmitneed > 0) {
                xmitneed[zhe_oc_get_cid(t->oc)] += t->xmitneed;
            }
            t = &subs[t->next.idx];
        } while (t != s);
        for (cid_t cid = 0; cid < N_OUT_CONDUITS; cid++) {
            if (xmitneed[cid] > 0 && !zhe_xmitw_hasspace(zhe_out_conduit_from_cid(0, cid), xmitneed[cid])) {
                return 0;
            }
        }
        t = s;
        do {
            t->handler(prid, pay, paysz, s->arg);
            t = &subs[t->next.idx];
        } while (t != s);
        return 1;
    }
}

int zhe_handle_msdata_deliver(zhe_rid_t prid, zhe_paysize_t paysz, const void *pay)
{
#if ZHE_MAX_URISPACE > 0 && MAX_PEERS > 0
    /* FIXME: this doesn't work for unicast conduits; perhaps should speed things up in the trivial cases */
    zhe_residx_t prid_idx;
    if (!zhe_uristore_getidx_for_rid(prid, &prid_idx)) {
        return zhe_handle_msdata_deliver_anon(prid, paysz, pay);
    } else {
        zhe_paysize_t xmitneed[N_OUT_CONDUITS];
        zhe_residx2sub_iter_t it;
        zhe_subidx_t subidx;
        memset(xmitneed, 0, sizeof(xmitneed));
        zhe_residx2sub_iter_init(&it, &residx2sub[prid_idx]);
        while (zhe_residx2sub_iter_next(&it, &subidx)) {
            const struct subtable *s = &subs[subidx.idx];
            if (s->xmitneed > 0) {
                xmitneed[zhe_oc_get_cid(s->oc)] += s->xmitneed;
            }
        }
        for (cid_t cid = 0; cid < N_OUT_CONDUITS; cid++) {
            if (xmitneed[cid] > 0 && !zhe_xmitw_hasspace(zhe_out_conduit_from_cid(0, cid), xmitneed[cid])) {
                return 0;
            }
        }
        zhe_residx2sub_iter_init(&it, &residx2sub[prid_idx]);
        while (zhe_residx2sub_iter_next(&it, &subidx)) {
            const struct subtable *s = &subs[subidx.idx];
            /* FIXME: which resource id should we pass to the handler? 0 is not a valid one, so that's kinda reasonable */
            s->handler(prid, pay, paysz, s->arg);
        }
        return 1;
    }
#else
    return zhe_handle_msdata_deliver_anon(prid, paysz, pay);
#endif
}

/////////////////////////////////////////////////////////////////////////////////////

static uint8_t gcommitid;

#define MAX2(a,b) ((a) > (b) ? (a) : (b))
#if ZHE_MAX_URISPACE > 0
#define MAX3(a,b,c) (MAX2((a), MAX2((b), (c))))
#define MAX_DECLITEM MAX3(ZHE_MAX_RESOURCES, ZHE_MAX_PUBLICATIONS, ZHE_MAX_SUBSCRIPTIONS)
#else
#define MAX_DECLITEM MAX2(ZHE_MAX_PUBLICATIONS, ZHE_MAX_SUBSCRIPTIONS)
#endif
#if MAX_DECLITEM <= UINT8_MAX-1
typedef uint8_t declitem_idx_t;
#define DECLITEM_IDX_INVALID UINT8_MAX
#elif MAX_DECLITEM <= UINT16_MAX-1
typedef uint16_t declitem_idx_t;
#define DECLITEM_IDX_INVALID UINT16_MAX
#elif MAX_DECLITEM <= UINT32_MAX-1
typedef uint32_t declitem_idx_t;
#define DECLITEM_IDX_INVALID UINT32_MAX
#elif MAX_DECLITEM <= UINT64_MAX-1
typedef uint64_t declitem_idx_t;
#define DECLITEM_IDX_INVALID UINT64_MAX
#else
#error "MAX_DECLITEM way larger than expected"
#endif

enum declitem_kind {
#if ZHE_MAX_URISPACE > 0
    DIK_RESOURCE,
#endif
    DIK_PUBLICATION,
    DIK_SUBSCRIPTION
};
#if ZHE_MAX_URISPACE > 0
#define DECLITEM_KIND_FIRST DIK_RESOURCE
#else
#define DECLITEM_KIND_FIRST DIK_PUBLICATION
#endif
#define DECLITEM_KIND_LAST DIK_SUBSCRIPTION
#define N_DECLITEM_KINDS ((int)DECLITEM_KIND_LAST + 1)

typedef peeridx_t cursoridx_t;

#define MULTICAST_CURSORIDX (MAX_PEERS > 1 && HAVE_UNICAST_CONDUIT ? MAX_PEERS : 0)

struct pending_decls {
    cursoridx_t cnt;
    cursoridx_t pos;
    cursoridx_t peers[MULTICAST_CURSORIDX + 1];
    declitem_idx_t cursor[MULTICAST_CURSORIDX + 1][N_DECLITEM_KINDS];
};

struct decl_results {
    zhe_rid_t rid;     /* a resource id reported back by an error response */
    uint8_t status;
    DECL_BITSET(waiting, MAX_PEERS_1); /* peers we still require a response from */
};

static struct pending_decls pending_decls;
static struct decl_results decl_results;

void zhe_accept_peer_sched_hist_decls(peeridx_t peeridx)
{
#if MAX_PEERS > 1 && HAVE_UNICAST_CONDUIT
    const cursoridx_t cursoridx = peeridx;
    for (cursoridx_t idx = 0; idx < pending_decls.cnt; idx++) {
        zhe_assert(pending_decls.peers[idx] != cursoridx);
    }
    zhe_assert(pending_decls.cnt < sizeof(pending_decls.cursor) / sizeof(pending_decls.cursor[0]));
    pending_decls.peers[pending_decls.cnt++] = cursoridx;
#else
    const cursoridx_t cursoridx = MULTICAST_CURSORIDX;
    if (pending_decls.cnt == 0) {
        pending_decls.peers[pending_decls.cnt++] = cursoridx;
    } else {
        zhe_assert(pending_decls.cnt == 1 && pending_decls.peers[pending_decls.cnt] == cursoridx);
    }
#endif
    enum declitem_kind kind = DECLITEM_KIND_FIRST;
    do {
        pending_decls.cursor[cursoridx][kind] = 0;
    } while(kind++ != DECLITEM_KIND_LAST);
}

void zhe_reset_peer_unsched_hist_decls(peeridx_t peeridx)
{
#if MAX_PEERS > 1 && HAVE_UNICAST_CONDUIT
    cursoridx_t cursoridx = peeridx;
    for (cursoridx_t idx = 0; idx < pending_decls.cnt; idx++) {
        if (pending_decls.peers[idx] != cursoridx) {
            continue;
        }

        pending_decls.peers[idx] = pending_decls.peers[--pending_decls.cnt];
        if (pending_decls.pos == pending_decls.cnt) {
            pending_decls.pos = 0;
        }
        return;
    }
#endif
}

static void sched_fresh_declare(enum declitem_kind kind, declitem_idx_t itemidx)
{
    cursoridx_t idx;
    /* See if we are still working on doing fresh declarations: those use a cursor index of
     MULTICAST instead of a valid peer idx */
    for (idx = 0; idx < pending_decls.cnt; idx++) {
        if (pending_decls.peers[idx] == MULTICAST_CURSORIDX) {
            break;
        }
    }
    /* if we are not currently performing fresh declarations, schedule new ones */
    if (idx == pending_decls.cnt) {
        zhe_assert(pending_decls.cnt < sizeof(pending_decls.cursor) / sizeof(pending_decls.cursor[0]));
        pending_decls.peers[pending_decls.cnt++] = MULTICAST_CURSORIDX;
    }
    /* if current fresh declarations have progressed past itemidx, restart from itemidx - which
     means that any declarations with a higher index will be repeated, so once deleting of local
     resources, publications or subscriptions is actually implemented, duplicate declarations
     will be the result */
    if (pending_decls.cursor[MULTICAST_CURSORIDX][kind] > itemidx) {
        pending_decls.cursor[MULTICAST_CURSORIDX][kind] = itemidx;
    }
}

/////////////////////////////////////////////////////////////////////////////////////

#if ZHE_MAX_URISPACE > 0
static void send_declare_resource(struct out_conduit *oc, declitem_idx_t *cursor, bool committed, zhe_time_t tnow)
{
    zhe_paysize_t urisz;
    const uint8_t *uri;
    zhe_rid_t rid;
    bool islocal;
    if (*cursor == ZHE_MAX_RESOURCES) {
        *cursor = DECLITEM_IDX_INVALID;
    } else if (!zhe_uristore_geturi_for_idx((zhe_residx_t)*cursor, &rid, &urisz, &uri, &islocal) || !islocal) {
        (*cursor)++;
    } else {
        const zhe_paysize_t declsz = 1 + zhe_pack_ridreq(rid) + zhe_pack_vle16req(urisz) + urisz;
        zhe_msgsize_t from;
        if (zhe_oc_pack_mdeclare(oc, committed, 1, declsz, &from, tnow)) {
            ZT(PUBSUB, "sending dres %ju rid %ju %*.*s", (uintmax_t)*cursor, (uintmax_t)rid, (int)urisz, (int)urisz, (char*)uri);
            zhe_pack_dresource(rid, urisz, uri);
            zhe_oc_pack_mdeclare_done(oc, from, tnow);
            (*cursor)++;
        } else {
            ZT(PUBSUB, "postponing dres %ju rid %ju %*.*s", (uintmax_t)*cursor, (uintmax_t)rid, (int)urisz, (int)urisz, (char*)uri);
        }
    }
}
#endif

static void send_declare_pub(struct out_conduit *oc, declitem_idx_t *cursor, bool committed, zhe_time_t tnow)
{
    /* Currently not pushing publication declarations in peer mode */
#if MAX_PEERS == 0
    declitem_idx_t pub = *cursor;
    zhe_msgsize_t from;
    if (*cursor == ZHE_MAX_PUBLICATIONS) {
        *cursor = DECLITEM_IDX_INVALID;
    } if (pubs[pub].rid == 0) {
        (*cursor)++;
    } else if (zhe_oc_pack_mdeclare(oc, committed, 1, WC_DPUB_SIZE, &from, tnow)) {
        ZT(PUBSUB, "sending dpub %ju rid %ju", (uintmax_t)pub, (uintmax_t)pubs[pub].rid);
        zhe_pack_dpub(pubs[pub].rid);
        zhe_oc_pack_mdeclare_done(oc, from, tnow);
        (*cursor)++;
    } else {
        ZT(PUBSUB, "postponing dpub %ju rid %ju", (uintmax_t)pub, (uintmax_t)pubs[pub].rid);
    }
#else
    (*cursor)++;
#endif
}

static void send_declare_sub(struct out_conduit *oc, declitem_idx_t *cursor, bool committed, zhe_time_t tnow)
{
    declitem_idx_t sub = *cursor;
    zhe_msgsize_t from;
    if (*cursor == ZHE_MAX_SUBSCRIPTIONS) {
        *cursor = DECLITEM_IDX_INVALID;
    } else if (subs[sub].rid == 0) {
        (*cursor)++;
    } else if (zhe_oc_pack_mdeclare(oc, committed, 1, WC_DSUB_SIZE, &from, tnow)) {
        ZT(PUBSUB, "sending dsub %d rid %ju", (uintmax_t)sub, (uintmax_t)subs[sub].rid);
        zhe_pack_dsub(subs[sub].rid);
        zhe_oc_pack_mdeclare_done(oc, from, tnow);
        (*cursor)++;
    } else {
        ZT(PUBSUB, "postponing dsub %d rid %ju", (uintmax_t)sub, (uintmax_t)subs[sub].rid);
    }
}

static int send_declare_commit(struct out_conduit *oc, uint8_t commitid, zhe_time_t tnow)
{
    zhe_msgsize_t from;
    if (zhe_oc_pack_mdeclare(oc, false, 1, WC_DCOMMIT_SIZE, &from, tnow)) {
        ZT(PUBSUB, "sending commit %u", commitid);
        zhe_pack_dcommit(commitid);
        zhe_oc_pack_mdeclare_done(oc, from, tnow);
        zhe_pack_msend();
        return 1;
    } else {
        ZT(PUBSUB, "postponing commit %u", commitid);
        return 0;
    }
}

static bool zhe_send_declares1(zhe_time_t tnow, const cursoridx_t cursoridx, struct out_conduit **commit_oc)
{
    peeridx_t peeridx;
    cid_t cid;
    bool committed;
    zhe_assert(cursoridx == MULTICAST_CURSORIDX || cursoridx < MAX_PEERS_1);
    if (cursoridx == MULTICAST_CURSORIDX) {
        committed = false;
        peeridx = 0;
#if HAVE_UNICAST_CONDUIT
        cid = UNICAST_CID;
#else
        cid = 0;
#endif
    } else {
        committed = true;
        peeridx = 0;
        cid = 0;
    }
    struct out_conduit * const oc = zhe_out_conduit_from_cid(peeridx, cid);
    if (!zhe_out_conduit_is_connected(peeridx, cid)) {
        enum declitem_kind kind;
        kind = DECLITEM_KIND_FIRST;
        do {
            pending_decls.cursor[cursoridx][kind] = DECLITEM_IDX_INVALID;
        } while (kind ++ != DECLITEM_KIND_LAST);
        *commit_oc = NULL;
        return true;
    } else {
        int done = 0;
        enum declitem_kind kind;
        kind = DECLITEM_KIND_FIRST;
        do {
            declitem_idx_t *idx = &pending_decls.cursor[cursoridx][kind];
            if (*idx == DECLITEM_IDX_INVALID) {
                done++;
            } else {
                switch (kind) {
#if ZHE_MAX_URISPACE > 0
                    case DIK_RESOURCE:     send_declare_resource(oc, idx, committed, tnow); break;
#endif
                    case DIK_PUBLICATION:  send_declare_pub(oc, idx, committed, tnow); break;
                    case DIK_SUBSCRIPTION: send_declare_sub(oc, idx, committed, tnow); break;
                }
            }
        } while (kind++ != DECLITEM_KIND_LAST);
        *commit_oc = oc;
        return (done == N_DECLITEM_KINDS);
    }
}

void zhe_send_declares(zhe_time_t tnow)
{
    struct out_conduit *commit_oc;
    if (pending_decls.cnt == 0) {
        zhe_assert(pending_decls.pos == 0);
    } else {
        const bool fresh = (pending_decls.peers[pending_decls.pos] == MULTICAST_CURSORIDX);
        if (fresh && (zhe_bitset_count(decl_results.waiting, MAX_PEERS_1) > 0 || decl_results.status != (uint8_t)ZHE_DECL_OK)) {
            /* can't send declarations in a transaction until a previous error result has been collected */
#if 0 /* Maybe allow historical ones? But it can possibly cause the historical ones to race ahead, and I don't want */
            if (++pending_decls.pos == pending_decls.cnt) {
                pending_decls.pos = 0;
            }
#endif
        } else {
            if (!zhe_send_declares1(tnow, pending_decls.peers[pending_decls.pos], &commit_oc)) {
                if (++pending_decls.pos == pending_decls.cnt) {
                    pending_decls.pos = 0;
                }
            } else if (fresh && commit_oc != NULL && !send_declare_commit(commit_oc, gcommitid, tnow)) {
                if (++pending_decls.pos == pending_decls.cnt) {
                    pending_decls.pos = 0;
                }
            } else {
                if (fresh) {
                    for (peeridx_t peeridx = 0; peeridx <= MAX_PEERS_1; peeridx++) {
                        if (zhe_established_peer(peeridx)) {
                            zhe_bitset_set(decl_results.waiting, peeridx);
                        }
                    }
                    if (commit_oc != NULL) {
                        gcommitid++;
                    }
                }
                pending_decls.peers[pending_decls.pos] = pending_decls.peers[--pending_decls.cnt];
                if (pending_decls.pos == pending_decls.cnt) {
                    pending_decls.pos = 0;
                }
            }
        }
    }
}

void zhe_reset_peer_declstatus(peeridx_t peeridx)
{
    zhe_bitset_clear(decl_results.waiting, peeridx);
}

void zhe_note_declstatus(peeridx_t peeridx, uint8_t status, zhe_rid_t rid)
{
    if (zhe_bitset_test(decl_results.waiting, peeridx)) {
        zhe_bitset_clear(decl_results.waiting, peeridx);
        if (status != (uint8_t)ZHE_DECL_OK && (decl_results.status == (uint8_t)ZHE_DECL_OK || decl_results.status == (uint8_t)ZHE_DECL_AGAIN)) {
            decl_results.status = DRESULT_IS_VALID_DECLSTATUS(status) ? status : (uint8_t)ZHE_DECL_OTHER;
            decl_results.rid = rid;
            ZT(PUBSUB, "**** FIXME: handle AGAIN case ****\n");
        }
    }
}

enum zhe_declstatus zhe_get_declstatus(zhe_rid_t *rid)
{
    if (zhe_bitset_count(decl_results.waiting, MAX_PEERS_1) > 0) {
        /* it returns pending until all results have been received, even though an error result could potentially be returned sooner */
        return ZHE_DECL_PENDING;
    } else {
        const uint8_t s = decl_results.status;
        if (rid) { *rid = decl_results.rid; }
        decl_results.status = (uint8_t)ZHE_DECL_OK;
        decl_results.rid = 0;
        return (enum zhe_declstatus)s;
    }
}
/////////////////////////////////////////////////////////////////////////////

#if ZHE_MAX_URISPACE > 0
bool zhe_rid_in_use_anonymously(zhe_rid_t rid)
{
    zhe_paysize_t dummysz;
    const uint8_t *dummyuri;
    for (zhe_pubidx_t pubidx = (zhe_pubidx_t){0}; pubidx.idx < ZHE_MAX_PUBLICATIONS; pubidx.idx++) {
        if (pubs[pubidx.idx].rid == rid) {
            return !zhe_uristore_geturi_for_rid(rid, &dummysz, &dummyuri);
        }
    }
    for (zhe_subidx_t subidx = (zhe_subidx_t){0}; subidx.idx < ZHE_MAX_PUBLICATIONS; subidx.idx++) {
        if (subs[subidx.idx].rid == rid) {
            return !zhe_uristore_geturi_for_rid(rid, &dummysz, &dummyuri);
        }
    }
    return false;
}
#endif

#if ZHE_MAX_URISPACE > 0 && MAX_PEERS > 0
void zhe_update_subs_for_resource_decl(zhe_rid_t rid)
{
    zhe_paysize_t itsz;
    zhe_residx_t residx;
    const uint8_t *ituri;
    ZT(PUBSUB, "zhe_update_subs_for_resource_decl rid %ju", (uintmax_t)rid);
    zhe_uristore_geturi_for_rid(rid, &itsz, &ituri);
    zhe_uristore_getidx_for_rid(rid, &residx);
    for (zhe_subidx_t subidx = (zhe_subidx_t){0}; subidx.idx <= max_subidx.idx; subidx.idx++) {
        const zhe_rid_t subrid = subs[subidx.idx].rid;
        if (subrid != 0) {
            zhe_paysize_t subsz;
            const uint8_t *suburi;
            if (zhe_uristore_geturi_for_rid(subrid, &subsz, &suburi) && zhe_urimatch(suburi, subsz, ituri, itsz)) {
                (void)zhe_residx2sub_insert(&residx2sub[residx], subidx);
                ZT(PUBSUB, "zhe_update_subs_for_resource_decl rid %ju: add sub %u (now #%u)", (uintmax_t)rid, subidx.idx, (unsigned)zhe_residx2sub_count(&residx2sub[residx]).idx);
            }
        }
    }
}

static void zhe_update_subs_for_sub_decl(zhe_rid_t rid, zhe_subidx_t subidx)
{
    zhe_paysize_t subsz;
    const uint8_t *suburi;
    ZT(PUBSUB, "zhe_update_subs_for_sub_decl rid %ju subidx %u", (uintmax_t)rid, (unsigned)subidx.idx);
    if (zhe_uristore_geturi_for_rid(rid, &subsz, &suburi)) {
        uristore_iter_t it;
        zhe_rid_t itrid;
        zhe_paysize_t itsz;
        const uint8_t *ituri;
        zhe_uristore_iter_init(&it);
        while (zhe_uristore_iter_next(&it, &itrid, &itsz, &ituri)) {
            if (zhe_urimatch(suburi, subsz, ituri, itsz)) {
                zhe_residx_t residx;
                zhe_uristore_getidx_for_rid(itrid, &residx);
                (void)zhe_residx2sub_insert(&residx2sub[residx], subidx);
                ZT(PUBSUB, "zhe_update_subs_for_sub_decl rid %ju subidx %u - add to rid %ju (now #%u)", (uintmax_t)rid, (unsigned)subidx.idx, (uintmax_t)itrid, (unsigned)zhe_residx2sub_count(&residx2sub[residx]).idx);
            }
        }
    }
}
#endif

bool zhe_declare_resource(zhe_rid_t rid, const char *uri)
{
#if ZHE_MAX_URISPACE > 0
    if (zhe_rid_in_use_anonymously(rid)) {
        /* Not allowed to declare a resource after having declared subscriptions or publications, to guarantee that subs & pubs not backed by a URI really are simple (at least locally) */
        return false;
    } else {
        zhe_residx_t residx;
        peeridx_t loser;
        const size_t urisz = strlen(uri);
        const enum uristore_result res = zhe_uristore_store(&residx, URISTORE_PEERIDX_SELF, rid, (const uint8_t *)uri, urisz, false, &loser);
        switch (res) {
            case USR_OK:
#if ZHE_MAX_URISPACE > 0 && MAX_PEERS > 0
                zhe_update_subs_for_resource_decl(rid);
#endif
                sched_fresh_declare(DIK_RESOURCE, residx);
                return true;
            case USR_DUPLICATE:
                return true;
            case USR_AGAIN:
            case USR_INVALID:
            case USR_NOSPACE:
            case USR_MISMATCH:
                return false;
        }
    }
#else
    return false;
#endif
}

zhe_pubidx_t zhe_publish(zhe_rid_t rid, unsigned cid, int reliable)
{
    /* We will be publishing rid, dynamically allocating a "pubidx" for it and scheduling a
     DECLARE message that informs the broker of this.  By scheduling it, we avoid the having
     to send a reliable message when the transmit window is full.  */
    zhe_pubidx_t pubidx;
    zhe_assert(rid > 0 && rid <= ZHE_MAX_RID);
    for (pubidx.idx = 0; pubidx.idx < ZHE_MAX_PUBLICATIONS; pubidx.idx++) {
        if (pubs[pubidx.idx].rid == 0) {
            break;
        }
    }
    zhe_assert(pubidx.idx < ZHE_MAX_PUBLICATIONS);
    zhe_assert(!zhe_bitset_test(pubs_isrel, pubidx.idx));
    zhe_assert(cid < N_OUT_CONDUITS);
    pubs[pubidx.idx].rid = rid;
    /* FIXME: horrible hack ... */
    pubs[pubidx.idx].oc = zhe_out_conduit_from_cid(0, (cid_t)cid);
    max_pubidx = pubidx;
    if (reliable) {
        zhe_bitset_set(pubs_isrel, pubidx.idx);
    }
    ZT(PUBSUB, "publish: %u rid %ju (%s)", pubidx.idx, (uintmax_t)rid, reliable ? "reliable" : "unreliable");
#if MAX_PEERS == 0
    sched_fresh_declare(DIK_PUBLICATION, pubidx.idx);
#elif ZHE_MAX_URISPACE == 0
    for (peeridx_t peeridx = 0; peeridx < MAX_PEERS_1; peeridx++) {
        if (zhe_ridtable_contains(&peers_rsubs[peeridx].rsubs, rid)) {
            ZT(PUBSUB, "publish: %u rid %ju has remote subs", pubidx.idx, (uintmax_t)rid);
            zhe_bitset_set(pubs_rsubs, pubidx.idx);
            break;
        }
    }
#else
    for (peeridx_t peeridx = 0; peeridx < MAX_PEERS_1; peeridx++) {
        if (!zhe_established_peer(peeridx)) {
            continue;
        }
        zhe_ridtable_iter_t it;
        zhe_rid_t subrid;
        zhe_ridtable_iter_init(&it, &peers_rsubs[peeridx].rsubs);
        while (zhe_ridtable_iter_next(&it, &subrid)) {
            /* FIXME: can/should cache URI for publisher */
            if (pub_sub_match(rid, subrid)) {
                if (pubs_rsubcounts[pubidx.idx] == 0) {
                    ZT(PUBSUB, "pub %u rid %ju: has remote subs", (unsigned)pubidx.idx, (uintmax_t)rid);
                }
                pubs_rsubcounts[pubidx.idx]++;
                ZT(DEBUG, "zhe_publish: pub %u rid %ju: rsubcount now %u", (unsigned)pubidx.idx, (uintmax_t)rid, (unsigned)pubs_rsubcounts[pubidx.idx]);
            }
        }
    }
#endif
    return pubidx;
}

zhe_subidx_t zhe_subscribe(zhe_rid_t rid, zhe_paysize_t xmitneed, unsigned cid, zhe_subhandler_t handler, void *arg)
{
    zhe_subidx_t subidx;
    zhe_assert(rid > 0 && rid <= ZHE_MAX_RID);
    zhe_assert(max_subidx.idx < ZHE_MAX_SUBSCRIPTIONS);
    if (subs[max_subidx.idx].rid == 0) {
        subidx = max_subidx;
    } else {
        subidx = max_subidx; /* FIXME: all this "no delete possible" is no good */
        subidx.idx++;
    }
    zhe_subidx_t nextidx;
    if (!zhe_rid2sub_search(&rid2sub, rid, &nextidx)) {
        nextidx = max_subidx;
    }
    zhe_assert(subidx.idx < ZHE_MAX_SUBSCRIPTIONS);
    subs[subidx.idx].rid = rid;
    subs[subidx.idx].next = nextidx;
    subs[subidx.idx].xmitneed = xmitneed;
    /* FIXME: horrible hack ... */
    subs[subidx.idx].oc = zhe_out_conduit_from_cid(0, (cid_t)cid);
    subs[subidx.idx].handler = handler;
    subs[subidx.idx].arg = arg;
    /* FIXME: this fails badly when we can delete subscriptions */
    max_subidx = subidx;
    (void)zhe_rid2sub_insert(&rid2sub, (rid2subtable_t){ .rid = rid, .subidx = subidx });
#if ZHE_MAX_URISPACE > 0 && MAX_PEERS > 0
    zhe_update_subs_for_sub_decl(rid, subidx);
#endif
    sched_fresh_declare(DIK_SUBSCRIPTION, subidx.idx);
    ZT(PUBSUB, "subscribe: %u rid %ju", subidx.idx, (uintmax_t)rid);
    return subidx;
}

int zhe_write(zhe_pubidx_t pubidx, const void *data, zhe_paysize_t sz, zhe_time_t tnow)
{
    /* returns 0 on failure and 1 on success; the only defined failure case is a full transmit
     window for reliable pulication while remote subscribers exist */
    struct out_conduit * const oc = pubs[pubidx.idx].oc;
    int relflag;
    zhe_assert(pubs[pubidx.idx].rid != 0);
#if ZHE_MAX_URISPACE == 0 || MAX_PEERS == 0
    if (!zhe_bitset_test(pubs_rsubs, pubidx.idx)) {
        /* success is assured if there are no subscribers */
        return 1;
    }
#else
    if (pubs_rsubcounts[pubidx.idx] == 0) {
        /* success is assured if there are no subscribers */
        return 1;
    }
#endif

    relflag = zhe_bitset_test(pubs_isrel, pubidx.idx);

    if (zhe_oc_am_draining_window(oc)) {
        return !relflag;
    } else if (!zhe_oc_pack_msdata(oc, relflag, pubs[pubidx.idx].rid, sz, tnow)) {
        /* for reliable, a full window means failure; for unreliable it is a non-issue */
        return !relflag;
    } else {
        zhe_oc_pack_msdata_payload(oc, relflag, sz, data);
        zhe_oc_pack_msdata_done(oc, relflag, tnow);
#if LATENCY_BUDGET == 0
        zhe_pack_msend();
#endif
        return 1;
    }
}

int zhe_write_uri(const char *uri, const void *data, zhe_paysize_t sz, zhe_time_t tnow)
{
    size_t urisz = strlen(uri);
    if (!zhe_urivalid((const uint8_t *)uri, urisz)) {
        return -1;
    } else if (!zhe_out_conduit_is_connected(0, 0)) {
        return 1;
    } else {
        struct out_conduit * const oc = zhe_out_conduit_from_cid(0, 0);
        if (zhe_oc_am_draining_window(oc)) {
            return 0;
        } else if (!zhe_oc_pack_mwdata(oc, 1, (zhe_paysize_t)urisz, uri, sz, tnow)) {
            return 0;
        } else {
            zhe_oc_pack_msdata_payload(oc, 1, sz, data);
            zhe_oc_pack_msdata_done(oc, 1, tnow);
#if LATENCY_BUDGET == 0
            zhe_pack_msend();
#endif
            return 1;
        }
    }
}
