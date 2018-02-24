/* -*- mode: c; c-basic-offset: 4; fill-column: 95; -*- */
#include <string.h>
#include <limits.h>
#include <inttypes.h>

#include "zhe-assert.h"
#include "zhe-config-deriv.h"
#include "zhe-msg.h"
#include "zhe-int.h"
#include "zhe-tracing.h"
#include "zhe.h"
#include "zhe-pack.h"
#include "zhe-unpack.h"
#include "zhe-bitset.h"
#include "zhe-pubsub.h"
#include "zhe-binheap.h"

#if ZHE_MAX_URISPACE > 0
#include "zhe-uristore.h"
#include "zhe-uri.h"
#endif

#define PEERST_UNKNOWN       0
#define PEERST_OPENING_MIN   1
#define PEERST_OPENING_MAX   5
#define PEERST_ESTABLISHED 255

static union {
    const struct peerid v;
    struct peerid v_nonconst;
} ownid_union;
#define ownid (ownid_union.v)

struct in_conduit {
    seq_t seq;                    /* next seq to be delivered */
    seq_t lseqpU;                 /* latest seq known to exist, plus UNIT */
    seq_t useq;                   /* next unreliable seq to be delivered */
    uint8_t synched: 1;           /* whether a synch was received since (re)establishing the connection */
    uint8_t usynched: 1;          /* whether some unreliable data was received since (re)establishing the connection */
    zhe_time_t tack;              /* time of most recent ack sent */
};

typedef uint16_t xwpos_t;

struct out_conduit {
    zhe_address_t addr;           /* destination address */
    seq_t    seq;                 /* next seq to send */
    seq_t    seqbase;             /* latest seq ack'd + UNIT = first available */
    seq_t    useq;                /* next unreliable seq to send */
    xwpos_t  pos;                 /* next byte goes into rbuf[pos] */
    xwpos_t  spos;                /* starting pos of current sample for patching in size */
    xwpos_t  firstpos;            /* starting pos (actually, size) of oldest sample in window */
    xwpos_t  xmitw_bytes;         /* size of transmit window pointed to by rbuf */
#if (defined(XMITW_SAMPLES) && XMITW_SAMPLES > 0) || (defined(XMITW_SAMPLES_UNICAST) && XMITW_SAMPLES_UNICAST > 0)
    uint16_t xmitw_samples;       /* size of transmit window in samples */
#endif
    zhe_time_t tsynch;            /* next time to send out a SYNCH because of unack'd messages */
    cid_t    cid;                 /* conduit id */
    zhe_time_t last_rexmit;       /* time of latest retransmit */
    seq_t    last_rexmit_seq;     /* latest sequence number retransmitted */
    uint8_t  draining_window: 1;  /* set to true if draining window (waiting for ACKs) after hitting limit */
    uint8_t  sched_synch: 1;      /* whether a SYNCH must be scheduled (set on transition of empty to non-empty xmit window */
    uint8_t  *rbuf;               /* reliable samples (or declarations); prepended by size (of type zhe_msgsize_t) */
#if XMITW_SAMPLE_INDEX
    seq_t    firstidx;
    xwpos_t *rbufidx;             /* rbuf[rbufidx[seq % xmitw_samples]] is first byte of length of message seq */
#endif
};

struct peer {
    uint8_t state;                /* connection state for this peer */
    zhe_time_t tlease;            /* peer must send something before tlease or we'll close the session | next time for scout/open msg */
    zhe_timediff_t lease_dur;     /* lease duration in ms */
#if HAVE_UNICAST_CONDUIT
    struct out_conduit oc;        /* unicast to this peer */
#else
    struct { zhe_address_t addr; } oc;
#endif
    struct in_conduit ic[N_IN_CONDUITS]; /* one slot for each out conduit from this peer */
    struct peerid id;             /* peer id */
#if N_OUT_MCONDUITS > 0
    DECL_BITSET(mc_member, N_OUT_MCONDUITS);
#endif
};

#if N_OUT_MCONDUITS > 0
#  if MAX_PEERS == 0
#    error "N_OUT_CONDUITS > 1 requires MAX_PEERS > 0"
#  endif
MAKE_PACKAGE_SPEC(BINHEAP, (static, zhe_minseqheap, seq_t, peeridx_t, peeridx_t, MAX_PEERS), type, init, isempty, insert, raisekey, delete, min)

struct out_mconduit {
    struct out_conduit oc;        /* same transmit window management as unicast */
    zhe_minseqheap_t seqbase;     /* tracks ACKs from peers for computing oc.seqbase as min of them all */
};

static struct out_mconduit out_mconduits[N_OUT_MCONDUITS];
static uint8_t out_mconduits_oc_rbuf[N_OUT_MCONDUITS][XMITW_BYTES];
#if XMITW_SAMPLE_INDEX
static xwpos_t out_mconduits_oc_rbufidx[N_OUT_MCONDUITS][XMITW_SAMPLES];
#endif
#endif

#if N_OUT_MCONDUITS == 0
#  define DO_FOR_UNICAST_OR_MULTICAST(cid_, unicast_, multicast_) do { \
          unicast_; \
      } while (0)
#elif ! HAVE_UNICAST_CONDUIT
#  define DO_FOR_UNICAST_OR_MULTICAST(cid_, unicast_, multicast_) do { \
          multicast_; \
      } while (0)
#else
#  define DO_FOR_UNICAST_OR_MULTICAST(cid_, unicast_, multicast_) do { \
          if (cid_ == UNICAST_CID) { \
              unicast_; \
          } else { \
              multicast_; \
          } \
      } while (0)
#endif

unsigned zhe_trace_cats;
struct zhe_platform *zhe_platform;

/* we send SCOUT messages to a separately configurable address (not so much because it really seems
   necessary to have a separate address for scouting, as that we need a statically available address
   to use for the destination of the outgoing packet) */
static zhe_address_t scoutaddr;

#if MAX_MULTICAST_GROUPS > 0
static uint16_t n_multicast_locators;
static zhe_address_t multicast_locators[MAX_MULTICAST_GROUPS];
#endif

/* output buffer is a single packet; a single packet has a single destination and carries reliable data for at most one conduit */
static uint8_t outbuf[TRANSPORT_MTU]; /* where we buffer next outgoing packet */
static zhe_msgsize_t outp;             /* current position in outbuf */
#define OUTSPOS_UNSET ((zhe_msgsize_t) -1)
static zhe_msgsize_t outspos;          /* OUTSPOS_UNSET or pos of last reliable SData/Declare header (OUTSPOS_UNSET <=> outc == NULL) */
static struct out_conduit *outc;  /* conduit over which reliable messages are carried in this packet, or NULL */
static zhe_address_t *outdst;    /* destination address: &scoutaddr, &peer.oc.addr, &out_mconduits[cid].addr */
#if LATENCY_BUDGET != 0 && LATENCY_BUDGET != LATENCY_BUDGET_INF
static zhe_time_t outdeadline;       /* pack until destination change, packet full, or this time passed */
#endif

/* In client mode, we pretend the broker is peer 0 (and the only peer at that). It isn't really a peer,
   but the data structures we need are identical, only the discovery behaviour and (perhaps) session
   handling is a bit different. */
static peeridx_t npeers;
static struct peer peers[MAX_PEERS_1];
#if HAVE_UNICAST_CONDUIT
static uint8_t peers_oc_rbuf[MAX_PEERS_1][XMITW_BYTES_UNICAST];
#if XMITW_SAMPLE_INDEX
static xwpos_t peers_oc_rbufidx[MAX_PEERS_1][XMITW_SAMPLES_UNICAST];
#endif
#endif

/* In peer mode, always send scouts periodically, with tlastscout giving the time of the last scout
   message to go out. In client mode, scouting is conditional upon the state of the broker, in that
   case scouts only go out if peers[0].state = UNKNOWN, but we then overload tlastscout to determine
   when to send a KEEPALIVE. And for that, we simply update tlastscout every time a packet goes out
   when in client mode. */
#if SCOUT_COUNT > 0
#if SCOUT_COUNT <= 255
static uint8_t scout_count = SCOUT_COUNT;
#elif SCOUT_COUNT <= 65535
static uint16_t scout_count = SCOUT_COUNT;
#endif
#endif /* SCOUT_COUNT > 0 */
static zhe_time_t tlastscout;

static void remove_acked_messages(struct out_conduit * const c, seq_t seq);

#if N_OUT_MCONDUITS > 0
MAKE_PACKAGE_BODY(BINHEAP, (static, zhe_minseqheap, seq_t, peeridx_t, PEERIDX_INVALID, peeridx_t, zhe_seq_lt, MAX_PEERS), init, heapify, check, insert, delete, raisekey, min, isempty)
#endif

static void oc_reset_transmit_window(struct out_conduit * const oc)
{
    oc->seqbase = oc->seq;
    oc->firstpos = oc->spos;
    oc->draining_window = 0;
}

static void oc_setup1(struct out_conduit * const oc, cid_t cid, xwpos_t xmitw_bytes, uint8_t *rbuf, uint16_t xmitw_samples, xwpos_t *rbufidx)
{
    memset(&oc->addr, 0, sizeof(oc->addr));
    oc->cid = cid;
    oc->seq = 0;
    oc->useq = 0;
    oc->pos = sizeof(zhe_msgsize_t);
    oc->spos = 0;
    oc->xmitw_bytes = xmitw_bytes;
#if (defined(XMITW_SAMPLES) && XMITW_SAMPLES > 0) || (defined(XMITW_SAMPLES_UNICAST) && XMITW_SAMPLES_UNICAST > 0)
    oc->xmitw_samples = xmitw_samples;
#endif
    oc->rbuf = rbuf;
#if XMITW_SAMPLE_INDEX
    oc->firstidx = 0;
    oc->rbufidx = rbufidx;
#endif
    oc_reset_transmit_window(oc);
}

static void reset_outbuf(void)
{
    outspos = OUTSPOS_UNSET;
    outp = 0;
    outc = NULL;
    outdst = NULL;
}

bool zhe_established_peer(peeridx_t peeridx)
{
    return peers[peeridx].state == PEERST_ESTABLISHED;
}

int zhe_compare_peer_ids_for_peeridx(peeridx_t a, peeridx_t b)
{
    /* if a.id is a prefix of b.id, a precedes b */
    zhe_assert(peers[a].state != PEERST_UNKNOWN && peers[b].state != PEERST_UNKNOWN);
    const zhe_paysize_t asz = peers[a].id.len;
    const zhe_paysize_t bsz = peers[b].id.len;
    const int c = memcmp(peers[a].id.id, peers[b].id.id, (asz <= bsz) ? asz : bsz);
    return (c != 0) ? c : (asz == bsz) ? 0 : (asz < bsz) ? -1 : 1;
}

