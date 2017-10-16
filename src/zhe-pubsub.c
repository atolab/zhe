#include <string.h>
#include <limits.h>

#include "zhe-config-deriv.h"
#include "zhe-tracing.h"
#include "zhe-assert.h"
#include "zhe-msg.h"
#include "zhe-int.h"
#include "zhe-pack.h"
#include "zhe-pubsub.h"
#include "zhe-bitset.h"

/* Start using a RID-to-subscription mapping if ZHE_MAX_SUBSCRIPTIONS is over this threshold */
#define RID_TABLE_THRESHOLD 32

struct subtable {
    /* ID of the resource subscribed to (could also be a SID, actually) */
    zhe_rid_t rid;
    zhe_subidx_t next;

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
/* FIXME: should support deleting pubs, subs, &c., and then a we need a linked list instead of a simple maximum */
static zhe_subidx_t max_subidx;
#if ZHE_MAX_SUBSCRIPTIONS > RID_TABLE_THRESHOLD
static zhe_subidx_t rid2sub[ZHE_MAX_RID+1];
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
static DECL_BITSET(pubs_rsubs, ZHE_MAX_PUBLICATIONS);

struct precommit {
#if MAX_PEERS == 0
    DECL_BITSET(rsubs, ZHE_MAX_PUBLICATIONS);
#else
    DECL_BITSET(rsubs, ZHE_MAX_RID+1);
#endif
    uint8_t result;
    zhe_rid_t invalid_rid;
};

#if MAX_PEERS > 0
struct peer_rsubs {
    DECL_BITSET(rsubs, ZHE_MAX_RID+1);
};
static struct peer_rsubs peers_rsubs[MAX_PEERS];
#endif

static struct precommit precommit[MAX_PEERS_1];
static struct precommit precommit_curpkt;

void zhe_decl_note_error(uint8_t bitmask, zhe_rid_t rid)
{
    ZT(PUBSUB, "decl_note_error: mask %x rid %ju", bitmask, (uintmax_t)rid);
    if (precommit_curpkt.result == 0) {
        precommit_curpkt.invalid_rid = rid;
    }
    precommit_curpkt.result |= bitmask;
}

void zhe_rsub_register(peeridx_t peeridx, zhe_rid_t rid, uint8_t submode)
{
#if MAX_PEERS == 0
    zhe_pubidx_t pubidx;
    zhe_assert(rid != 0);
    for (pubidx.idx = 0; pubidx.idx < ZHE_MAX_PUBLICATIONS; pubidx.idx++) {
        if (pubs[pubidx.idx].rid == rid) {
            break;
        }
    }
    if (submode == SUBMODE_PUSH && pubidx.idx < ZHE_MAX_PUBLICATIONS) {
        zhe_bitset_set(precommit_curpkt.rsubs, pubidx.idx);
    } else {
        zhe_decl_note_error(((submode != SUBMODE_PUSH) ? 1 : 0) | ((pubidx.idx >= ZHE_MAX_PUBLICATIONS) ? 2 : 0), rid);
    }
#else
    if (submode == SUBMODE_PUSH && rid <= ZHE_MAX_RID) {
        zhe_bitset_set(precommit_curpkt.rsubs, rid);
    } else {
        zhe_decl_note_error(((submode != SUBMODE_PUSH) ? 1 : 0) | ((rid > ZHE_MAX_RID) ? 2 : 0), rid);
    }
#endif
}

uint8_t zhe_rsub_precommit(peeridx_t peeridx, zhe_rid_t *err_rid)
{
    zhe_assert (precommit_curpkt.result == 0);
    if (precommit[peeridx].result == 0) {
        ZT(PUBSUB, "rsub_precommit peeridx %u ok", peeridx);
        return 0;
    } else {
        uint8_t result = precommit[peeridx].result;
        ZT(PUBSUB, "rsub_precommit peeridx %u result %u", peeridx, result);
        *err_rid = precommit[peeridx].invalid_rid;
        memset(&precommit[peeridx], 0, sizeof(precommit[peeridx]));
        return result;
    }
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
    for (size_t i = 0; i < sizeof(peers_rsubs[peeridx].rsubs); i++) {
        peers_rsubs[peeridx].rsubs[i] |= precommit[peeridx].rsubs[i];
        for (size_t j = 0; j < CHAR_BIT * sizeof(precommit[peeridx].rsubs[i]); j++) {
            if (i+j <= ZHE_MAX_RID && zhe_bitset_test(precommit[peeridx].rsubs, (unsigned)(i+j))) {
                zhe_rid_t rid = (zhe_rid_t)(i+j);
                zhe_pubidx_t pubidx;
                for (pubidx.idx = 0; pubidx.idx < ZHE_MAX_PUBLICATIONS; pubidx.idx++) {
                    if (pubs[pubidx.idx].rid == rid) {
#if ENABLE_TRACING
                        if (!zhe_bitset_test(pubs_rsubs, pubidx.idx)) {
                            ZT(PUBSUB, "pub %u rid %ju: now have remote subs", (unsigned)pubidx.idx, (uintmax_t)rid);
                        }
#endif
                        zhe_bitset_set(pubs_rsubs, pubidx.idx);
                        break;
                    }
                }
            }
        }
    }
#endif
    memset(&precommit[peeridx], 0, sizeof(precommit[peeridx]));
}

void zhe_rsub_precommit_curpkt_abort(peeridx_t peeridx)
{
    memset(&precommit_curpkt, 0, sizeof(precommit_curpkt));
}

void zhe_rsub_precommit_curpkt_done(peeridx_t peeridx)
{
    for (size_t i = 0; i < sizeof(precommit[peeridx].rsubs); i++) {
        precommit[peeridx].rsubs[i] |= precommit_curpkt.rsubs[i];
    }
    if (precommit[peeridx].invalid_rid == 0) {
        precommit[peeridx].invalid_rid = precommit_curpkt.invalid_rid;
    }
    precommit[peeridx].result |= precommit_curpkt.result;
    zhe_rsub_precommit_curpkt_abort(peeridx);
}

void zhe_rsub_clear(peeridx_t peeridx)
{
#if MAX_PEERS == 0
    memset(&pubs_rsubs, 0, sizeof(pubs_rsubs));
#else
    memset(&peers_rsubs[peeridx], 0, sizeof(peers_rsubs[peeridx]));
    zhe_pubidx_t pubidx;
    for (pubidx.idx = 0; pubidx.idx < ZHE_MAX_PUBLICATIONS; pubidx.idx++) {
        const zhe_rid_t rid = pubs[pubidx.idx].rid;
        zhe_assert(rid <= ZHE_MAX_RID);
        if (rid != 0 && zhe_bitset_test(pubs_rsubs, pubidx.idx)) {
            peeridx_t i;
            for (i = 0; i < MAX_PEERS_1; i++) {
                if (zhe_bitset_test(peers_rsubs[i].rsubs, rid)) {
                    break;
                }
            }
            if (i == MAX_PEERS_1) {
                ZT(PUBSUB, "pub %u rid %ju: no more remote subs", (unsigned)pubidx.idx, (uintmax_t)rid);
                zhe_bitset_clear(pubs_rsubs, pubidx.idx);
            }
        }
    }
#endif
    memset(&precommit[peeridx], 0, sizeof(precommit[peeridx]));
    zhe_rsub_precommit_curpkt_abort(peeridx);
}

/////////////////////////////////////////////////////////////////////////////

int zhe_handle_msdata_deliver(zhe_rid_t prid, zhe_paysize_t paysz, const void *pay)
{
#if ZHE_MAX_SUBSCRIPTIONS > RID_TABLE_THRESHOLD
    if (prid > ZHE_MAX_RID || (rid2sub[prid].idx == 0 && subs[0].rid != prid)) {
        /* not subscribed */
        return 1;
    }
    zhe_assert(rid2sub[prid].idx < ZHE_MAX_SUBSCRIPTIONS);
    zhe_assert(subs[rid2sub[prid].idx].rid == prid);
    const struct subtable * const s = &subs[rid2sub[prid].idx];
#else
    zhe_subidx_t k;
    for (k.idx = 0; k.idx < ZHE_MAX_SUBSCRIPTIONS; k.idx++) {
        if (subs[k.idx].rid == prid) {
            break;
        }
    }
    if (k.idx == ZHE_MAX_SUBSCRIPTIONS) {
        return 1;
    }
    const struct subtable * const s = &subs[k.idx];
#endif

    if (s->next.idx == 0) {
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
        for (const struct subtable *t = s; t != &subs[0]; t = &subs[t->next.idx]) {
            if (t->xmitneed > 0) {
                xmitneed[zhe_oc_get_cid(t->oc)] += t->xmitneed;
            }
        }
        for (cid_t cid = 0; cid < N_OUT_CONDUITS; cid++) {
            if (xmitneed[cid] > 0 && !zhe_xmitw_hasspace(zhe_out_conduit_from_cid(0, cid), xmitneed[cid])) {
                return 0;
            }
        }

        s->handler(prid, pay, paysz, s->arg);
        return 1;
    }
}

/////////////////////////////////////////////////////////////////////////////////////

static uint8_t gcommitid;

#define ZHE_MAX_RESOURCES 30
#define MAX3(a,b,c) ((a) > (b) ? ((a) > (c) ? (a) : (c)) : ((b) > (c)) ? (b) : (c))
#define MAX_DECLITEM MAX3(ZHE_MAX_RESOURCES, ZHE_MAX_PUBLICATIONS, ZHE_MAX_SUBSCRIPTIONS)
#if MAX_DECLITEM <= UINT8_MAX-1
typedef uint8_t declitem_idx_t;
#define DECLITEM_IDX_INVALID UINT8_MAX
#elif MAX_DECLITEM <= UINT16_MAX-1
typedef uint16_t declitem_idx_t;
#define DECLITEM_IDX_INVALID UINT16_MAX
#elif MAX_DECLITEM <= UINT32_MAX-1
typedef uint32_t declitem_idx_t;
#define DECLITEM_IDX_INVALID UINT32_MAX
#else
#error "MAX_DECLITEM way larger than expected"
#endif

enum declitem_kind {
    DIK_RESOURCE,
    DIK_PUBLICATION,
    DIK_SUBSCRIPTION
};
#define DECLITEM_KIND_FIRST DIK_RESOURCE
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

static struct pending_decls pending_decls;

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

/////////////////////////////////////////////////////////////////////////////////////

static int send_declare_resource(struct out_conduit *oc, declitem_idx_t res, zhe_time_t tnow)
{
    return 1;
}

static int send_declare_pub(struct out_conduit *oc, declitem_idx_t pub, zhe_time_t tnow)
{
    /* Currently not pushing publication declarations in peer mode */
#if MAX_PEERS == 0
    zhe_msgsize_t from;
    if (pubs[pub].rid == 0) {
        return 1;
    } else if (zhe_oc_pack_mdeclare(oc, 1, WC_DPUB_SIZE, &from, tnow)) {
        ZT(PUBSUB, "sending dpub %d rid %ju", pub, (uintmax_t)pubs[pub].rid);
        zhe_pack_dpub(pubs[pub].rid);
        zhe_oc_pack_mdeclare_done(oc, from, tnow);
        return 1;
    } else {
        ZT(PUBSUB, "postponing dpub %d rid %ju", pub, (uintmax_t)pubs[pub].rid);
        return 0;
    }
#else
    return 1;
#endif
}

static int send_declare_sub(struct out_conduit *oc, declitem_idx_t sub, zhe_time_t tnow)
{
    zhe_msgsize_t from;
    if (subs[sub].rid == 0) {
        return 1;
    } else if (zhe_oc_pack_mdeclare(oc, 1, WC_DSUB_SIZE, &from, tnow)) {
        ZT(PUBSUB, "sending dsub %d rid %ju", sub, (uintmax_t)subs[sub].rid);
        zhe_pack_dsub(subs[sub].rid);
        zhe_oc_pack_mdeclare_done(oc, from, tnow);
        return 1;
    } else {
        ZT(PUBSUB, "postponing dsub %d rid %ju", sub, (uintmax_t)subs[sub].rid);
        return 0;
    }
}

static int send_declare_commit(struct out_conduit *oc, uint8_t commitid, zhe_time_t tnow)
{
    zhe_msgsize_t from;
    if (zhe_oc_pack_mdeclare(oc, 1, WC_DCOMMIT_SIZE, &from, tnow)) {
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

static struct out_conduit *zhe_send_declares1(zhe_time_t tnow, const cursoridx_t cursoridx)
{
    zhe_assert(cursoridx == MULTICAST_CURSORIDX || cursoridx < MAX_PEERS_1);
#if HAVE_UNICAST_CONDUIT
    struct out_conduit * const oc = (cursoridx == MULTICAST_CURSORIDX) ? zhe_out_conduit_from_cid(0, 0) : zhe_out_conduit_from_cid(cursoridx, UNICAST_CID);
#else
    struct out_conduit * const oc = (cursoridx == MULTICAST_CURSORIDX) ? zhe_out_conduit_from_cid(0, 0) : zhe_out_conduit_from_cid(cursoridx, 0);
#endif
    declitem_idx_t idx;
    int done = 0;
    enum declitem_kind kind;

    kind = DECLITEM_KIND_FIRST;
    do {
        if ((idx = pending_decls.cursor[cursoridx][kind]) == DECLITEM_IDX_INVALID) {
            done++;
        } else {
            declitem_idx_t max;
            int success;
            //ZT(PUBSUB, "send_declares_1 cursoridx %u kind %u idx %u cid %u", (unsigned)cursoridx, (unsigned)kind, (unsigned)idx, (unsigned)zhe_oc_get_cid(oc));
            switch (kind) {
                    /* FIXME: the check against max value is ok while we don't delete entities only */
                case DIK_RESOURCE:     success = send_declare_resource(oc, idx, tnow); max = ZHE_MAX_RESOURCES; break;
                case DIK_PUBLICATION:  success = send_declare_pub(oc, idx, tnow); max = max_pubidx.idx+1;  break;
                case DIK_SUBSCRIPTION: success = send_declare_sub(oc, idx, tnow); max = max_subidx.idx+1;  break;
            }
            if (success) {
                /* FIXME: improve iterator over items to declare */
                if (++pending_decls.cursor[cursoridx][kind] == max) {
                    pending_decls.cursor[cursoridx][kind] = DECLITEM_IDX_INVALID;
                    done++;
                }
            }
        }
    } while (kind++ != DECLITEM_KIND_LAST);

    return (done == N_DECLITEM_KINDS) ? oc : NULL;
}

void zhe_send_declares(zhe_time_t tnow)
{
    struct out_conduit *commit_oc;
    if (pending_decls.cnt == 0) {
        zhe_assert(pending_decls.pos == 0);
    } else if ((commit_oc = zhe_send_declares1(tnow, pending_decls.peers[pending_decls.pos])) == NULL) {
        if (++pending_decls.pos == pending_decls.cnt) {
            pending_decls.pos = 0;
        }
    } else if (!send_declare_commit(commit_oc, gcommitid, tnow)) {
        if (++pending_decls.pos == pending_decls.cnt) {
            pending_decls.pos = 0;
        }
    } else {
        gcommitid++;
        pending_decls.peers[pending_decls.pos] = pending_decls.peers[--pending_decls.cnt];
        if (pending_decls.pos == pending_decls.cnt) {
            pending_decls.pos = 0;
        }
    }
}

/////////////////////////////////////////////////////////////////////////////

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
    {
        cursoridx_t idx;
        for (idx = 0; idx < pending_decls.cnt; idx++) {
            if (pending_decls.peers[idx] == MULTICAST_CURSORIDX) {
                break;
            }
        }
        if (idx == pending_decls.cnt + 1) {
            zhe_assert(pending_decls.cnt < sizeof(pending_decls.cursor) / sizeof(pending_decls.cursor[0]));
            pending_decls.peers[pending_decls.cnt++] = MULTICAST_CURSORIDX;
        }
        if (pending_decls.cursor[MULTICAST_CURSORIDX][DIK_PUBLICATION] > pubidx.idx) {
            /* FIXME: double declare ... once you can delete publications */
            pending_decls.cursor[MULTICAST_CURSORIDX][DIK_PUBLICATION] = pubidx.idx;
        }
    }
#else
    for (peeridx_t peeridx = 0; peeridx < MAX_PEERS_1; peeridx++) {
        if (zhe_bitset_test(peers_rsubs[peeridx].rsubs, rid)) {
            ZT(PUBSUB, "publish: %u rid %ju has remote subs", pubidx.idx, (uintmax_t)rid, reliable ? "reliable" : "unreliable");
            zhe_bitset_set(pubs_rsubs, pubidx.idx);
            break;
        }
    }
#endif
    return pubidx;
}

zhe_subidx_t zhe_subscribe(zhe_rid_t rid, zhe_paysize_t xmitneed, unsigned cid, zhe_subhandler_t handler, void *arg)
{
    zhe_subidx_t subidx, nextidx;
    zhe_assert(rid > 0 && rid <= ZHE_MAX_RID);
    for (subidx.idx = 0; subidx.idx < ZHE_MAX_SUBSCRIPTIONS; subidx.idx++) {
        if (subs[subidx.idx].rid == 0) {
            break;
        }
    }
#if ZHE_MAX_SUBSCRIPTIONS > RID_TABLE_THRESHOLD
    nextidx = rid2sub[rid];
#else
    for (nextidx.idx = 0; nextidx.idx < ZHE_MAX_SUBSCRIPTIONS; nextidx.idx++) {
        if (subs[nextidx.idx].rid == rid) {
            break;
        }
    }
    if (nextidx.idx == ZHE_MAX_SUBSCRIPTIONS) {
        nextidx.idx = 0; /* using 0 to signal the end of the list is an ungainly hack, but it works for now */
    }
#endif
    zhe_assert(subidx.idx < ZHE_MAX_SUBSCRIPTIONS);
    subs[subidx.idx].rid = rid;
    subs[subidx.idx].next = nextidx;
    subs[subidx.idx].xmitneed = xmitneed;
    /* FIXME: horrible hack ... */
    subs[subidx.idx].oc = zhe_out_conduit_from_cid(0, (cid_t)cid);
    subs[subidx.idx].handler = handler;
    subs[subidx.idx].arg = arg;
#if ZHE_MAX_SUBSCRIPTIONS > RID_TABLE_THRESHOLD
    rid2sub[rid] = subidx;
#endif
    max_subidx = subidx;
   {
        cursoridx_t idx;
        for (idx = 0; idx < pending_decls.cnt; idx++) {
            if (pending_decls.peers[idx] == MULTICAST_CURSORIDX) {
                break;
            }
        }
        if (idx == pending_decls.cnt + 1) {
            zhe_assert(pending_decls.cnt < sizeof(pending_decls.cursor) / sizeof(pending_decls.cursor[0]));
            pending_decls.peers[pending_decls.cnt++] = MULTICAST_CURSORIDX;
        }
        if (pending_decls.cursor[MULTICAST_CURSORIDX][DIK_SUBSCRIPTION] > subidx.idx) {
            /* FIXME: double declare ... once you can delete publications */
            pending_decls.cursor[MULTICAST_CURSORIDX][DIK_SUBSCRIPTION] = subidx.idx;
        }
    }
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
    if (!zhe_bitset_test(pubs_rsubs, pubidx.idx)) {
        /* success is assured if there are no subscribers */
        return 1;
    }

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
        /* not flushing to allow packing */
        return 1;
    }
}