static void reset_peer(peeridx_t peeridx, zhe_time_t tnow)
{
    struct peer * const p = &peers[peeridx];
    if (p->state != PEERST_UNKNOWN) { /* test is just to supress the trace at start-up */
        ZT(PEERDISC, "reset_peer @ %u", peeridx);
    }
    zhe_reset_peer_rsubs(peeridx);
    /* If data destined for this peer, drop it it */
    zhe_reset_peer_unsched_hist_decls(peeridx);
    zhe_reset_peer_declstatus(peeridx);
#if ZHE_MAX_URISPACE > 0
    zhe_uristore_reset_peer(peeridx);
#endif
#if HAVE_UNICAST_CONDUIT
    if (outdst == &p->oc.addr) {
        reset_outbuf();
    }
#endif
#if N_OUT_MCONDUITS > 0
    /* For those multicast conduits where this peer is among the ones that need to ACK,
       update the administration */
    for (cid_t i = 0; i < N_OUT_MCONDUITS; i++) {
        struct out_mconduit * const mc = &out_mconduits[i];
            zhe_assert(zhe_bitset_test(p->mc_member, (unsigned)i));
        if (zhe_minseqheap_delete(&mc->seqbase, peeridx)) {
            if (zhe_minseqheap_isempty(&mc->seqbase)) {
                remove_acked_messages(&mc->oc, mc->oc.seq);
            } else {
                remove_acked_messages(&mc->oc, zhe_minseqheap_min(&mc->seqbase));
            }
        } else {
            zhe_assert(!zhe_bitset_test(p->mc_member, (unsigned)i) || p->state != PEERST_ESTABLISHED);
        }
    }
#endif
    if (p->state == PEERST_ESTABLISHED) {
        npeers--;
    }
#ifndef NDEBUG
    /* State of most fields shouldn't matter if peer state is UNKNOWN, sequence numbers
       and transmit windows in conduits do matter (so we don't need to clear them upon
       accepting the peer) */
    memset(p, 0xee, sizeof(*p));
#endif
    p->state = PEERST_UNKNOWN;
#if HAVE_UNICAST_CONDUIT
#if XMITW_SAMPLE_INDEX
    xwpos_t * const rbufidx = peers_oc_rbufidx[peeridx];
#else
    xwpos_t * const rbufidx = NULL;
#endif
    oc_setup1(&p->oc, UNICAST_CID, XMITW_BYTES_UNICAST, peers_oc_rbuf[peeridx], XMITW_SAMPLES_UNICAST, rbufidx);
#endif
    for (cid_t i = 0; i < N_IN_CONDUITS; i++) {
        p->ic[i].seq = 0;
        p->ic[i].lseqpU = 0;
        p->ic[i].useq = 0;
        p->ic[i].synched = 0;
        p->ic[i].usynched = 0;
    }
#if N_OUT_MCONDUITS > 0
    memset(p->mc_member, 0, sizeof(p->mc_member));
#endif
}

static void init_globals(zhe_time_t tnow)
{
#if N_OUT_MCONDUITS > 0
    /* Need to reset out_mconduits[.].seqbase.ix[i] before reset_peer(i) may be called */
    memset(out_mconduits, 0, sizeof(out_mconduits));
    for (cid_t i = 0; i < N_OUT_MCONDUITS; i++) {
        struct out_mconduit * const mc = &out_mconduits[i];
#if XMITW_SAMPLE_INDEX
        xwpos_t * const rbufidx = out_mconduits_oc_rbufidx[i];
#else
        xwpos_t * const rbufidx = NULL;
#endif
        oc_setup1(&mc->oc, i, XMITW_BYTES, out_mconduits_oc_rbuf[i], XMITW_SAMPLES, rbufidx);
        mc->seqbase.n = 0;
        zhe_minseqheap_init(&mc->seqbase);
    }
#endif
    for (peeridx_t i = 0; i < MAX_PEERS_1; i++) {
        reset_peer(i, tnow);
    }
    npeers = 0;
    reset_outbuf();
#if LATENCY_BUDGET != 0 && LATENCY_BUDGET != LATENCY_BUDGET_INF
    outdeadline = tnow;
#endif
    tlastscout = tnow;
#if ZHE_MAX_URISPACE > 0
    zhe_uristore_init();
#endif
    zhe_pubsub_init();
}

int zhe_seq_lt(seq_t a, seq_t b)
{
    return (sseq_t) (a - b) < 0;
}

int zhe_seq_le(seq_t a, seq_t b)
{
    return (sseq_t) (a - b) <= 0;
}

static xwpos_t xmitw_pos_add(const struct out_conduit *c, xwpos_t p, xwpos_t a)
{
    if ((p += a) >= c->xmitw_bytes) {
        p -= c->xmitw_bytes;
    }
    return p;
}

static zhe_msgsize_t xmitw_load_msgsize(const struct out_conduit *c, xwpos_t p)
{
    zhe_msgsize_t sz;
    if (p < c->xmitw_bytes - sizeof(sz)) {
        memcpy(&sz, &c->rbuf[p], sizeof(sz));
        return sz;
    } else {
        memcpy(&sz, &c->rbuf[p], c->xmitw_bytes - p);
        memcpy((char *)&sz + (c->xmitw_bytes - p), &c->rbuf[0], sizeof(sz) - (c->xmitw_bytes - p));
        return sz;
    }
}

static void xmitw_store_msgsize(const struct out_conduit *c, xwpos_t p, zhe_msgsize_t sz)
{
    if (p < c->xmitw_bytes - sizeof(sz)) {
        memcpy(&c->rbuf[p], &sz, sizeof(sz));
    } else {
        memcpy(&c->rbuf[p], &sz, c->xmitw_bytes - p);
        memcpy(&c->rbuf[0], (char *)&sz + (c->xmitw_bytes - p), sizeof(sz) - (c->xmitw_bytes - p));
    }
}

static xwpos_t zhe_xmitw_bytesavail(const struct out_conduit *c)
{
    xwpos_t res;
    zhe_assert(c->pos < c->xmitw_bytes);
    zhe_assert(c->pos == xmitw_pos_add(c, c->spos, sizeof(zhe_msgsize_t)));
    zhe_assert(c->firstpos < c->xmitw_bytes);
    res = c->firstpos + (c->firstpos < c->pos ? c->xmitw_bytes : 0) - c->pos;
    zhe_assert(res <= c->xmitw_bytes);
    return res;
}

static seq_t oc_get_nsamples(struct out_conduit const * const c)
{
    return (seq_t)(c->seq - c->seqbase) >> SEQNUM_SHIFT;
}

int zhe_xmitw_hasspace(const struct out_conduit *c, zhe_paysize_t sz)
{
#if (defined(XMITW_SAMPLES) && XMITW_SAMPLES > 0) || (defined(XMITW_SAMPLES_UNICAST) && XMITW_SAMPLES_UNICAST > 0)
    if (oc_get_nsamples(c) == c->xmitw_samples) {
        return 0;
    }
#endif
    const zhe_paysize_t av = zhe_xmitw_bytesavail(c);
    return av >= sizeof(zhe_msgsize_t) && av - sizeof(zhe_msgsize_t) >= sz;
}

static xwpos_t xmitw_skip_sample(const struct out_conduit *c, xwpos_t p)
{
    zhe_msgsize_t sz = xmitw_load_msgsize(c, p);
    return xmitw_pos_add(c, p, sizeof(zhe_msgsize_t) + sz);
}

#if XMITW_SAMPLE_INDEX
static xwpos_t xmitw_load_rbufidx(const struct out_conduit *c, seq_t seq)
{
    seq_t off = (seq_t)(seq - c->seqbase) >> SEQNUM_SHIFT;
    seq_t idx = (c->firstidx + off) % c->xmitw_samples;
    return c->rbufidx[idx];
}

static void xmitw_store_rbufidx(const struct out_conduit *c, seq_t seq, xwpos_t p)
{
    seq_t off = (seq_t)(seq - c->seqbase) >> SEQNUM_SHIFT;
    seq_t idx = (c->firstidx + off) % c->xmitw_samples;
    c->rbufidx[idx] = p;
}

#ifndef NDEBUG
static void check_xmitw(const struct out_conduit *c)
{
    zhe_assert(c->pos == xmitw_pos_add(c, c->spos, sizeof(zhe_msgsize_t)));
    if (c->seq == c->seqbase) {
        zhe_assert(c->spos == c->firstpos);
    } else {
        xwpos_t p = c->firstpos;
        seq_t seq = c->seqbase;
        do {
            zhe_assert(p == xmitw_load_rbufidx(c, seq));
            p = xmitw_skip_sample(c, p);
            seq += SEQNUM_UNIT;
        } while (seq != c->seq);
        assert(p == c->spos);
    }
}
#endif
#endif

void zhe_pack_msend(zhe_time_t tnow)
{
#if MSYNCH_INTERVAL < 2 * ROUNDTRIP_TIME_ESTIMATE
#error "zhe_pack_msend assumes MSYNCH_INTERVAL - 2 * ROUNDTRIP_TIME_ESTIMATE is a good indicator for setting the S flag"
#endif
    if (outp > 0) {
        zhe_assert ((outspos == OUTSPOS_UNSET) == (outc == NULL));
        zhe_assert (outdst != NULL);
        if (outspos != OUTSPOS_UNSET) {
            /* FIXME: not-so-great proxy for transition past 3/4 of window size */
            xwpos_t cnt = zhe_xmitw_bytesavail(outc);
            ZT(DEBUG, "msend spos set: cnt=%u (tnow=%u-tsynch=%u)=%u", (unsigned)cnt, (unsigned)tnow, (unsigned)outc->tsynch, (unsigned)(tnow - outc->tsynch));
            if ((cnt < outc->xmitw_bytes / 4 && cnt + outspos >= outc->xmitw_bytes / 4) ||
                ((zhe_timediff_t)(tnow - outc->tsynch) > MSYNCH_INTERVAL - 2 * ROUNDTRIP_TIME_ESTIMATE)) {
                outbuf[outspos] |= MSFLAG;
                outc->sched_synch = 1;
            }
            if (outc->sched_synch) {
                outc->tsynch = tnow;
                outc->sched_synch = 0;
            }
        }
        const int sendres = zhe_platform_send(zhe_platform, outbuf, outp, outdst);
        if (sendres < 0) {
            zhe_assert(0);
        } else {
#if MAX_PEERS == 0
            if (sendres > 0) {
                /* we didn't drop the packet for lack of space, so postpone next keepalive */
                tlastscout = tnow;
            }
#endif
        }
        outp = 0;
        outspos = OUTSPOS_UNSET;
        outc = NULL;
        outdst = NULL;
    }
}

static void pack_check_avail(uint16_t n)
{
    zhe_assert(sizeof (outbuf) - outp >= n);
}

void zhe_pack_reserve(zhe_address_t *dst, struct out_conduit *oc, zhe_paysize_t cnt, zhe_time_t tnow)
{
    /* oc != NULL <=> reserving for reliable data */
    /* make room by sending out current packet if requested number of bytes is no longer
       available, and also send out current packet if the destination changes */
    if (TRANSPORT_MTU - outp < cnt || (outdst != NULL && dst != outdst) || (outc && outc != oc)) {
        /* we should never even try to generate a message that is too large for a packet */
        zhe_assert(outp != 0);
        zhe_pack_msend(tnow);
    }
    if (oc) {
        outc = oc;
    }
    outdst = dst;
#if LATENCY_BUDGET != 0 && LATENCY_BUDGET != LATENCY_BUDGET_INF
    if (outp == 0) {
        /* packing deadline: note that no incomplete messages will ever be in the buffer when
           we check, because it is single-threaded and we always complete whatever message we
           start constructing */
        outdeadline = tnow + LATENCY_BUDGET;
        ZT(DEBUG, "deadline at %"PRIu32".%0"PRIu32, ZTIME_TO_SECu32(outdeadline), ZTIME_TO_MSECu32(outdeadline));
    }
#endif
}

void zhe_pack1(uint8_t x)
{
    pack_check_avail(1);
    outbuf[outp++] = x;
}

void zhe_pack2(uint8_t x, uint8_t y)
{
    pack_check_avail(2);
    outbuf[outp++] = x;
    outbuf[outp++] = y;
}

void zhe_pack_vec(zhe_paysize_t n, const void *vbuf)
{
    const uint8_t *buf = vbuf;
    zhe_pack_vle16(n);
    pack_check_avail(n);
    while (n--) {
        outbuf[outp++] = *buf++;
    }
}

uint16_t zhe_pack_locs_calcsize(void)
{
#if MAX_MULTICAST_GROUPS > 0
    size_t n = zhe_pack_vle16req(n_multicast_locators);
    char tmp[TRANSPORT_ADDRSTRLEN];
    for (uint16_t i = 0; i < n_multicast_locators; i++) {
        size_t n1 = zhe_platform_addr2string(zhe_platform, tmp, sizeof(tmp), &multicast_locators[i]);
        zhe_assert(n1 < UINT16_MAX);
        n += zhe_pack_vle16req((uint16_t)n1) + n1;
    }
    zhe_assert(n < UINT16_MAX);
    return (uint16_t)n;
#else
    return 1;
#endif
}

void zhe_pack_locs(void)
{
#if MAX_MULTICAST_GROUPS > 0
    zhe_pack_vle16(n_multicast_locators);
    for (uint16_t i = 0; i < n_multicast_locators; i++) {
        char tmp[TRANSPORT_ADDRSTRLEN];
        uint16_t n1 = (uint16_t)zhe_platform_addr2string(zhe_platform, tmp, sizeof(tmp), &multicast_locators[i]);
        zhe_pack_vec(n1, tmp);
    }
#else
    zhe_pack_vle16(0);
#endif
}

cid_t zhe_oc_get_cid(struct out_conduit *c)
{
    return c->cid;
}

void zhe_oc_hit_full_window(struct out_conduit *c, zhe_time_t tnow)
{
    c->draining_window = 1;
    if (outp > 0) {
        zhe_pack_msynch(outdst, MSFLAG, c->cid, c->seqbase, oc_get_nsamples(c), tnow);
        zhe_pack_msend(tnow);
    }
}

int zhe_oc_am_draining_window(const struct out_conduit *c)
{
    return c->draining_window;
}

#if N_OUT_MCONDUITS > 0
int zhe_ocm_have_peers(const struct out_mconduit *mc)
{
    return !zhe_minseqheap_isempty(&mc->seqbase);
}
#endif

void zhe_oc_pack_copyrel(struct out_conduit *c, zhe_msgsize_t from)
{
    /* only for non-empty sequence of initial bytes of message (i.e., starts with header */
    zhe_assert(c->pos == xmitw_pos_add(c, c->spos, sizeof(zhe_msgsize_t)));
    zhe_assert(from < outp);
    zhe_assert(!(outbuf[from] & MSFLAG));
    while (from < outp) {
        zhe_assert(c->pos != c->firstpos || c->seq == c->seqbase);
        c->rbuf[c->pos] = outbuf[from++];
        c->pos = xmitw_pos_add(c, c->pos, 1);
    }
}

zhe_msgsize_t zhe_oc_pack_payload_msgprep(seq_t *s, struct out_conduit *c, int relflag, zhe_paysize_t sz, zhe_time_t tnow)
{
    zhe_assert(c->pos == xmitw_pos_add(c, c->spos, sizeof(zhe_msgsize_t)));
    if (!relflag) {
        zhe_pack_reserve_mconduit(&c->addr, NULL, c->cid, sz, tnow);
        *s = c->useq;
    } else {
        zhe_pack_reserve_mconduit(&c->addr, c, c->cid, sz, tnow);
        *s = c->seq;
        outspos = outp;
    }
    return outp;
}

void zhe_oc_pack_payload(struct out_conduit *c, int relflag, zhe_paysize_t sz, const void *vdata)
{
    /* c->spos points to size byte, header byte immediately follows it, so reliability flag is
     easily located in the buffer */
    const uint8_t *data = (const uint8_t *)vdata;
    while (sz--) {
        outbuf[outp++] = *data;
        if (relflag) {
            zhe_assert(c->pos != c->firstpos);
            c->rbuf[c->pos] = *data;
            c->pos = xmitw_pos_add(c, c->pos, 1);
        }
        data++;
    }
}

void zhe_oc_pack_payload_done(struct out_conduit *c, int relflag, zhe_time_t tnow)
{
    if (!relflag) {
        c->useq += SEQNUM_UNIT;
    } else {
        zhe_msgsize_t len = (zhe_msgsize_t) (c->pos - c->spos + (c->pos < c->spos ? c->xmitw_bytes : 0) - sizeof(zhe_msgsize_t));
        xmitw_store_msgsize(c, c->spos, len);
#if XMITW_SAMPLE_INDEX
        xmitw_store_rbufidx(c, c->seq, c->spos);
#endif
        c->spos = c->pos;
        c->pos = xmitw_pos_add(c, c->pos, sizeof(zhe_msgsize_t));
        if (c->seq == c->seqbase) {
            /* first unack'd sample, schedule SYNCH */
            c->sched_synch = 1;
        }
        /* prep for next sample */
        c->seq += SEQNUM_UNIT;
    }
}

struct out_conduit *zhe_out_conduit_from_cid(peeridx_t peeridx, cid_t cid)
{
    struct out_conduit *c;
    DO_FOR_UNICAST_OR_MULTICAST(cid, c = &peers[peeridx].oc, c = &out_mconduits[cid].oc);
    return c;
}

bool zhe_out_conduit_is_connected(peeridx_t peeridx, cid_t cid)
{
    bool c;
    DO_FOR_UNICAST_OR_MULTICAST(cid, c = (peers[peeridx].state == PEERST_ESTABLISHED), c = zhe_ocm_have_peers(&out_mconduits[cid]));
    return c;
}

///////////////////////////////////////////////////////////////////////////////////////////

/* What the combination of (*interpret and return value) results in:
 *               DIM_INTERPRET     DIM_IGNORE     DIM_ABORT
 * ZUR_OK        C&ack             ignore         abort-txn
 * ZUR_SHORT     abort-txn,X       X              abort-txn,X
 * ZUR_OVERFLOW  abort-txn,close   close          abort-txn,close
 * where
 * - X = close if packet mode, try again when more data arrives if stream mode
 *   interpretation starts in {INTERPRET,IGNORE} and where INTERPRET may transition to
 * - ABORT - so IGNORE need not do "abort-txn"
 * - C = commit & send a SUCCESS result if the transaction contents are acceptable, or
 *   abort & send an error code if the transaction contents are unacceptable. */

enum declaration_interpretation_mode {
    DIM_INTERPRET,
    DIM_IGNORE,
    DIM_ABORT
};

#if ENABLE_TRACING
static const char *decl_intp_mode_str(enum declaration_interpretation_mode m)
{
    const char *res = "?";
    switch (m) {
        case DIM_INTERPRET: res = "interpret"; break;
        case DIM_IGNORE: res = "ignore"; break;
        case DIM_ABORT: res = "abort"; break;
    }
    return res;
}
#endif

static zhe_unpack_result_t handle_dresource(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, enum declaration_interpretation_mode *interpret, bool tentative)
{
    zhe_unpack_result_t res;
    uint8_t hdr;
    zhe_rid_t rid;
    zhe_paysize_t urisize;
    const uint8_t *uri;
    struct unpack_props_iter it;
    if ((res = zhe_unpack_byte(end, data, &hdr)) != ZUR_OK ||
        (res = zhe_unpack_rid(end, data, &rid)) != ZUR_OK ||
        (res = zhe_unpack_vecref(end, data, &urisize, &uri)) != ZUR_OK) {
        return res;
    }
    if ((hdr & DPFLAG) && (res = zhe_unpack_props(end, data, &it)) != ZUR_OK) {
        return res;
    }
#if ZHE_MAX_URISPACE > 0 /* FIXME: I think I should reject non-wildcard URIs, rather than ignore them */
    if (*interpret == DIM_INTERPRET) {
        zhe_residx_t idx;
        if (zhe_rid_in_use_anonymously(rid)) {
            zhe_decl_note_error_curpkt(16, rid);
            return ZUR_OK;
        } else {
            /* FIXME: when the C flag is set, there's no point in making it tentative (failure would force a disconnect anyway), while it does increases the risk of a failed transaction */
            peeridx_t loser;
            switch (zhe_uristore_store(&idx, peeridx, rid, uri, urisize, tentative, &loser)) {
                case USR_OK:
                    if (!tentative) {
#if ZHE_MAX_URISPACE > 0 && MAX_PEERS > 0
                        zhe_update_subs_for_resource_decl(rid);
#endif
                    } else if (loser == peeridx) {
                        zhe_decl_note_error_curpkt(ZHE_DECL_AGAIN, rid);
                        *interpret = DIM_ABORT;
                    } else if (loser != PEERIDX_INVALID) {
                        zhe_decl_note_error_somepeer(loser, ZHE_DECL_AGAIN, rid);
                    }
                    return ZUR_OK;
                case USR_DUPLICATE:
                    return ZUR_OK;
                case USR_AGAIN:
                    /* pretend we never received this declare message - trying again on retransmit (this one should clear quickly because it only involves local GC) */
                    *interpret = DIM_ABORT;
                    return ZUR_OK;
                case USR_NOSPACE:
                    zhe_decl_note_error_curpkt(ZHE_DECL_NOSPACE, rid);
                    return ZUR_OK;
                case USR_MISMATCH:
                    zhe_decl_note_error_curpkt(ZHE_DECL_CONFLICT, rid);
                    return ZUR_OK;
                case USR_INVALID:
                    zhe_decl_note_error_curpkt(ZHE_DECL_INVALID, rid);
                    return ZUR_OK;
            }
        }
    }
#endif
    return ZUR_OK;
}

static zhe_unpack_result_t handle_dpub(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, enum declaration_interpretation_mode *interpret)
{
    zhe_unpack_result_t res;
    uint8_t hdr;
    struct unpack_props_iter it;
    if ((res = zhe_unpack_byte(end, data, &hdr)) != ZUR_OK ||
        (res = zhe_unpack_rid(end, data, NULL)) != ZUR_OK) {
        return res;
    }
    if ((hdr & DPFLAG) && (res = zhe_unpack_props(end, data, &it)) != ZUR_OK) {
        return res;
    }
    return ZUR_OK;
}

static zhe_unpack_result_t handle_dsub(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, enum declaration_interpretation_mode *interpret, bool tentative)
{
    zhe_unpack_result_t res;
    zhe_rid_t rid;
    uint8_t hdr, mode;
    struct unpack_props_iter it;
    if ((res = zhe_unpack_byte(end, data, &hdr)) != ZUR_OK ||
        (res = zhe_unpack_rid(end, data, &rid)) != ZUR_OK ||
        (res = zhe_unpack_byte(end, data, &mode)) != ZUR_OK) {
        return res;
    }
    if (mode == 0 || mode > SUBMODE_MAX) {
        return ZUR_OVERFLOW;
    }
    if (mode == SUBMODE_PERIODPULL || mode == SUBMODE_PERIODPUSH) {
        if ((res = zhe_unpack_vle32(end, data, NULL)) != ZUR_OK /* temporal origin */ ||
            (res = zhe_unpack_vle32(end, data, NULL)) != ZUR_OK /* period */ ||
            (res = zhe_unpack_vle32(end, data, NULL)) != ZUR_OK /* duration */) {
            return res;
        }
    }
    if ((hdr & DPFLAG) && (res = zhe_unpack_props(end, data, &it)) != ZUR_OK) {
        return res;
    }
    if (*interpret == DIM_INTERPRET) {
        zhe_rsub_register(peeridx, rid, mode, tentative);
    }
    return ZUR_OK;
}

static zhe_unpack_result_t handle_dselection(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, enum declaration_interpretation_mode *interpret)
{
    zhe_unpack_result_t res;
    zhe_rid_t sid;
    uint8_t hdr;
    zhe_paysize_t dummy;
    struct unpack_props_iter it;
    if ((res = zhe_unpack_byte(end, data, &hdr)) != ZUR_OK ||
        (res = zhe_unpack_rid(end, data, &sid)) != ZUR_OK ||
        (res = zhe_unpack_vec(end, data, 0, &dummy, NULL)) != ZUR_OK) {
        return res;
    }
    if ((hdr & DPFLAG) && (res = zhe_unpack_props(end, data, &it)) != ZUR_OK) {
        return res;
    }
    if (*interpret == DIM_INTERPRET) {
        zhe_decl_note_error_curpkt(ZHE_DECL_UNSUPPORTED, sid);
    }
    return ZUR_OK;
}

static zhe_unpack_result_t handle_dbindid(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, enum declaration_interpretation_mode *interpret)
{
    zhe_unpack_result_t res;
    zhe_rid_t sid;
    if ((res = zhe_unpack_skip(end, data, 1)) != ZUR_OK ||
        (res = zhe_unpack_rid(end, data, &sid)) != ZUR_OK ||
        (res = zhe_unpack_rid(end, data, NULL)) != ZUR_OK) {
        return res;
    }
    if (*interpret == DIM_INTERPRET) {
        zhe_decl_note_error_curpkt(ZHE_DECL_UNSUPPORTED, sid);
    }
    return ZUR_OK;
}

static zhe_unpack_result_t handle_dcommit(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, enum declaration_interpretation_mode *interpret, zhe_time_t tnow)
{
    zhe_unpack_result_t res;
    uint8_t commitid;
    zhe_rid_t err_rid;
    if ((res = zhe_unpack_skip(end, data, 1)) != ZUR_OK ||
        (res = zhe_unpack_byte(end, data, &commitid)) != ZUR_OK) {
        return res;
    }
    if (*interpret == DIM_INTERPRET) {
#if HAVE_UNICAST_CONDUIT
        struct out_conduit * const oc = zhe_out_conduit_from_cid(peeridx, UNICAST_CID);
#else
        struct out_conduit * const oc = zhe_out_conduit_from_cid(peeridx, 0);
#endif
        zhe_msgsize_t from;
        uint8_t commitres;
        /* Use worst-case size for result */
        if (!zhe_oc_pack_mdeclare(oc, false, 1, WC_DRESULT_SIZE, &from, tnow)) {
            /* If we can't reserve space in the transmit window, pretend we never received the
               DECLARE message: eventually we'll get a retransmit and retry. */
            *interpret = DIM_ABORT;
            return ZUR_OK;
        } else {
            zhe_rsub_precommit_curpkt_done(peeridx);
            if ((commitres = zhe_rsub_precommit(peeridx, &err_rid)) == 0) {
                zhe_rsub_commit(peeridx);
#if ZHE_MAX_URISPACE > 0
                zhe_uristore_commit_tentative(peeridx);
#endif
            } else {
#if ZHE_MAX_URISPACE > 0
                zhe_uristore_abort_tentative(peeridx);
#endif
            }
            zhe_pack_dresult(commitid, commitres, err_rid);
            zhe_oc_pack_mdeclare_done(oc, from, tnow);
            zhe_pack_msend(tnow);
        }
    }
    return ZUR_OK;
}

static zhe_unpack_result_t handle_dresult(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, enum declaration_interpretation_mode *interpret)
{
    zhe_unpack_result_t res;
    uint8_t commitid, status;
    zhe_rid_t rid = 0;
    if ((res = zhe_unpack_skip(end, data, 1)) != ZUR_OK ||
        (res = zhe_unpack_byte(end, data, &commitid)) != ZUR_OK ||
        (res = zhe_unpack_byte(end, data, &status)) != ZUR_OK) {
        return res;
    }
    if (status && (res = zhe_unpack_rid(end, data, &rid)) != ZUR_OK) {
        return res;
    }
    ZT(PUBSUB, "handle_dresult %u intp %s | commitid %u status %u rid %ju", (unsigned)peeridx, decl_intp_mode_str(*interpret), commitid, status, (uintmax_t)rid);
    if (*interpret == DIM_INTERPRET && status != 0) {
        zhe_note_declstatus(peeridx, status, rid);
    }
    return ZUR_OK;
}

static zhe_unpack_result_t handle_dfresource(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, enum declaration_interpretation_mode *interpret)
{
    zhe_unpack_result_t res;
    if ((res = zhe_unpack_skip(end, data, 1)) != ZUR_OK ||
        (res = zhe_unpack_rid(end, data, NULL)) != ZUR_OK) {
        return res;
    }
    return ZUR_OK;
}

static zhe_unpack_result_t handle_dfpub(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, enum declaration_interpretation_mode *interpret)
{
    zhe_unpack_result_t res;
    if ((res = zhe_unpack_skip(end, data, 1)) != ZUR_OK ||
        (res = zhe_unpack_rid(end, data, NULL)) != ZUR_OK) {
        return res;
    }
    return ZUR_OK;
}

static zhe_unpack_result_t handle_dfsub(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, enum declaration_interpretation_mode *interpret)
{
    zhe_unpack_result_t res;
    zhe_rid_t rid;
    if ((res = zhe_unpack_skip(end, data, 1)) != ZUR_OK ||
        (res = zhe_unpack_rid(end, data, &rid)) != ZUR_OK) {
        return res;
    }
    return ZUR_OK;
}

static zhe_unpack_result_t handle_dfselection(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, enum declaration_interpretation_mode *interpret)
{
    zhe_unpack_result_t res;
    zhe_rid_t sid;
    if ((res = zhe_unpack_skip(end, data, 1)) != ZUR_OK ||
        (res = zhe_unpack_rid(end, data, &sid))) {
        return res;
    }
    return ZUR_OK;
}


///////////////////////////////////////////////////////////////////////////////////////////

static zhe_unpack_result_t handle_mscout(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, zhe_time_t tnow)
{
#if MAX_PEERS > 0
    const uint32_t lookfor = MSCOUT_PEER;
    const int state_ok = (peers[peeridx].state == PEERST_UNKNOWN);
#else
    const uint32_t lookfor = MSCOUT_CLIENT;
    const int state_ok = 1;
#endif
    zhe_unpack_result_t res;
    uint8_t hdr;
    uint32_t mask;
    struct unpack_props_iter it;
    if ((res = zhe_unpack_byte(end, data, &hdr)) != ZUR_OK ||
        (res = zhe_unpack_vle32(end, data, &mask)) != ZUR_OK) {
        return res;
    }
    if ((hdr & MPFLAG) && (res = zhe_unpack_props(end, data, &it)) != ZUR_OK) {
        return res;
    }
    /* For a client all activity is really client-initiated, so we can get away
       with not responding to a SCOUT; for a peer it is different */
    if ((mask & lookfor) && state_ok) {
        ZT(PEERDISC, "got a scout! sending a hello");
        zhe_pack_mhello(&peers[peeridx].oc.addr, tnow);
        zhe_pack_msend(tnow);
    }
    return ZUR_OK;
}

static int set_peer_mcast_locs(peeridx_t peeridx, struct unpack_locs_iter *it)
{
    zhe_paysize_t sz;
    const uint8_t *loc;
    while (zhe_unpack_locs_iter(it, &sz, &loc)) {
#if N_OUT_MCONDUITS > 0
        for (cid_t cid = 0; cid < N_OUT_MCONDUITS; cid++) {
            char tmp[TRANSPORT_ADDRSTRLEN];
            const size_t tmpsz = zhe_platform_addr2string(zhe_platform, tmp, sizeof(tmp), &out_mconduits[cid].oc.addr);
            if (sz == tmpsz && memcmp(loc, tmp, sz) == 0) {
                zhe_bitset_set(peers[peeridx].mc_member, (unsigned)cid);
                ZT(PEERDISC, "loc %s cid %u", tmp, (unsigned)cid);
            }
        }
#endif
    }
    return 1;
}

static zhe_unpack_result_t handle_mhello(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, zhe_time_t tnow)
{
#if MAX_PEERS > 0
    const uint32_t lookfor = MSCOUT_PEER | MSCOUT_BROKER;
    const int state_ok = (peers[peeridx].state == PEERST_UNKNOWN || peers[peeridx].state == PEERST_ESTABLISHED);
#else
    const uint32_t lookfor = MSCOUT_BROKER;
    const int state_ok = (peers[peeridx].state == PEERST_UNKNOWN);
#endif
    zhe_unpack_result_t res;
    struct unpack_locs_iter locs_it;
    struct unpack_props_iter props_it;
    uint8_t hdr;
    uint32_t mask;
    if ((res = zhe_unpack_byte(end, data, &hdr)) != ZUR_OK ||
        (res = zhe_unpack_vle32(end, data, &mask)) != ZUR_OK ||
        (res = zhe_unpack_locs(end, data, &locs_it)) != ZUR_OK) {
        return res;
    }
    if ((hdr & MPFLAG) && (res = zhe_unpack_props(end, data, &props_it)) != ZUR_OK) {
        return res;
    }
    if ((mask & lookfor) && state_ok) {
        int send_open = 1;
        ZT(PEERDISC, "got a hello! sending an open");
        if (peers[peeridx].state != PEERST_ESTABLISHED) {
            if (!set_peer_mcast_locs(peeridx, &locs_it)) {
                ZT(PEERDISC, "'twas but a hello with an invalid locator list ...");
                send_open = 0;
            } else {
                peers[peeridx].state = PEERST_OPENING_MIN;
                peers[peeridx].tlease = tnow;
            }
        } else {
            /* FIXME: a hello when established indicates a reconnect for the other one => should at least clear ic[.].synched, usynched - but maybe more if we want some kind of notification of the event ... */
            for (cid_t cid = 0; cid < N_IN_CONDUITS; cid++) {
                peers[peeridx].ic[cid].synched = 0;
                peers[peeridx].ic[cid].usynched = 0;
            }
        }
        if (send_open) {
            zhe_pack_mopen(&peers[peeridx].oc.addr, SEQNUM_LEN, &ownid, LEASE_DURATION, tnow);
            zhe_pack_msend(tnow);
        }
    }
    return ZUR_OK;
}

static peeridx_t find_peeridx_by_id(peeridx_t peeridx, zhe_paysize_t idlen, const uint8_t * restrict id)
{
    /* keepalive, open, accept and close contain a peer id, and can be used to switch source address;
       peeridx on input is the peeridx determined using the source address, on return it will be the
       peeridx of the known peer with this id (if any) and otherwise just the same idx */

    if (peers[peeridx].state == PEERST_ESTABLISHED) {
        /* assume there is no foul play and this is the same peer */
        return peeridx;
    }

    for (peeridx_t i = 0; i < MAX_PEERS_1; i++) {
        if (peers[i].state != PEERST_ESTABLISHED) {
            continue;
        }
        if (peers[i].id.len == idlen && memcmp(peers[i].id.id, id, idlen) == 0) {
#if ENABLE_TRACING
            if (ZTT(PEERDISC)) {
                char olda[TRANSPORT_ADDRSTRLEN], newa[TRANSPORT_ADDRSTRLEN];
                zhe_platform_addr2string(zhe_platform, olda, sizeof(olda), &peers[i].oc.addr);
                zhe_platform_addr2string(zhe_platform, newa, sizeof(newa), &peers[peeridx].oc.addr);
                ZT(PEERDISC, "peer %u changed address from %s to %s", (unsigned)i, olda, newa);
            }
#endif
            peers[i].oc.addr = peers[peeridx].oc.addr;
            return i;
        }
    }
    return peeridx;
}

#if ENABLE_TRACING
static char tohexdigit(uint8_t x)
{
    zhe_assert(x <= 15);
    return (x <= 9) ? (char)('0' + x) : (char)('a' + (x - 10));
}
#endif

static void accept_peer(peeridx_t peeridx, zhe_paysize_t idlen, const uint8_t * restrict id, zhe_timediff_t lease_dur, zhe_time_t tnow)
{
    struct peer * const p = &peers[peeridx];
    zhe_assert(p->state != PEERST_ESTABLISHED);
    zhe_assert(idlen > 0);
    zhe_assert(idlen <= PEERID_SIZE);
    zhe_assert(lease_dur >= 0);

#if ENABLE_TRACING
    if (ZTT(PEERDISC)) {
        char astr[TRANSPORT_ADDRSTRLEN];
        char idstr[3*PEERID_SIZE], *idstrp = idstr;
        zhe_platform_addr2string(zhe_platform, astr, sizeof(astr), &p->oc.addr);
        for (int i = 0; i < idlen; i++) {
            if (i > 0) {
                *idstrp++ = ':';
            }
            *idstrp++ = tohexdigit(id[i] >> 4);
            *idstrp++ = tohexdigit(id[i] & 0xf);
            zhe_assert(idstrp < idstr + sizeof(idstr));
        }
        *idstrp = 0;
        ZT(PEERDISC, "accept peer %s %s @ %u; lease = %" PRId32, idstr, astr, peeridx, (int32_t)lease_dur);
    }
#endif

    p->state = PEERST_ESTABLISHED;
    p->id.len = idlen;
    memcpy(p->id.id, id, idlen);
    p->lease_dur = lease_dur;
    p->tlease = tnow + (zhe_time_t)p->lease_dur;
#if N_OUT_MCONDUITS > 0
    for (cid_t cid = 0; cid < N_OUT_MCONDUITS; cid++) {
        if (zhe_bitset_test(p->mc_member, (unsigned)cid)) {
            struct out_mconduit * const mc = &out_mconduits[cid];
            zhe_minseqheap_insert(&mc->seqbase, peeridx, mc->oc.seq);
        }
    }
#endif
    npeers++;

#if MAX_PEERS == 0
    for (cid_t cid = 0; cid < N_IN_CONDUITS; cid++) {
        peers[peeridx].ic[cid].synched = 1;
        peers[peeridx].ic[cid].usynched = 1;
    }
#endif

    zhe_accept_peer_sched_hist_decls(peeridx);
}

static int conv_lease_to_ztimediff(zhe_timediff_t *res, uint32_t ld100)
{
    if (ld100 > ZHE_TIMEDIFF_MAX / (100000000 / ZHE_TIMEBASE)) {
        return 0;
    }
    *res = (100000000 / ZHE_TIMEBASE) * (zhe_timediff_t)ld100;
    return 1;
}

static zhe_unpack_result_t handle_mopen(peeridx_t * restrict peeridx, const uint8_t * const end, const uint8_t **data, zhe_time_t tnow)
{
    zhe_unpack_result_t res;
    uint8_t hdr, version;
    zhe_paysize_t idlen;
    uint8_t id[PEERID_SIZE];
    uint8_t reason;
    uint32_t ld100;
    zhe_timediff_t ld;
    struct unpack_locs_iter locs_it;
    struct unpack_props_iter props_it;
    struct peer *p;
    if ((res = zhe_unpack_byte(end, data, &hdr)) != ZUR_OK ||
        (res = zhe_unpack_byte(end, data, &version)) != ZUR_OK /* version */ ||
        (res = zhe_unpack_vec(end, data, sizeof(id), &idlen, id)) != ZUR_OK /* peer id */ ||
        (res = zhe_unpack_vle32(end, data, &ld100)) != ZUR_OK /* lease duration */ ||
        (res = zhe_unpack_locs(end, data, &locs_it)) != ZUR_OK) {
        return res;
    }
    if ((hdr & MPFLAG) && (res = zhe_unpack_props(end, data, &props_it)) != ZUR_OK) {
        return res;
    } else {
        uint8_t propid;
        zhe_paysize_t propsz;
        const uint8_t *prop;
        while (zhe_unpack_props_iter(&props_it, &propid, &propsz, &prop)) {
            if (propid == PROP_SEQLEN) {
                uint8_t seqlen;
                res = zhe_unpack_vle8(prop + propsz, &prop, &seqlen);
                switch (res) {
                    case ZUR_OK:
                        if (seqlen != SEQNUM_LEN) {
                            ZT(PEERDISC, "got an open with an unsupported sequence number size (%hhu)", seqlen);
                            reason = CLR_UNSUPP_SEQLEN;
                            goto reject;
                        }
                        break;
                    case ZUR_OVERFLOW:
                        ZT(PEERDISC, "got an open with an unsupported sequence number size (> 255)");
                        reason = CLR_UNSUPP_SEQLEN;
                        goto reject;
                    case ZUR_SHORT:
                    case ZUR_ABORT:
                        ZT(PEERDISC, "got an open with an invalid sequence number property");
                        reason = CLR_ERROR;
                        goto reject;
                }
            }
        }
    }
    if (version != ZHE_VERSION) {
        ZT(PEERDISC, "got an open with an unsupported version (%hhu)", version);
        reason = CLR_UNSUPP_PROTO;
        goto reject;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        ZT(PEERDISC, "got an open with an under- or oversized id (%hu)", idlen);
        reason = CLR_ERROR;
        goto reject;
    }
    if (!conv_lease_to_ztimediff(&ld, ld100)) {
        ZT(PEERDISC, "got an open with a lease duration that is not representable here");
        reason = CLR_ERROR;
        goto reject;
    }
    if (idlen == ownid.len && memcmp(ownid.id, id, idlen) == 0) {
        ZT(PEERDISC, "got an open with my own peer id");
        goto reject_no_close;
    }

    *peeridx = find_peeridx_by_id(*peeridx, idlen, id);

    p = &peers[*peeridx];
    if (p->state != PEERST_ESTABLISHED) {
        if (!set_peer_mcast_locs(*peeridx, &locs_it)) {
            ZT(PEERDISC, "'twas but an open with an invalid locator list ...");
            reason = CLR_ERROR;
            goto reject;
        }
        accept_peer(*peeridx, idlen, id, ld, tnow);
    }
    zhe_pack_maccept(&p->oc.addr, &ownid, &p->id, LEASE_DURATION, tnow);
    zhe_pack_msend(tnow);

    return ZUR_OK;

reject:
    zhe_pack_mclose(&peers[*peeridx].oc.addr, reason, &ownid, tnow);
    /* don't want anything to do with the other anymore; calling reset on one that is already in UNKNOWN is harmless */
    reset_peer(*peeridx, tnow);
    /* no point in interpreting following messages in packet */
reject_no_close:
    return ZUR_ABORT;
}

static zhe_unpack_result_t handle_maccept(peeridx_t * restrict peeridx, const uint8_t * const end, const uint8_t **data, zhe_time_t tnow)
{
    zhe_unpack_result_t res;
    zhe_paysize_t idlen;
    uint8_t id[PEERID_SIZE];
    uint32_t ld100;
    uint8_t hdr;
    zhe_timediff_t ld;
    struct unpack_props_iter props_it;
    int forme;
    if ((res = zhe_unpack_byte(end, data, &hdr)) != ZUR_OK ||
        (res = zhe_unpack_vec(end, data, sizeof(id), &idlen, id)) != ZUR_OK) {
        return res;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        ZT(PEERDISC, "got an open with an under- or oversized id (%hu)", idlen);
        goto reject_no_close;
    }
    forme = (idlen == ownid.len && memcmp(id, ownid.id, idlen) == 0);
    if ((res = zhe_unpack_vec(end, data, sizeof (id), &idlen, id)) != ZUR_OK ||
        (res = zhe_unpack_vle32(end, data, &ld100)) != ZUR_OK) {
        return res;
    }
    if ((hdr & MPFLAG) && (res = zhe_unpack_props(end, data, &props_it)) != ZUR_OK) {
        return res;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        ZT(PEERDISC, "got an open with an under- or oversized id (%hu)", idlen);
        goto reject_no_close;
    }
    if (forme) {
        if (!conv_lease_to_ztimediff(&ld, ld100)) {
            ZT(PEERDISC, "got an open with a lease duration that is not representable here");
            goto reject;
        }
        *peeridx = find_peeridx_by_id(*peeridx, idlen, id);
        if (peers[*peeridx].state >= PEERST_OPENING_MIN && peers[*peeridx].state <= PEERST_OPENING_MAX) {
            accept_peer(*peeridx, idlen, id, ld, tnow);
        }
    }
    return ZUR_OK;

reject:
    zhe_pack_mclose(&peers[*peeridx].oc.addr, CLR_ERROR, &ownid, tnow);
    /* don't want anything to do with the other anymore; calling reset on one that is already in UNKNOWN is harmless */
    reset_peer(*peeridx, tnow);
    /* no point in interpreting following messages in packet */
reject_no_close:
    return ZUR_ABORT;
}

static zhe_unpack_result_t handle_mclose(peeridx_t * restrict peeridx, const uint8_t * const end, const uint8_t **data, zhe_time_t tnow)
{
    zhe_unpack_result_t res;
    zhe_paysize_t idlen;
    uint8_t id[PEERID_SIZE];
    uint8_t reason;
    if ((res = zhe_unpack_skip(end, data, 1)) != ZUR_OK ||
        (res = zhe_unpack_vec(end, data, sizeof(id), &idlen, id)) != ZUR_OK ||
        (res = zhe_unpack_byte(end, data, &reason)) != ZUR_OK) {
        return res;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        ZT(PEERDISC, "got a close with an under- or oversized id (%hu)", idlen);
        reset_peer(*peeridx, tnow);
        return ZUR_OK;
    }
    *peeridx = find_peeridx_by_id(*peeridx, idlen, id);
    if (peers[*peeridx].state != PEERST_UNKNOWN) {
        reset_peer(*peeridx, tnow);
    }
    return ZUR_OK;
}

static int ic_may_deliver_seq(const struct in_conduit *ic, uint8_t hdr, seq_t seq)
{
    if (hdr & MRFLAG) {
        return (ic->seq == seq);
    } else if (ic->usynched) {
        return zhe_seq_le(ic->useq, seq);
    } else {
        return 1;
    }
}

static void ic_update_seq (struct in_conduit *ic, uint8_t hdr, seq_t seq)
{
    zhe_assert(ic_may_deliver_seq(ic, hdr, seq));
    if (hdr & MRFLAG) {
        zhe_assert(zhe_seq_lt(ic->seq, ic->lseqpU));
        ic->seq = seq + SEQNUM_UNIT;
    } else {
        zhe_assert(zhe_seq_le(ic->seq, ic->lseqpU));
        ic->useq = seq + SEQNUM_UNIT;
        ic->usynched = 1;
    }
}

static void acknack_if_needed(peeridx_t peeridx, cid_t cid, int wantsack, zhe_time_t tnow)
{
    seq_t cnt = (peers[peeridx].ic[cid].lseqpU - peers[peeridx].ic[cid].seq) >> SEQNUM_SHIFT;
    uint32_t mask;
    zhe_assert(zhe_seq_le(peers[peeridx].ic[cid].seq, peers[peeridx].ic[cid].lseqpU));
    if (cnt == 0) {
        mask = 0;
    } else {
        mask = ~(uint32_t)0;
        if (cnt < 32) { /* avoid undefined behaviour */
            mask >>= 32 - cnt;
        }
    }
    if (wantsack || (mask != 0 && (zhe_timediff_t)(tnow - peers[peeridx].ic[cid].tack) > ROUNDTRIP_TIME_ESTIMATE)) {
        /* ACK goes out over unicast path; the conduit used for sending it doesn't have
           much to do with it other than administrative stuff */
        ZT(RELIABLE, "acknack_if_needed peeridx %u cid %u wantsack %d mask %u seq %u", peeridx, cid, wantsack, mask, peers[peeridx].ic[cid].seq >> SEQNUM_SHIFT);
        zhe_pack_macknack(&peers[peeridx].oc.addr, cid, peers[peeridx].ic[cid].seq, mask, tnow);
        zhe_pack_msend(tnow);
        peers[peeridx].ic[cid].tack = tnow;
    }
}

static zhe_unpack_result_t handle_mdeclare(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, cid_t cid, zhe_time_t tnow)
{
    /* Note 1: not buffering data received out-of-order, so but need to decode everything to
       find next message, which we may "have to" interpret - we don't really "have to", but to
       simply pretend we never received it is a bit rough.

       Note 2: a commit requires us to send something in reply, but we may be unable to because
       of a full transmit window in the reliable channel.  The elegant option is to suspend
       further input processing, until space is available again, the inelegant one is to verify
       we have space beforehand, and pretend we never received the DECLARE if we don't. */
    zhe_unpack_result_t res;
    uint8_t hdr;
    uint16_t ndecls;
    seq_t seq;
    enum declaration_interpretation_mode intp;
    if ((res = zhe_unpack_byte(end, data, &hdr)) != ZUR_OK ||
        (res = zhe_unpack_seq(end, data, &seq)) != ZUR_OK ||
        (res = zhe_unpack_vle16(end, data, &ndecls)) != ZUR_OK) {
        return res;
    }
    if (!(peers[peeridx].state == PEERST_ESTABLISHED && peers[peeridx].ic[cid].synched)) {
        intp = DIM_IGNORE;
    } else {
        if (zhe_seq_le(peers[peeridx].ic[cid].seq, seq + SEQNUM_UNIT) && zhe_seq_lt(peers[peeridx].ic[cid].lseqpU, seq + SEQNUM_UNIT)) {
            peers[peeridx].ic[cid].lseqpU = seq + SEQNUM_UNIT;
        }
        intp = ic_may_deliver_seq(&peers[peeridx].ic[cid], MRFLAG, seq) ? DIM_INTERPRET : DIM_IGNORE;
    }
    ZT(PUBSUB, "handle_mdeclare %p seq %u peeridx %u ndecls %u intp %s", data, seq, peeridx, ndecls, decl_intp_mode_str(intp));
    while (ndecls > 0 && *data < end && res == ZUR_OK) {
        switch (**data & DKIND) {
            case DRESOURCE:   res = handle_dresource(peeridx, end, data, &intp, !(hdr & MCFLAG)); break;
            case DPUB:        res = handle_dpub(peeridx, end, data, &intp); break;
            case DSUB:        res = handle_dsub(peeridx, end, data, &intp, !(hdr & MCFLAG)); break;
            case DSELECTION:  res = handle_dselection(peeridx, end, data, &intp); break;
            case DBINDID:     res = handle_dbindid(peeridx, end, data, &intp); break;
            case DCOMMIT:     res = handle_dcommit(peeridx, end, data, &intp, tnow); break;
            case DRESULT:     res = handle_dresult(peeridx, end, data, &intp); break;
            case DFRESOURCE:  res = handle_dfresource(peeridx, end, data, &intp); break;
            case DFPUB:       res = handle_dfpub(peeridx, end, data, &intp); break;
            case DFSUB:       res = handle_dfsub(peeridx, end, data, &intp); break;
            case DFSELECTION: res = handle_dfselection(peeridx, end, data, &intp); break;
            default:          res = ZUR_OVERFLOW; break;
        }
        if (res == ZUR_OK) {
            ndecls--;
        }
    }
    if (res == ZUR_OK && ndecls != 0) {
        res = ZUR_SHORT;
    }
    if (res != ZUR_OK) {
        intp = DIM_ABORT;
    }
    switch (intp) {
        case DIM_IGNORE:
            break;
        case DIM_ABORT:
            ZT(PUBSUB, "handle_mdeclare %u .. abort res = %d", peeridx, (int)res);
#if ZHE_MAX_URISPACE > 0
            zhe_uristore_abort_tentative(peeridx);
#endif
            zhe_rsub_precommit_curpkt_abort(peeridx);
            break;
        case DIM_INTERPRET:
            /* Merge uncommitted declaration state resulting from this DECLARE message into
               uncommitted state accumulator, as we have now completely and successfully processed
               this message.  */
            ZT(PUBSUB, "handle_mdeclare %u .. packet done", peeridx);
            zhe_rsub_precommit_curpkt_done(peeridx);
            (void)ic_update_seq(&peers[peeridx].ic[cid], MRFLAG, seq);
            /* If C flag set, commit, closing the connection if an error is encountered */
            if (hdr & MCFLAG) {
                uint8_t commitres;
                zhe_rid_t err_rid;
                ZT(PUBSUB, "handle_mdeclare %u .. C flag set", peeridx);
                if ((commitres = zhe_rsub_precommit_status_for_Cflag(peeridx, &err_rid)) != 0) {
                    ZT(PUBSUB, "handle_mdeclare %u .. failures noted, close", peeridx);
                    zhe_pack_mclose(&peers[peeridx].oc.addr, CLR_INCOMPAT_DECL, &ownid, tnow);
                    zhe_pack_msend(tnow);
                    reset_peer(peeridx, tnow);
                }
            }
            break;
    }
    if (peers[peeridx].state == PEERST_ESTABLISHED && peers[peeridx].ic[cid].synched) {
        acknack_if_needed(peeridx, cid, hdr & MSFLAG, tnow);
    }
    return res;
}

static zhe_unpack_result_t handle_msynch(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, cid_t cid, zhe_time_t tnow)
{
    zhe_unpack_result_t res;
    uint8_t hdr;
    seq_t cnt_shifted;
    seq_t seq_msg;
    if ((res = zhe_unpack_byte(end, data, &hdr)) != ZUR_OK ||
        (res = zhe_unpack_seq(end, data, &seq_msg)) != ZUR_OK) {
        return res;
    }
    if (!(hdr & MUFLAG)) {
        cnt_shifted = 0;
    } else if ((res = zhe_unpack_seq(end, data, &cnt_shifted)) != ZUR_OK) {
        return res;
    }
    if (peers[peeridx].state == PEERST_ESTABLISHED) {
        seq_t seqbase = seq_msg - cnt_shifted;
        ZT(RELIABLE, "handle_msynch peeridx %u cid %u seqbase %u cnt %u", peeridx, cid, seqbase >> SEQNUM_SHIFT, cnt_shifted >> SEQNUM_SHIFT);
        if (!peers[peeridx].ic[cid].synched) {
            ZT(PEERDISC, "handle_msynch peeridx %u cid %u seqbase %u cnt %u", peeridx, cid, seqbase >> SEQNUM_SHIFT, cnt_shifted >> SEQNUM_SHIFT);
            peers[peeridx].ic[cid].seq = seqbase;
            peers[peeridx].ic[cid].lseqpU = seq_msg;
            peers[peeridx].ic[cid].synched = 1;
        } else if (zhe_seq_le(peers[peeridx].ic[cid].seq, seqbase) || zhe_seq_lt(seq_msg, peers[peeridx].ic[cid].seq)) {
            ZT(RELIABLE, "handle_msynch peeridx %u cid %u seqbase %u cnt %u", peeridx, cid, seqbase >> SEQNUM_SHIFT, cnt_shifted >> SEQNUM_SHIFT);
            peers[peeridx].ic[cid].seq = seqbase;
            peers[peeridx].ic[cid].lseqpU = seq_msg;
        }
        acknack_if_needed(peeridx, cid, hdr & MSFLAG, tnow);
    }
    return ZUR_OK;
}

unsigned zhe_delivered, zhe_discarded;

static zhe_unpack_result_t handle_msdata(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, cid_t cid, zhe_time_t tnow)
{
    zhe_unpack_result_t res;
    uint8_t hdr;
    zhe_paysize_t paysz;
    const uint8_t *pay;
    seq_t seq;
    zhe_rid_t rid, prid;
    if ((res = zhe_unpack_byte(end, data, &hdr)) != ZUR_OK ||
        (res = zhe_unpack_seq(end, data, &seq)) != ZUR_OK ||
        (res = zhe_unpack_rid(end, data, &rid)) != ZUR_OK) {
        return res;
    }
    if (!(hdr & MAFLAG)) {
        prid = rid;
    } else if ((res = zhe_unpack_rid(end, data, &prid)) != ZUR_OK) {
        return res;
    }
    if ((res = zhe_unpack_vecref(end, data, &paysz, &pay)) != ZUR_OK) {
        return res;
    }

    if (peers[peeridx].state != PEERST_ESTABLISHED) {
        /* Not accepting data from peers that we haven't (yet) established a connection with */
        return ZUR_OK;
    }

    if (!(hdr & MRFLAG)) {
        if (ic_may_deliver_seq(&peers[peeridx].ic[cid], hdr, seq)) {
            (void)zhe_handle_msdata_deliver(prid, paysz, pay);
            ic_update_seq(&peers[peeridx].ic[cid], hdr, seq);
        }
    } else if (peers[peeridx].ic[cid].synched) {
        /* Only move lseqpU forward based on the received sequence number if the seq is greater than the next-to-be-delivered and greater than the latest known, or else we can end up with ic[cid].lseqpU < ic[cid].seq */
        if (zhe_seq_le(peers[peeridx].ic[cid].seq, seq + SEQNUM_UNIT) && zhe_seq_lt(peers[peeridx].ic[cid].lseqpU, seq + SEQNUM_UNIT)) {
            peers[peeridx].ic[cid].lseqpU = seq + SEQNUM_UNIT;
        }
        if (ic_may_deliver_seq(&peers[peeridx].ic[cid], hdr, seq)) {
            ZT(RELIABLE, "handle_msdata peeridx %u cid %u seq %u deliver", peeridx, cid, seq >> SEQNUM_SHIFT);
            if (zhe_handle_msdata_deliver(prid, paysz, pay)) {
                /* if failed to deliver, we must retry, which necessitates a retransmit and not updating the conduit state */
                ic_update_seq(&peers[peeridx].ic[cid], hdr, seq);
            }
            zhe_delivered++;
        } else {
            ZT(RELIABLE, "handle_msdata peeridx %u cid %u seq %u != %u", peeridx, cid, seq >> SEQNUM_SHIFT, peers[peeridx].ic[cid].seq >> SEQNUM_SHIFT);
            zhe_discarded++;
        }
        acknack_if_needed(peeridx, cid, hdr & MSFLAG, tnow);
    }

    return ZUR_OK;
}

static zhe_unpack_result_t handle_mwdata(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, cid_t cid, zhe_time_t tnow)
{
    zhe_unpack_result_t res;
    uint8_t hdr;
    zhe_paysize_t paysz;
    const uint8_t *pay = *data;
    seq_t seq;
    zhe_paysize_t urisz;
    const uint8_t *uri;
    if ((res = zhe_unpack_byte(end, data, &hdr)) != ZUR_OK ||
        (res = zhe_unpack_seq(end, data, &seq)) != ZUR_OK ||
        (res = zhe_unpack_vecref(end, data, &urisz, &uri)) != ZUR_OK ||
        (res = zhe_unpack_vecref(end, data, &paysz, &pay)) != ZUR_OK) {
        return res;
    }

    if (peers[peeridx].state != PEERST_ESTABLISHED) {
        /* Not accepting data from peers that we haven't (yet) established a connection with */
        return ZUR_OK;
    }

    if (!(hdr & MRFLAG)) {
        if (ic_may_deliver_seq(&peers[peeridx].ic[cid], hdr, seq)) {
#if ZHE_MAX_URISPACE > 0
            if (!zhe_urivalid(uri, urisz)) {
                goto err_invalid_uri;
            }
            (void)zhe_handle_mwdata_deliver(urisz, uri, paysz, pay);
#endif
            ic_update_seq(&peers[peeridx].ic[cid], hdr, seq);
        }
    } else if (peers[peeridx].ic[cid].synched) {
        /* Only move lseqpU forward based on the received sequence number if the seq is greater than the next-to-be-delivered and greater than the latest known, or else we can end up with ic[cid].lseqpU < ic[cid].seq */
        if (zhe_seq_le(peers[peeridx].ic[cid].seq, seq + SEQNUM_UNIT) && zhe_seq_lt(peers[peeridx].ic[cid].lseqpU, seq + SEQNUM_UNIT)) {
            peers[peeridx].ic[cid].lseqpU = seq + SEQNUM_UNIT;
        }
        if (ic_may_deliver_seq(&peers[peeridx].ic[cid], hdr, seq)) {
#if ZHE_MAX_URISPACE > 0
            ZT(RELIABLE, "handle_mwdata peeridx %u cid %u seq %u deliver", peeridx, cid, seq >> SEQNUM_SHIFT);
            if (!zhe_urivalid(uri, urisz)) {
                goto err_invalid_uri;
            }
            if (zhe_handle_mwdata_deliver(urisz, uri, paysz, pay)) {
                /* if failed to deliver, we must retry, which necessitates a retransmit and not updating the conduit state */
                ic_update_seq(&peers[peeridx].ic[cid], hdr, seq);
            }
#else
            ic_update_seq(&peers[peeridx].ic[cid], hdr, seq);
#endif
            zhe_delivered++;
        } else {
            ZT(RELIABLE, "handle_mwdata peeridx %u cid %u seq %u != %u", peeridx, cid, seq >> SEQNUM_SHIFT, peers[peeridx].ic[cid].seq >> SEQNUM_SHIFT);
            zhe_discarded++;
        }
        acknack_if_needed(peeridx, cid, hdr & MSFLAG, tnow);
    }
    return ZUR_OK;

#if ZHE_MAX_URISPACE > 0
err_invalid_uri:
    return ZUR_OVERFLOW;
#endif
}

#if ! XMITW_SAMPLE_INDEX
static xwpos_t xmitw_skip_to_seq(const struct out_conduit *c, xwpos_t p, seq_t s, seq_t end)
{
    while (zhe_seq_lt(s, end)) {
        s += SEQNUM_UNIT;
        p = xmitw_skip_sample(c, p);
    }
    return p;
}
#endif

static void remove_acked_messages(struct out_conduit * restrict c, seq_t seq)
{
    ZT(RELIABLE, "remove_acked_messages cid %u %p seq %u", c->cid, (void*)c, seq >> SEQNUM_SHIFT);

#if !defined(NDEBUG) && XMITW_SAMPLE_INDEX
    check_xmitw(c);
#endif

    if (zhe_seq_lt(c->seq, seq)) {
        /* Broker is ACKing samples we haven't even sent yet, use the opportunity to drain the
           transmit window */
        seq = c->seq;
    }

    if(zhe_seq_lt(c->seqbase, seq)) {
        /* Acking some samples, drop everything from seqbase up to but not including seq */
#if XMITW_SAMPLE_INDEX
        seq_t cnt = (seq_t)(seq - c->seqbase) >> SEQNUM_SHIFT;
        c->firstpos = (seq == c->seq) ? c->spos : xmitw_load_rbufidx(c, seq);
        c->firstidx = (c->firstidx + cnt) % c->xmitw_samples;
        c->seqbase = seq;
#else
        c->firstpos = xmitw_skip_to_seq(c, c->firstpos, c->seqbase, seq);
        c->seqbase = seq;
#endif
        zhe_assert(((c->firstpos + sizeof(zhe_msgsize_t)) % c->xmitw_bytes == c->pos) == (c->seq == c->seqbase));
    }

    if (oc_get_nsamples(c) == 0) {
        c->draining_window = 0;
    }
}

static zhe_unpack_result_t handle_macknack(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, cid_t cid, zhe_time_t tnow)
{
    struct out_conduit * const c = zhe_out_conduit_from_cid(peeridx, cid);
    zhe_unpack_result_t res;
    seq_t seq, seq_ack;
    uint8_t hdr;
    uint32_t mask;
    if ((res = zhe_unpack_byte(end, data, &hdr)) != ZUR_OK ||
        (res = zhe_unpack_seq(end, data, &seq)) != ZUR_OK) {
        return res;
    }
    if (!(hdr & MMFLAG)) {
        mask = 0;
    } else if ((res = zhe_unpack_vle32(end, data, &mask)) != ZUR_OK && res != ZUR_OVERFLOW) {
        /* Mask must be a valid VLE number, but it need not fit in 32 bits -- so we ignore an overflow */
        return res;
    } else {
        /* Make the retransmit request for message SEQ implied by the use of an ACKNACK
         explicit in the mask (which means we won't retransmit SEQ + 32). */
        mask = (mask << 1) | 1;
    }
    if (peers[peeridx].state != PEERST_ESTABLISHED) {
        return ZUR_OK;
    }

    if (zhe_seq_lt(seq, c->seqbase) || zhe_seq_lt(c->seq, seq)) {
        /* If a peer ACKs messages we have dropped already, or if it NACKs ones we have not
           even sent yet, send a SYNCH and but otherwise ignore the ACKNACK */
        ZT(RELIABLE, "handle_macknack peeridx %u cid %u %p seq %u mask %08x - [%u,%u] - send synch", peeridx, cid, (void*)c, seq >> SEQNUM_SHIFT, mask, c->seqbase >> SEQNUM_SHIFT, (c->seq >> SEQNUM_SHIFT)-1);
        zhe_pack_msynch(&c->addr, 0, c->cid, c->seqbase, oc_get_nsamples(c), tnow);
        zhe_pack_msend(tnow);
        return ZUR_OK;
    }

    DO_FOR_UNICAST_OR_MULTICAST(cid, seq_ack = seq, seq_ack = zhe_minseqheap_raisekey(&out_mconduits[cid].seqbase, peeridx, seq, c->seqbase));
    remove_acked_messages(c, seq_ack);

    if (mask == 0) {
        /* Pure ACK - no need to do anything else */
        if (seq != c->seq) {
            ZT(RELIABLE, "handle_macknack peeridx %u cid %u seq %u ACK but we have [%u,%u]", peeridx, cid, seq >> SEQNUM_SHIFT, c->seqbase >> SEQNUM_SHIFT, (c->seq >> SEQNUM_SHIFT)-1);
        } else {
            ZT(RELIABLE, "handle_macknack peeridx %u cid %u seq %u ACK", peeridx, cid, seq >> SEQNUM_SHIFT);
        }
    } else if ((zhe_timediff_t)(tnow - c->last_rexmit) <= ROUNDTRIP_TIME_ESTIMATE && zhe_seq_lt(seq, c->last_rexmit_seq)) {
        ZT(RELIABLE, "handle_macknack peeridx %u cid %u seq %u mask %08x - suppress", peeridx, cid, seq >> SEQNUM_SHIFT, mask);
    } else {
        /* Retransmits can always be performed because they do not require buffering new
           messages, all we need to do is push out the buffered messages.  We want the S bit
           set on the last of the retransmitted ones, so we "clear" outspos and then set it
           before pushing out that last sample. */
        xwpos_t p;
        zhe_msgsize_t sz, outspos_tmp = OUTSPOS_UNSET;
        ZT(RELIABLE, "handle_macknack peeridx %u cid %u seq %u mask %08x", peeridx, cid, seq >> SEQNUM_SHIFT, mask);
        /* Do not set the S bit on anything that happens to currently be in the output buffer,
           if that is of the same conduit as the one we are retransmitting on, as we by now know
           that we will retransmit at least one message and therefore will send a message with
           the S flag set and will schedule a SYNCH anyway */
        if (outc == c) {
            outspos = OUTSPOS_UNSET;
            outc = NULL;
        }
        /* Note: transmit window is formatted as SZ1 [MSG2 x SZ1] SZ2 [MSG2 x SZ2], &c,
           wrapping around at c->xmit_bytes.  */
#if XMITW_SAMPLE_INDEX
        p = xmitw_load_rbufidx(c, seq);
#else
        p = xmitw_skip_to_seq(c, c->firstpos, c->seqbase, seq);
#endif
        while (mask && zhe_seq_lt(seq, c->seq)) {
            if ((mask & 1) == 0) {
                p = xmitw_skip_sample(c, p);
            } else {
                /* Out conduit is NULL so that the invariant that (outspos == OUTSPOS_UNSET) <=>
                   (outc == NULL) is maintained, and also in consideration of the fact that keeping
                   track of the conduit and the position of the last reliable message is solely
                   for the purpose of setting the S flag and scheduling SYNCH messages.  Retransmits
                   are require none of that beyond what we do here locally anyway. */
                ZT(RELIABLE, "handle_macknack   rx %u", seq >> SEQNUM_SHIFT);
                sz = xmitw_load_msgsize(c, p);
                p = xmitw_pos_add(c, p, sizeof(zhe_msgsize_t));
                zhe_pack_reserve_mconduit(&c->addr, NULL, cid, sz, tnow);
                outspos_tmp = outp;
                while (sz--) {
                    zhe_pack1(c->rbuf[p]);
                    p = xmitw_pos_add(c, p, 1);
                }
            }
            mask >>= 1;
            seq += SEQNUM_UNIT;
        }
        c->last_rexmit = tnow;
        c->last_rexmit_seq = seq;
        /* If we sent at least one message, outspos_tmp has been set. Set the S flag in that final
           message. Also make sure we send a SYNCH not too long after (and so do all that pack_msend
           would otherwise have done for c). */
        if(outspos_tmp != OUTSPOS_UNSET) {
            /* Note: setting the S bit is not the same as a SYNCH, maybe it would be better to send
             a SYNCH instead? */
            outbuf[outspos_tmp] |= MSFLAG;
            zhe_pack_msend(tnow);
        }
    }
    return ZUR_OK;
}

static zhe_unpack_result_t handle_mping(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, zhe_time_t tnow)
{
    zhe_unpack_result_t res;
    uint16_t hash;
    if ((res = zhe_unpack_skip(end, data, 1)) != ZUR_OK ||
        (res = zhe_unpack_vle16(end, data, &hash)) != ZUR_OK) {
        return res == ZUR_OVERFLOW ? ZUR_OK : res;
    }
    zhe_pack_mpong(&peers[peeridx].oc.addr, hash, tnow);
    zhe_pack_msend(tnow);
    return ZUR_OK;
}

static zhe_unpack_result_t handle_mpong(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data)
{
    zhe_unpack_result_t res;
    if ((res = zhe_unpack_skip(end, data, 1)) != ZUR_OK ||
        (res = zhe_unpack_vle16(end, data, NULL)) != ZUR_OK) {
        return res == ZUR_OVERFLOW ? ZUR_OK : res;
    }
    return ZUR_OK;
}

static zhe_unpack_result_t handle_mkeepalive(peeridx_t * restrict peeridx, const uint8_t * const end, const uint8_t **data, zhe_time_t tnow)
{
    zhe_unpack_result_t res;
    zhe_paysize_t idlen;
    uint8_t id[PEERID_SIZE];
    if ((res = zhe_unpack_skip(end, data, 1)) != ZUR_OK ||
        (res = zhe_unpack_vec(end, data, sizeof(id), &idlen, id)) != ZUR_OK) {
        return res;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        reset_peer(*peeridx, tnow);
        return ZUR_OVERFLOW;
    }
    (void)find_peeridx_by_id(*peeridx, idlen, id);
    return ZUR_OK;
}

static zhe_unpack_result_t handle_mconduit(peeridx_t peeridx, const uint8_t * const end, const uint8_t **data, cid_t * restrict cid, zhe_time_t tnow)
{
    zhe_unpack_result_t res;
    uint8_t hdr, cid_byte;
    if ((res = zhe_unpack_byte(end, data, &hdr)) != ZUR_OK) {
        return res;
    } else if (hdr & MZFLAG) {
        *cid = 1 + ((hdr >> 5) & 0x3);
    } else if ((res = zhe_unpack_vle8(end, data, &cid_byte)) != ZUR_OK) {
        return res;
    } else if (cid_byte >= N_IN_CONDUITS) {
        reset_peer(peeridx, tnow);
        return ZUR_OVERFLOW;
    } else {
        *cid = (cid_t)cid_byte;
    }
    return ZUR_OK;
}

static zhe_unpack_result_t handle_packet(peeridx_t * restrict peeridx, const uint8_t * const end, const uint8_t **data, zhe_time_t tnow)
{
    zhe_unpack_result_t res;
    const uint8_t *data1 = *data;
    cid_t cid = 0;
    do {
        ZT(DEBUG, "handle_packet: kind = %u", (unsigned)(*data1 & MKIND));
        switch (*data1 & MKIND) {
            case MSCOUT:     res = handle_mscout(*peeridx, end, &data1, tnow); break;
            case MHELLO:     res = handle_mhello(*peeridx, end, &data1, tnow); break;
            case MOPEN:      res = handle_mopen(peeridx, end, &data1, tnow); break;
            case MACCEPT:    res = handle_maccept(peeridx, end, &data1, tnow); break;
            case MCLOSE:     res = handle_mclose(peeridx, end, &data1, tnow); break;
            case MDECLARE:   res = handle_mdeclare(*peeridx, end, &data1, cid, tnow); break;
            case MSDATA:     res = handle_msdata(*peeridx, end, &data1, cid, tnow); break;
            case MWDATA:     res = handle_mwdata(*peeridx, end, &data1, cid, tnow); break;
            case MPING:      res = handle_mping(*peeridx, end, &data1, tnow); break;
            case MPONG:      res = handle_mpong(*peeridx, end, &data1); break;
            case MSYNCH:     res = handle_msynch(*peeridx, end, &data1, cid, tnow); break;
            case MACKNACK:   res = handle_macknack(*peeridx, end, &data1, cid, tnow); break;
            case MKEEPALIVE: res = handle_mkeepalive(peeridx, end, &data1, tnow); break;
            case MCONDUIT:   res = handle_mconduit(*peeridx, end, &data1, &cid, tnow); break;
            default:         res = ZUR_OVERFLOW; break;
        }
        if (res == ZUR_OK) {
            *data = data1;
        }
    } while (data1 < end && res == ZUR_OK);
    return res;
}

int zhe_init(const struct zhe_config *config, struct zhe_platform *pf, zhe_time_t tnow)
{
    /* Is there a way to make the transport pluggable at run-time without dynamic allocation? I don't think so, not with the MTU so important ... */
    if (config->idlen == 0 || config->idlen > PEERID_SIZE) {
        return -1;
    }
    if (config->n_mconduit_dstaddrs > N_OUT_MCONDUITS) {
        return -1;
    }
    if (config->n_mcgroups_join > MAX_MULTICAST_GROUPS) {
        /* but you don't have to join MAX groups */
        return -1;
    }

    ownid_union.v_nonconst.len = (zhe_paysize_t)config->idlen;
    memcpy(ownid_union.v_nonconst.id, config->id, config->idlen);

    init_globals(tnow);
    zhe_platform = pf;
    scoutaddr = *config->scoutaddr;
    /* For multicast receive locators and multicast destination addresses: allow setting fewer than we support (for out conduits, that simply means, no peer will ever match the unused ones, making them effectively unused and the result is simply a waste of memory and a bit of CPU time). Secondly, if either is set to 0 then treat it as a synonym for using the scouting address (this simplifies life a little bit for a minimal test program). When it is not zero, don't do this. */
#if MAX_MULTICAST_GROUPS > 0
    n_multicast_locators = (uint16_t)config->n_mcgroups_join;
    if (n_multicast_locators > 0) {
        for (size_t i = 0; i < config->n_mcgroups_join; i++) {
            multicast_locators[i] = config->mcgroups_join[i];
        }
    } else {
        multicast_locators[0] = scoutaddr;
        n_multicast_locators = 1;
    }
#endif
#if N_OUT_MCONDUITS > 0
    if (config->n_mconduit_dstaddrs > 0) {
        for (cid_t i = 0; i < config->n_mconduit_dstaddrs; i++) {
            out_mconduits[i].oc.addr = config->mconduit_dstaddrs[i];
        }
    } else {
        out_mconduits[0].oc.addr = scoutaddr;
    }
#endif
    return 0;
}

void zhe_start(zhe_time_t tnow)
{
    tlastscout = tnow;
}

#if TRANSPORT_MODE == TRANSPORT_PACKET
int zhe_input(const void * restrict buf, size_t sz, const struct zhe_address *src, zhe_time_t tnow)
{
#if ENABLE_TRACING
    char addrstr[TRANSPORT_ADDRSTRLEN];
#endif
    peeridx_t peeridx, free_peeridx = PEERIDX_INVALID;

    for (peeridx = 0; peeridx < MAX_PEERS_1; peeridx++) {
        if (zhe_platform_addr_eq(src, &peers[peeridx].oc.addr)) {
            break;
        } else if (peers[peeridx].state == PEERST_UNKNOWN && free_peeridx == PEERIDX_INVALID) {
            free_peeridx = peeridx;
        }
    }

#if ENABLE_TRACING
    if (ZTT(DEBUG)) {
        (void)zhe_platform_addr2string(zhe_platform, addrstr, sizeof(addrstr), src);
    }
#endif

    if (peeridx == MAX_PEERS_1 && free_peeridx != PEERIDX_INVALID) {
        ZT(DEBUG, "possible new peer %s @ %u", addrstr, free_peeridx);
        peeridx = free_peeridx;
        peers[peeridx].oc.addr = *src;
    }

    if (peeridx < MAX_PEERS_1) {
        zhe_unpack_result_t res;
        const uint8_t *bufp = buf;
        ZT(DEBUG, "handle message from %s @ %u", addrstr, peeridx);
        if (peers[peeridx].state == PEERST_ESTABLISHED) {
            peers[peeridx].tlease = tnow;
        }
        res = handle_packet(&peeridx, buf + sz, &bufp, tnow);
        switch (res)
        {
            case ZUR_OK:
                break;
            case ZUR_SHORT:
            case ZUR_OVERFLOW:
            case ZUR_ABORT:
                reset_peer(peeridx, tnow);
                break;
        }
        return (int)(bufp - (const uint8_t *)buf);
    } else {
        ZT(DEBUG, "message from %s dropped: no available peeridx", addrstr);
        return 0;
    }
}
#elif TRANSPORT_MODE == TRANSPORT_STREAM
#if MAX_PEERS != 0
#  error "stream currently only implemented for client mode"
#endif
int zhe_input(const void * restrict buf, size_t sz, const struct zhe_address *src, zhe_time_t tnow)
{
    if (sz == 0) {
        return 0;
    } else {
        zhe_unpack_result_t res;
        const uint8_t *bufp = buf;
        peeridx_t peeridx = 0;
        res = handle_packet(&peeridx, buf + sz, &bufp, tnow);
        if (bufp > (const uint8_t *)buf && peers[0].state == PEERST_ESTABLISHED) {
            /* any complete message is considered proof of liveliness of the broker once a connection has been established */
            peers[0].tlease = tnow;
        }
        switch (res)
        {
            case ZUR_OK:
            case ZUR_SHORT:
                break;
            case ZUR_OVERFLOW:
            case ZUR_ABORT:
                reset_peer(peeridx, tnow);
                break;
        }
        return (int)(bufp - (const uint8_t *)buf);
    }
}
#endif

#if MAX_PEERS == 0
static void send_scout(zhe_time_t tnow)
{
    if (peers[0].state == PEERST_UNKNOWN) {
        zhe_pack_mscout(&scoutaddr, tnow);
    } else {
#if LEASE_DURATION > 0
        zhe_pack_mkeepalive(&scoutaddr, &ownid, tnow);
#endif /* LEASE_DURATION > 0 */
    }
    zhe_pack_msend(tnow);
}
#elif SCOUT_COUNT == 0 /* && MAX_PEERS > 0 */
static void send_scout(zhe_time_t tnow)
{
    zhe_pack_mscout(&scoutaddr, tnow);
    if (zhe_platform_needs_keepalive(zhe_platform)) {
        /* Scout messages will take care of keeping alive the peer, but there is also the issue of potentially changing source
         addresses ... so we should send a keepalive every now and then if we know some peers */
        zhe_pack_mkeepalive(&scoutaddr, &ownid, tnow);
    }
    zhe_pack_msend(tnow);
}
#else /* SCOUT_COUNT > 0 && MAX_PEERS > 0 */
static void send_scout(zhe_time_t tnow)
{
    if (scout_count > 0) {
        --scout_count;
        zhe_pack_mscout(&scoutaddr, tnow);
    }
#if LEASE_DURATION > 0
    if (npeers > 0 && (scout_count == 0 || zhe_platform_needs_keepalive(zhe_platform))) {
        zhe_pack_mkeepalive(&scoutaddr, &ownid, tnow);
    }
#endif /* LEASE_DURATION */
    zhe_pack_msend(tnow);
}
#endif /* MAX_PEERS */

static void maybe_send_msync_oc(struct out_conduit * const oc, zhe_time_t tnow)
{
    if (oc->seq != oc->seqbase && (zhe_timediff_t)(tnow - oc->tsynch) >= MSYNCH_INTERVAL) {
        oc->tsynch = tnow;
        zhe_pack_msynch(&oc->addr, MSFLAG, oc->cid, oc->seqbase, oc_get_nsamples(oc), tnow);
        zhe_pack_msend(tnow);
    }
}

void zhe_flush(zhe_time_t tnow)
{
    if (outp > 0) {
        zhe_pack_msend(tnow);
    }
}

void zhe_housekeeping(zhe_time_t tnow)
{
    /* FIXME: obviously, this is a waste of CPU time if MAX_PEERS is biggish (but worst-case cost isn't affected) */
    for (peeridx_t i = 0; i < MAX_PEERS_1; i++) {
        switch(peers[i].state) {
            case PEERST_UNKNOWN:
                break;
            case PEERST_ESTABLISHED:
                if ((zhe_timediff_t)(tnow - peers[i].tlease) > peers[i].lease_dur && peers[i].lease_dur != 0) {
                    ZT(PEERDISC, "lease expired on peer @ %u", i);
                    zhe_pack_mclose(&peers[i].oc.addr, 0, &ownid, tnow);
                    zhe_pack_msend(tnow);
                    reset_peer(i, tnow);
                }
#if HAVE_UNICAST_CONDUIT
                maybe_send_msync_oc(&peers[i].oc, tnow);
#endif
                break;
            default:
                zhe_assert(peers[i].state >= PEERST_OPENING_MIN && peers[i].state <= PEERST_OPENING_MAX);
                if ((zhe_timediff_t)(tnow - peers[i].tlease) > OPEN_INTERVAL) {
                    if (peers[i].state == PEERST_OPENING_MAX) {
                        /* maximum number of attempts reached, forget it */
                        ZT(PEERDISC, "giving up on attempting to establish a session with peer @ %u", i);
                        reset_peer(i, tnow);
                    } else {
                        ZT(PEERDISC, "retry opening a session with peer @ %u", i);
                        peers[i].state++;
                        peers[i].tlease = tnow;
                        zhe_pack_mopen(&peers[i].oc.addr, SEQNUM_LEN, &ownid, LEASE_DURATION, tnow);
                        zhe_pack_msend(tnow);
                    }
                }
                break;
        }
    }

#if N_OUT_MCONDUITS > 0
    for (cid_t cid = 0; cid < N_OUT_MCONDUITS; cid++) {
        struct out_mconduit * const mc = &out_mconduits[cid];
        maybe_send_msync_oc(&mc->oc, tnow);
    }
#endif

    zhe_send_declares(tnow);
    if ((zhe_timediff_t)(tnow - tlastscout) >= SCOUT_INTERVAL) {
        tlastscout = tnow;
        send_scout(tnow);
    }
#if ZHE_MAX_URISPACE > 0
    zhe_uristore_gc();
#endif

    /* Flush any pending output if the latency budget has been exceeded */
#if LATENCY_BUDGET != 0 && LATENCY_BUDGET != LATENCY_BUDGET_INF
    if (outp > 0 && (zhe_timediff_t)(tnow - outdeadline) >= 0) {
        zhe_pack_msend(tnow);
    }
#endif
}
