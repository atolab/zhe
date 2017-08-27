/* -*- mode: c; c-basic-offset: 4; fill-column: 95; -*- */
#include <stdio.h>
#include <inttypes.h>

#include <string.h>
#include <limits.h>

#include "zhe-assert.h"
#include "zhe-config-deriv.h"
#include "zhe-msg.h"
#include "zhe-int.h"
#include "zhe-tracing.h"
#include "zhe-time.h"
#include "zhe.h"
#include "pack.h"
#include "unpack.h"
#include "bitset.h"
#include "binheap.h"
#include "pubsub.h"

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
    zhe_time_t tack;                 /* time of most recent ack sent */
};

struct out_conduit {
    zhe_address_t addr;          /* destination address */
    seq_t    seq;                 /* next seq to send */
    seq_t    seqbase;             /* latest seq ack'd + UNIT = first available */
    seq_t    useq;                /* next unreliable seq to send */
    uint16_t pos;                 /* next byte goes into rbuf[pos] */
    uint16_t spos;                /* starting pos of current sample for patching in size */
    uint16_t firstpos;            /* starting pos (actually, size) of oldest sample in window */
    uint16_t xmitw_bytes;         /* size of transmit window pointed to by rbuf */
#if (defined(XMITW_SAMPLES) && XMITW_SAMPLES > 0) || (defined(XMITW_SAMPLES_UNICAST) && XMITW_SAMPLES_UNICAST > 0)
    uint16_t xmitw_samples;       /* size of transmit window in samples */
#endif
    zhe_time_t  tsynch;              /* next time to send out a SYNCH because of unack'd messages */
    cid_t    cid;                 /* conduit id */
    zhe_time_t  last_rexmit;         /* time of latest retransmit */
    seq_t    last_rexmit_seq;     /* latest sequence number retransmitted */
    uint8_t  draining_window: 1;  /* set to true if draining window (waiting for ACKs) after hitting limit */
    uint8_t  *rbuf;               /* reliable samples (or declarations); prepended by size (of type zhe_msgsize_t) */
#if XMITW_SAMPLE_INDEX
    seq_t    firstidx;
    uint16_t *rbufidx;            /* rbuf[rbufidx[seq % xmitw_samples]] is first byte of length of message seq */
#endif
};

struct peer {
    uint8_t state;                /* connection state for this peer */
    zhe_time_t tlease;               /* peer must send something before tlease or we'll close the session | next time for scout/open msg */
    zhe_timediff_t lease_dur;        /* lease duration in ms */
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
struct out_mconduit {
    struct out_conduit oc;        /* same transmit window management as unicast */
    struct minseqheap seqbase;    /* tracks ACKs from peers for computing oc.seqbase as min of them all */
};

static struct out_mconduit out_mconduits[N_OUT_MCONDUITS];
static uint8_t out_mconduits_oc_rbuf[N_OUT_MCONDUITS][XMITW_BYTES];
#if XMITW_SAMPLE_INDEX
static uint16_t out_mconduits_oc_rbufidx[N_OUT_MCONDUITS][XMITW_SAMPLES];
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

/* we send SCOUT messages to a separately configurable address (not so much because it really seems
   necessary to have a separate address for scouting, as that we need a statically available address
   to use for the destination of the outgoing packet) */
static zhe_address_t scoutaddr;
static struct zhe_transport *transport;

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
static uint16_t peers_oc_rbufidx[MAX_PEERS_1][XMITW_SAMPLES_UNICAST];
#endif
#endif

#if MAX_PEERS > 0
/* In peer mode, always send scouts periodically, with tnextscout giving the time for the next scout 
   message to go out. In client mode, scouting is conditional upon the state of the broker, in that
   case scouts only go out if peers[0].state = UNKNOWN, and we use peers[0].tlease to time them. */
static zhe_time_t tnextscout;
#endif

static void remove_acked_messages(struct out_conduit * const c, seq_t seq);

static void oc_reset_transmit_window(struct out_conduit * const oc)
{
    oc->seqbase = oc->seq;
    oc->firstpos = oc->spos;
    oc->draining_window = 0;
}

static void oc_setup1(struct out_conduit * const oc, cid_t cid, uint16_t xmitw_bytes, uint8_t *rbuf, uint16_t xmitw_samples, uint16_t *rbufidx)
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

static void reset_peer(peeridx_t peeridx, zhe_time_t tnow)
{
    struct peer * const p = &peers[peeridx];
    /* FIXME: stupid naming */
    zhe_rsub_clear(peeridx);
    /* If data destined for this peer, drop it it */
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
        if (zhe_minseqheap_delete(peeridx, &mc->seqbase)) {
            zhe_assert(zhe_bitset_test(p->mc_member, (unsigned)i));
            if (zhe_minseqheap_isempty(&mc->seqbase)) {
                remove_acked_messages(&mc->oc, mc->oc.seq);
            } else {
                remove_acked_messages(&mc->oc, zhe_minseqheap_get_min(&mc->seqbase));
            }
        } else {
            zhe_assert(!zhe_bitset_test(p->mc_member, (unsigned)i));
        }
    }
#endif
#ifndef NDEBUG
    /* State of most fields shouldn't matter if peer state is UNKNOWN, sequence numbers
       and transmit windows in conduits do matter (so we don't need to clear them upon
       accepting the peer) */
    memset(p, 0xee, sizeof(*p));
#endif
    if (p->state == PEERST_ESTABLISHED) {
        npeers--;
    }
    p->state = PEERST_UNKNOWN;
#if HAVE_UNICAST_CONDUIT
#if XMITW_SAMPLE_INDEX
    uint16_t * const rbufidx = peers_oc_rbufidx[peeridx];
#else
    uint16_t * const rbufidx = NULL;
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
    for (cid_t i = 0; i < N_OUT_MCONDUITS; i++) {
        struct out_mconduit * const mc = &out_mconduits[i];
#if XMITW_SAMPLE_INDEX
        uint16_t * const rbufidx = out_mconduits_oc_rbufidx[i];
#else
        uint16_t * const rbufidx = NULL;
#endif
        oc_setup1(&mc->oc, i, XMITW_BYTES, out_mconduits_oc_rbuf[i], XMITW_SAMPLES, rbufidx);
        mc->seqbase.n = 0;
        for (peeridx_t j = 0; j < MAX_PEERS; j++) {
            mc->seqbase.hx[j] = PEERIDX_INVALID;
            mc->seqbase.ix[j].i = PEERIDX_INVALID;
        }
    }
#endif
    for (peeridx_t i = 0; i < MAX_PEERS_1; i++) {
        reset_peer(i, tnow);
    }
    npeers = 0;
    reset_outbuf();
    /* FIXME: keep incoming packet buffer? I guess in packet mode that's ok, for streaming would probably need MAX_PEERS_1 */
#if LATENCY_BUDGET != 0 && LATENCY_BUDGET != LATENCY_BUDGET_INF
    outdeadline = tnow;
#endif
#if MAX_PEERS > 0
    tnextscout = tnow;
#endif
}

int zhe_seq_lt(seq_t a, seq_t b)
{
    return (sseq_t) (a - b) < 0;
}

int zhe_seq_le(seq_t a, seq_t b)
{
    return (sseq_t) (a - b) <= 0;
}

static uint16_t xmitw_pos_add(const struct out_conduit *c, uint16_t p, uint16_t a)
{
    if ((p += a) >= c->xmitw_bytes) {
        p -= c->xmitw_bytes;
    }
    return p;
}

static zhe_msgsize_t xmitw_load_msgsize(const struct out_conduit *c, uint16_t p)
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

static void xmitw_store_msgsize(const struct out_conduit *c, uint16_t p, zhe_msgsize_t sz)
{
    if (p < c->xmitw_bytes - sizeof(sz)) {
        memcpy(&c->rbuf[p], &sz, sizeof(sz));
    } else {
        memcpy(&c->rbuf[p], &sz, c->xmitw_bytes - p);
        memcpy(&c->rbuf[0], (char *)&sz + (c->xmitw_bytes - p), sizeof(sz) - (c->xmitw_bytes - p));
    }
}

static uint16_t zhe_xmitw_bytesavail(const struct out_conduit *c)
{
    uint16_t res;
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

#if XMITW_SAMPLE_INDEX
static uint16_t xmitw_load_rbufidx(const struct out_conduit *c, seq_t seq)
{
    seq_t off = (seq_t)(seq - c->seqbase) >> SEQNUM_SHIFT;
    seq_t idx = (c->firstidx + off) % c->xmitw_samples;
    return c->rbufidx[idx];
}

static void xmitw_store_rbufidx(const struct out_conduit *c, seq_t seq, uint16_t p)
{
    seq_t off = (seq_t)(seq - c->seqbase) >> SEQNUM_SHIFT;
    seq_t idx = (c->firstidx + off) % c->xmitw_samples;
    c->rbufidx[idx] = p;
}
#endif

void zhe_pack_msend(void)
{
    zhe_assert ((outspos == OUTSPOS_UNSET) == (outc == NULL));
    zhe_assert (outdst != NULL);
    if (outspos != OUTSPOS_UNSET) {
        /* FIXME: not-so-great proxy for transition past 3/4 of window size */
        uint16_t cnt = zhe_xmitw_bytesavail(outc);
        if (cnt < outc->xmitw_bytes / 4 && cnt + outspos >= outc->xmitw_bytes / 4) {
            outbuf[outspos] |= MSFLAG;
        }
    }
    if (transport->ops->send(transport, outbuf, outp, outdst) < 0) {
        zhe_assert(0);
    }
    outp = 0;
    outspos = OUTSPOS_UNSET;
    outc = NULL;
    outdst = NULL;
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
        zhe_pack_msend();
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
        ZT(DEBUG, ("deadline at %"PRIu32".%0"PRIu32, ZTIME_TO_SECu32(outdeadline), ZTIME_TO_MSECu32(outdeadline)));
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

void zhe_pack_u16(uint16_t x)
{
    zhe_pack2(x & 0xff, x >> 8);
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
        size_t n1 = transport->ops->addr2string(transport, tmp, sizeof(tmp), &multicast_locators[i]);
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
        uint16_t n1 = (uint16_t)transport->ops->addr2string(transport, tmp, sizeof(tmp), &multicast_locators[i]);
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
        zhe_pack_msend();
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

static void check_xmitw(const struct out_conduit *c);

zhe_msgsize_t zhe_oc_pack_payload_msgprep(seq_t *s, struct out_conduit *c, int relflag, zhe_paysize_t sz, zhe_time_t tnow)
{
    check_xmitw(c);
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
            c->tsynch = tnow + MSYNCH_INTERVAL;
        }
        /* prep for next sample */
        c->seq += SEQNUM_UNIT;
        check_xmitw(c);
    }
}

struct out_conduit *zhe_out_conduit_from_cid(peeridx_t peeridx, cid_t cid)
{
    struct out_conduit *c;
    DO_FOR_UNICAST_OR_MULTICAST(cid, c = &peers[peeridx].oc, c = &out_mconduits[cid].oc);
    return c;
}

///////////////////////////////////////////////////////////////////////////////////////////

static const uint8_t *handle_dresource(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data, int interpret)
{
    /* No use for a broker declaring its resources, but we don't bug out over it */
    uint8_t hdr;
    zhe_paysize_t dummy;
    if (!zhe_unpack_byte(end, &data, &hdr) ||
        !zhe_unpack_rid(end, &data, NULL) ||
        !zhe_unpack_vec(end, &data, 0, &dummy, NULL)) {
        return 0;
    }
    if ((hdr & DPFLAG) && !zhe_unpack_props(end, &data)) {
        return 0;
    }
    return data;
}

static const uint8_t *handle_dpub(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data, int interpret)
{
    /* No use for a broker declaring its publications, but we don't bug out over it */
    uint8_t hdr;
    if (!zhe_unpack_byte(end, &data, &hdr) ||
        !zhe_unpack_rid(end, &data, NULL)) {
        return 0;
    }
    if ((hdr & DPFLAG) && !zhe_unpack_props(end, &data)) {
        return 0;
    }
    return data;
}

static const uint8_t *handle_dsub(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data, int interpret)
{
    zhe_rid_t rid;
    uint8_t hdr, mode;
    if (!zhe_unpack_byte(end, &data, &hdr) ||
        !zhe_unpack_rid(end, &data, &rid) ||
        !zhe_unpack_byte(end, &data, &mode)) {
        return 0;
    }
    if (mode == 0 || mode > SUBMODE_MAX) {
        return 0;
    }
    if (mode == SUBMODE_PERIODPULL || mode == SUBMODE_PERIODPUSH) {
        if (!zhe_unpack_vle32(end, &data, NULL) ||
            !zhe_unpack_vle32(end, &data, NULL)) {
            return 0;
        }
    }
    if ((hdr & DPFLAG) && !zhe_unpack_props(end, &data)) {
        return 0;
    }
    if (interpret) {
        zhe_rsub_register(peeridx, rid, mode);
    }
    return data;
}

static const uint8_t *handle_dselection(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data, int interpret)
{
    /* FIXME: support selections? */
    zhe_rid_t sid;
    uint8_t hdr;
    zhe_paysize_t dummy;
    if (!zhe_unpack_byte(end, &data, &hdr) ||
        !zhe_unpack_rid(end, &data, &sid) ||
        !zhe_unpack_vec(end, &data, 0, &dummy, NULL)) {
        return 0;
    }
    if ((hdr & DPFLAG) && !zhe_unpack_props(end, &data)) {
        return 0;
    }
    if (interpret) {
        zhe_decl_note_error(4, sid);
    }
    return data;
}

static const uint8_t *handle_dbindid(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data, int interpret)
{
    /* FIXME: support bindings?  I don't think there's a need. */
    zhe_rid_t sid;
    if (!zhe_unpack_skip(end, &data, 1) ||
        !zhe_unpack_rid(end, &data, &sid) ||
        !zhe_unpack_rid(end, &data, NULL)) {
        return 0;
    }
    if (interpret) {
        zhe_decl_note_error(8, sid);
    }
    return data;
}

static const uint8_t *handle_dcommit(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data, int interpret, zhe_time_t tnow)
{
    uint8_t commitid;
    uint8_t res;
    zhe_rid_t err_rid;
    if (!zhe_unpack_skip(end, &data, 1) ||
        !zhe_unpack_byte(end, &data, &commitid)) {
        return 0;
    }
    if (interpret) {
        /* If we can't reserve space in the transmit window, pretend we never received the
         DECLARE message and abandon the rest of the packet.  Eventually we'll get a
         retransmit and retry.  Use worst-case size for result */
        struct out_conduit * const oc = zhe_out_conduit_from_cid(0, 0);
        zhe_msgsize_t from;
        if (!zhe_oc_pack_mdeclare(oc, 1, WC_DRESULT_SIZE, &from, tnow)) {
            return 0;
        } else {
            zhe_rsub_precommit_curpkt_done(peeridx);
            if ((res = zhe_rsub_precommit(peeridx, &err_rid)) == 0) {
                zhe_rsub_commit(peeridx);
            }
            zhe_pack_dresult(commitid, res, err_rid);
            zhe_oc_pack_mdeclare_done(oc, from, tnow);
            zhe_pack_msend();
        }
    }
    return data;
}

static const uint8_t *handle_dresult(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data, int interpret)
{
    uint8_t commitid, status;
    zhe_rid_t rid = 0;
    if (!zhe_unpack_skip(end, &data, 1) ||
        !zhe_unpack_byte(end, &data, &commitid) ||
        !zhe_unpack_byte(end, &data, &status)) {
        return 0;
    }
    if (status && !zhe_unpack_rid(end, &data, &rid)) {
        return 0;
    }
    ZT(PUBSUB, ("handle_dresult %u intp %d | commitid %u status %u rid %ju", (unsigned)peeridx, interpret, commitid, status, (uintmax_t)rid));
    if (interpret && status != 0) {
        /* Don't know what to do when the broker refuses my declarations - although I guess it
         would make some sense to close the connection and try again.  But even if that is
         the right thing to do, don't do that just yet, because it shouldn't fail.

         Also note that we're not looking at the commit id at all, I am not sure yet what
         problems that may cause ... */
        zhe_assert(0);
    }
    return data;
}

static const uint8_t *handle_ddeleteres(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data, int interpret)
{
    uint8_t hdr;
    zhe_paysize_t dummy;
    if (!zhe_unpack_byte(end, &data, &hdr) ||
        !zhe_unpack_vec(end, &data, 0, &dummy, NULL)) {
        return 0;
    }
    if ((hdr & DPFLAG) && !zhe_unpack_props(end, &data)) {
        return 0;
    }
    if (interpret) {
        zhe_decl_note_error(16, 0);
    }
    return data;
}

///////////////////////////////////////////////////////////////////////////////////////////

static const uint8_t *handle_mscout(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data, zhe_time_t tnow)
{
#if MAX_PEERS > 0
    const uint32_t lookfor = MSCOUT_PEER;
    const int state_ok = (peers[peeridx].state == PEERST_UNKNOWN);
#else
    const uint32_t lookfor = MSCOUT_CLIENT;
    const int state_ok = 1;
#endif
    uint8_t hdr;
    uint32_t mask;
    if (!zhe_unpack_byte(end, &data, &hdr) ||
        !zhe_unpack_vle32(end, &data, &mask)) {
        return 0;
    }
    /* For a client all activity is really client-initiated, so we can get away
       with not responding to a SCOUT; for a peer it is different */
    if ((mask & lookfor) && state_ok) {
        ZT(PEERDISC, ("got a scout! sending a hello"));
        zhe_pack_mhello(&peers[peeridx].oc.addr, tnow);
        zhe_pack_msend();
    }
    return data;
}

static int set_peer_mcast_locs(peeridx_t peeridx, struct unpack_locs_iter *it)
{
    zhe_paysize_t sz;
    const uint8_t *loc;
    while (zhe_unpack_locs_iter(it, &sz, &loc)) {
#if N_OUT_MCONDUITS > 0
        for (cid_t cid = 0; cid < N_OUT_MCONDUITS; cid++) {
            char tmp[TRANSPORT_ADDRSTRLEN];
            const size_t tmpsz = transport->ops->addr2string(transport, tmp, sizeof(tmp), &out_mconduits[cid].oc.addr);
            if (sz == tmpsz && memcmp(loc, tmp, sz) == 0) {
                zhe_bitset_set(peers[peeridx].mc_member, (unsigned)cid);
                ZT(PEERDISC, ("loc %s cid %u", tmp, (unsigned)cid));
            }
        }
#endif
    }
    return 1;
}

static const uint8_t *handle_mhello(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data, zhe_time_t tnow)
{
#if MAX_PEERS > 0
    const uint32_t lookfor = MSCOUT_PEER | MSCOUT_BROKER;
    const int state_ok = (peers[peeridx].state == PEERST_UNKNOWN || peers[peeridx].state == PEERST_ESTABLISHED);
#else
    const uint32_t lookfor = MSCOUT_BROKER;
    const int state_ok = (peers[peeridx].state == PEERST_UNKNOWN);
#endif
    struct unpack_locs_iter locs_it;
    uint8_t hdr;
    uint32_t mask;
    if (!zhe_unpack_byte(end, &data, &hdr) ||
        !zhe_unpack_vle32(end, &data, &mask) ||
        !zhe_unpack_locs(end, &data, &locs_it) ||
        !zhe_unpack_props(end, &data)) {
        return 0;
    }
    if ((mask & lookfor) && state_ok) {
        int send_open = 1;

        ZT(PEERDISC, ("got a hello! sending an open"));
        if (peers[peeridx].state != PEERST_ESTABLISHED) {
            if (!set_peer_mcast_locs(peeridx, &locs_it)) {
                ZT(PEERDISC, ("'twas but a hello with an invalid locator list ..."));
                send_open = 0;
            } else {
                peers[peeridx].state = PEERST_OPENING_MIN;
                peers[peeridx].tlease = tnow;
            }
        } else {
            /* FIXME: a hello when established indicates a reconnect for the other one => should at least clear ic[.].synched, usynched - but maybe more if we want some kind of nothing of the event ... */
            for (cid_t cid = 0; cid < N_IN_CONDUITS; cid++) {
                peers[peeridx].ic[cid].synched = 0;
                peers[peeridx].ic[cid].usynched = 0;
            }
        }
        if (send_open) {
            zhe_pack_mopen(&peers[peeridx].oc.addr, SEQNUM_LEN, &ownid, LEASE_DURATION, tnow);
            zhe_pack_msend();
        }
    }
    return data;
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
            if (ZTT(PEERDISC)) {
                char olda[TRANSPORT_ADDRSTRLEN], newa[TRANSPORT_ADDRSTRLEN];
                transport->ops->addr2string(transport, olda, sizeof(olda), &peers[i].oc.addr);
                transport->ops->addr2string(transport, newa, sizeof(newa), &peers[peeridx].oc.addr);
                ZT(PEERDISC, ("peer %u changed address from %s to %s", (unsigned)i, olda, newa));
            }
            peers[i].oc.addr = peers[peeridx].oc.addr;
            return i;
        }
    }
    return peeridx;
}

static char tohexdigit(uint8_t x)
{
    zhe_assert(x <= 15);
    return (x <= 9) ? (char)('0' + x) : (char)('a' + (x - 10));
}

static void accept_peer(peeridx_t peeridx, zhe_paysize_t idlen, const uint8_t * restrict id, zhe_timediff_t lease_dur, zhe_time_t tnow)
{
    struct peer * const p = &peers[peeridx];
    zhe_assert(p->state != PEERST_ESTABLISHED);
    zhe_assert(idlen > 0);
    zhe_assert(idlen <= PEERID_SIZE);
    zhe_assert(lease_dur >= 0);

    if (ZTT(PEERDISC)) {
        char astr[TRANSPORT_ADDRSTRLEN];
        char idstr[3*PEERID_SIZE], *idstrp = idstr;
        transport->ops->addr2string(transport, astr, sizeof(astr), &p->oc.addr);
        for (int i = 0; i < idlen; i++) {
            if (i > 0) {
                *idstrp++ = ':';
            }
            *idstrp++ = tohexdigit(id[i] >> 4);
            *idstrp++ = tohexdigit(id[i] & 0xf);
            zhe_assert(idstrp < idstr + sizeof(idstr));
        }
        *idstrp = 0;
        ZT(PEERDISC, ("accept peer %s %s @ %u; lease = %" PRId32, idstr, astr, peeridx, (int32_t)lease_dur));
    }

    p->state = PEERST_ESTABLISHED;
    p->id.len = idlen;
    memcpy(p->id.id, id, idlen);
    p->lease_dur = lease_dur;
    p->tlease = tnow + (zhe_time_t)p->lease_dur;
#if N_OUT_MCONDUITS > 0
    for (cid_t cid = 0; cid < N_OUT_MCONDUITS; cid++) {
        if (zhe_bitset_test(p->mc_member, (unsigned)cid)) {
            struct out_mconduit * const mc = &out_mconduits[cid];
            zhe_minseqheap_insert(peeridx, mc->oc.seq, &mc->seqbase);
        }
    }
#endif
    npeers++;

    /* FIXME: stupid naming - but we do need to declare everything (again). A much more sophisticated version could use the unicast channels for late joining ones, but we ain't there yet */
    zhe_reset_pubs_to_declare();
    zhe_reset_subs_to_declare();
}

static int conv_lease_to_ztimediff(zhe_timediff_t *res, uint32_t ld100)
{
    if (ld100 > ZHE_TIMEDIFF_MAX / (100000000 / ZHE_TIMEBASE)) {
        return 0;
    }
    *res = (100000000 / ZHE_TIMEBASE) * (zhe_timediff_t)ld100;
    return 1;
}

static const uint8_t *handle_mopen(peeridx_t * restrict peeridx, const uint8_t * const end, const uint8_t *data, zhe_time_t tnow)
{
    uint8_t hdr, version;
    uint16_t seqsize;
    zhe_paysize_t idlen;
    uint8_t id[PEERID_SIZE];
    zhe_paysize_t dummy;
    uint8_t reason;
    uint32_t ld100;
    zhe_timediff_t ld;
    struct unpack_locs_iter locs_it;
    struct peer *p;
    if (!zhe_unpack_byte(end, &data, &hdr) ||
        !zhe_unpack_byte(end, &data, &version) /* version */ ||
        !zhe_unpack_vec(end, &data, sizeof(id), &idlen, id) /* peer id */ ||
        !zhe_unpack_vle32(end, &data, &ld100) /* lease duration */ ||
        !zhe_unpack_vec(end, &data, 0, &dummy, NULL) /* auth */ ||
        !zhe_unpack_locs(end, &data, &locs_it)) {
        return 0;
    }
    if (!(hdr & MLFLAG)) {
        seqsize = 14;
    } else if (!zhe_unpack_vle16(end, &data, &seqsize)) {
        return 0;
    } else if (seqsize != SEQNUM_LEN) {
        ZT(PEERDISC, ("got an open with an unsupported sequence number size (%hu)", seqsize));
        reason = CLR_UNSUPP_SEQLEN;
        goto reject;
    }
    if (version != ZHE_VERSION) {
        ZT(PEERDISC, ("got an open with an unsupported version (%hhu)", version));
        reason = CLR_UNSUPP_PROTO;
        goto reject;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        ZT(PEERDISC, ("got an open with an under- or oversized id (%hu)", idlen));
        reason = CLR_ERROR;
        goto reject;
    }
    if (!conv_lease_to_ztimediff(&ld, ld100)) {
        ZT(PEERDISC, ("got an open with a lease duration that is not representable here"));
        reason = CLR_ERROR;
        goto reject;
    }
    if (idlen == ownid.len && memcmp(ownid.id, id, idlen) == 0) {
        ZT(PEERDISC, ("got an open with my own peer id"));
        goto reject_no_close;
    }

    *peeridx = find_peeridx_by_id(*peeridx, idlen, id);

    p = &peers[*peeridx];
    if (p->state != PEERST_ESTABLISHED) {
        if (!set_peer_mcast_locs(*peeridx, &locs_it)) {
            ZT(PEERDISC, ("'twas but an open with an invalid locator list ..."));
            reason = CLR_ERROR;
            goto reject;
        }
        accept_peer(*peeridx, idlen, id, ld, tnow);
    }
    zhe_pack_maccept(&p->oc.addr, &ownid, &p->id, LEASE_DURATION, tnow);
    zhe_pack_msend();

    return data;

reject:
    zhe_pack_mclose(&peers[*peeridx].oc.addr, reason, &ownid, tnow);
    /* don't want anything to do with the other anymore; calling reset on one that is already in UNKNOWN is harmless */
    reset_peer(*peeridx, tnow);
    /* no point in interpreting following messages in packet */
reject_no_close:
    return 0;
}

static const uint8_t *handle_maccept(peeridx_t * restrict peeridx, const uint8_t * const end, const uint8_t *data, zhe_time_t tnow)
{
    zhe_paysize_t idlen;
    uint8_t id[PEERID_SIZE];
    uint32_t ld100;
    zhe_timediff_t ld;
    zhe_paysize_t dummy;
    int forme;
    if (!zhe_unpack_skip(end, &data, 1) ||
        !zhe_unpack_vec(end, &data, sizeof(id), &idlen, id)) {
        return 0;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        ZT(PEERDISC, ("got an open with an under- or oversized id (%hu)", idlen));
        goto reject_no_close;
    }
    forme = (idlen == ownid.len && memcmp(id, ownid.id, idlen) == 0);
    if (!zhe_unpack_vec(end, &data, sizeof (id), &idlen, id) ||
        !zhe_unpack_vle32(end, &data, &ld100) ||
        !zhe_unpack_vec(end, &data, 0, &dummy, NULL)) {
        return 0;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        ZT(PEERDISC, ("got an open with an under- or oversized id (%hu)", idlen));
        goto reject_no_close;
    }
    if (forme) {
        if (!conv_lease_to_ztimediff(&ld, ld100)) {
            ZT(PEERDISC, ("got an open with a lease duration that is not representable here"));
            goto reject;
        }
        *peeridx = find_peeridx_by_id(*peeridx, idlen, id);
        if (peers[*peeridx].state >= PEERST_OPENING_MIN && peers[*peeridx].state <= PEERST_OPENING_MAX) {
            accept_peer(*peeridx, idlen, id, ld, tnow);
        }
    }
    return data;

reject:
    zhe_pack_mclose(&peers[*peeridx].oc.addr, CLR_ERROR, &ownid, tnow);
    /* don't want anything to do with the other anymore; calling reset on one that is already in UNKNOWN is harmless */
    reset_peer(*peeridx, tnow);
    /* no point in interpreting following messages in packet */
reject_no_close:
    return 0;
}

static const uint8_t *handle_mclose(peeridx_t * restrict peeridx, const uint8_t * const end, const uint8_t *data, zhe_time_t tnow)
{
    zhe_paysize_t idlen;
    uint8_t id[PEERID_SIZE];
    uint8_t reason;
    if (!zhe_unpack_skip(end, &data, 1) ||
        !zhe_unpack_vec(end, &data, sizeof(id), &idlen, id) ||
        !zhe_unpack_byte(end, &data, &reason)) {
        return 0;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        ZT(PEERDISC, ("got a close with an under- or oversized id (%hu)", idlen));
        reset_peer(*peeridx, tnow);
        return 0;
    }
    *peeridx = find_peeridx_by_id(*peeridx, idlen, id);
    if (peers[*peeridx].state != PEERST_UNKNOWN) {
        reset_peer(*peeridx, tnow);
    }
    return 0;
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
        ZT(RELIABLE, ("acknack_if_needed peeridx %u cid %u wantsack %d mask %u seq %u", peeridx, cid, wantsack, mask, peers[peeridx].ic[cid].seq >> SEQNUM_SHIFT));
        zhe_pack_macknack(&peers[peeridx].oc.addr, cid, peers[peeridx].ic[cid].seq, mask, tnow);
        zhe_pack_msend();
        peers[peeridx].ic[cid].tack = tnow;
    }
}

static const uint8_t *handle_mdeclare(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data, cid_t cid, zhe_time_t tnow)
{
    /* Note 1: not buffering data received out-of-order, so but need to decode everything to
       find next message, which we may "have to" interpret - we don't really "have to", but to
       simply pretend we never received it is a bit rough.

       Note 2: a commit requires us to send something in reply, but we may be unable to because
       of a full transmit window in the reliable channel.  The elegant option is to suspend
       further input processing, until space is available again, the inelegant one is to verify
       we have space beforehand, and pretend we never received the DECLARE if we don't. */
    uint8_t hdr;
    uint16_t ndecls;
    seq_t seq;
    int intp;
    if (!zhe_unpack_byte(end, &data, &hdr) ||
        !zhe_unpack_seq(end, &data, &seq) ||
        !zhe_unpack_vle16(end, &data, &ndecls)) {
        return 0;
    }
    if (!(peers[peeridx].state == PEERST_ESTABLISHED && peers[peeridx].ic[cid].synched)) {
        intp = 0;
    } else {
        if (zhe_seq_le(peers[peeridx].ic[cid].lseqpU, seq)) {
            peers[peeridx].ic[cid].lseqpU = seq + SEQNUM_UNIT;
        }
        intp = ic_may_deliver_seq(&peers[peeridx].ic[cid], hdr, seq);
    }
    ZT(PUBSUB, ("handle_mdeclare %p seq %u peeridx %u ndecls %u intp %d", data, seq, peeridx, ndecls, intp));
    while (ndecls > 0 && data < end && data != 0) {
        switch (*data & DKIND) {
            case DRESOURCE:  data = handle_dresource(peeridx, end, data, intp); break;
            case DPUB:       data = handle_dpub(peeridx, end, data, intp); break;
            case DSUB:       data = handle_dsub(peeridx, end, data, intp); break;
            case DSELECTION: data = handle_dselection(peeridx, end, data, intp); break;
            case DBINDID:    data = handle_dbindid(peeridx, end, data, intp); break;
            case DCOMMIT:    data = handle_dcommit(peeridx, end, data, intp, tnow); break;
            case DRESULT:    data = handle_dresult(peeridx, end, data, intp); break;
            case DDELETERES: data = handle_ddeleteres(peeridx, end, data, intp); break;
            default:         data = 0; break;
        }
        if (data != 0) {
            --ndecls;
        }
    }
    if (intp && ndecls != 0) {
        ZT(PUBSUB, ("handle_mdeclare %u .. abort", peeridx));
        zhe_rsub_precommit_curpkt_abort(peeridx);
        return 0;
    }
    if (intp) {
        /* Merge uncommitted declaration state resulting from this DECLARE message into
           uncommitted state accumulator, as we have now completely and successfully processed
           this message.  */
        ZT(PUBSUB, ("handle_mdeclare %u .. packet done", peeridx));
        zhe_rsub_precommit_curpkt_done(peeridx);
        (void)ic_update_seq(&peers[peeridx].ic[cid], hdr, seq);
    }
    if (peers[peeridx].state == PEERST_ESTABLISHED && peers[peeridx].ic[cid].synched) {
        acknack_if_needed(peeridx, cid, hdr & MSFLAG, tnow);
    }
    return data;
}

static const uint8_t *handle_msynch(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data, cid_t cid, zhe_time_t tnow)
{
    uint8_t hdr;
    seq_t cnt_shifted;
    seq_t seqbase;
    if (!zhe_unpack_byte(end, &data, &hdr) ||
        !zhe_unpack_seq(end, &data, &seqbase) ||
        !zhe_unpack_seq(end, &data, &cnt_shifted)) {
        return 0;
    }
    if (peers[peeridx].state == PEERST_ESTABLISHED) {
        ZT(RELIABLE, ("handle_msynch peeridx %u cid %u seq %u cnt %u", peeridx, cid, seqbase >> SEQNUM_SHIFT, cnt_shifted >> SEQNUM_SHIFT));
        if (zhe_seq_le(peers[peeridx].ic[cid].seq, seqbase) || !peers[peeridx].ic[cid].synched) {
            if (!peers[peeridx].ic[cid].synched) {
                ZT(PEERDISC, ("handle_msynch peeridx %u cid %u seq %u cnt %u", peeridx, cid, seqbase >> SEQNUM_SHIFT, cnt_shifted >> SEQNUM_SHIFT));
            }
            peers[peeridx].ic[cid].seq = seqbase;
            peers[peeridx].ic[cid].lseqpU = seqbase + cnt_shifted;
            peers[peeridx].ic[cid].synched = 1;
        }
        acknack_if_needed(peeridx, cid, hdr & MSFLAG, tnow);
    }
    return data;
}

unsigned zhe_delivered, zhe_discarded;

static const uint8_t *handle_msdata(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data, cid_t cid, zhe_time_t tnow)
{
    uint8_t hdr;
    zhe_paysize_t paysz;
    seq_t seq;
    zhe_rid_t rid, prid;
    if (!zhe_unpack_byte(end, &data, &hdr) ||
        !zhe_unpack_seq(end, &data, &seq) ||
        !zhe_unpack_rid(end, &data, &rid)) {
        return 0;
    }
    if (!(hdr & MPFLAG)) {
        prid = rid;
    } else if (!zhe_unpack_rid(end, &data, &prid)) {
        return 0;
    }

    /* Attempt to "extract" payload -- we don't actually extract it but leave it in place to save memory
       and time.  If it is fully present, pay will still point to the payload size and all
       we need to redo is skip the VLE encoded length in what we know to be a valid buffer */
    const uint8_t * const pay = data;
    if (!zhe_unpack_vec(end, &data, 0, &paysz, NULL)) {
        return 0;
    }

    if (peers[peeridx].state != PEERST_ESTABLISHED) {
        /* Not accepting data from peers that we haven't (yet) established a connection with */
        return data;
    }

    if (!(hdr & MRFLAG)) {
        if (ic_may_deliver_seq(&peers[peeridx].ic[cid], hdr, seq)) {
            (void)zhe_handle_msdata_deliver(prid, paysz, zhe_skip_validated_vle(pay));
            ic_update_seq(&peers[peeridx].ic[cid], hdr, seq);
        }
    } else if (peers[peeridx].ic[cid].synched) {
        if (zhe_seq_le(peers[peeridx].ic[cid].lseqpU, seq)) {
            peers[peeridx].ic[cid].lseqpU = seq + SEQNUM_UNIT;
        }
        if (ic_may_deliver_seq(&peers[peeridx].ic[cid], hdr, seq)) {
            ZT(RELIABLE, ("handle_msdata peeridx %u cid %u seq %u deliver", peeridx, cid, seq >> SEQNUM_SHIFT));
            if (zhe_handle_msdata_deliver(prid, paysz, zhe_skip_validated_vle(pay))) {
                /* if failed to deliver, we must retry, which necessitates a retransmit and not updating the conduit state */
                ic_update_seq(&peers[peeridx].ic[cid], hdr, seq);
            }
            zhe_delivered++;
        } else {
            ZT(RELIABLE, ("handle_msdata peeridx %u cid %u seq %u != %u", peeridx, cid, seq >> SEQNUM_SHIFT, peers[peeridx].ic[cid].seq >> SEQNUM_SHIFT));
            zhe_discarded++;
        }
        acknack_if_needed(peeridx, cid, hdr & MSFLAG, tnow);
    }

    return data;
}

#ifndef NDEBUG
static void check_xmitw(const struct out_conduit *c)
{
#if 0
    zhe_assert(c->pos == xmitw_pos_add(c, c->spos, sizeof(zhe_msgsize_t)));
    if (c->seq == c->seqbase) {
        zhe_assert(c->spos == c->firstpos);
    } else {
        zhe_msgsize_t len;
        uint16_t p;
        seq_t seq;
        seq = c->seqbase;
        p = c->firstpos;
        do {
            zhe_assert(p == xmitw_load_rbufidx(c, seq));
            len = xmitw_load_msgsize(c, p);
            seq += SEQNUM_UNIT;
            p = xmitw_pos_add(c, p, len + sizeof(zhe_msgsize_t));
        } while (seq != c->seq);
        assert(p == c->spos);
    }
#endif
}
#endif

static void remove_acked_messages(struct out_conduit * restrict c, seq_t seq)
{
    ZT(RELIABLE, ("remove_acked_messages cid %u %p seq %u", c->cid, (void*)c, seq >> SEQNUM_SHIFT));

#ifndef NDEBUG
    check_xmitw(c);
#endif

    if (zhe_seq_lt(c->seq, seq)) {
        /* Broker is ACKing samples we haven't even sent yet, use the opportunity to drain the
           transmit window */
        seq = c->seq;
    }

    if(zhe_seq_lt(c->seqbase, seq)) {
#if XMITW_SAMPLE_INDEX
        seq_t cnt = (seq_t)(seq - c->seqbase) >> SEQNUM_SHIFT;
        if (seq == c->seq) {
            c->firstpos = c->spos;
            c->firstidx = (c->firstidx + cnt) % c->xmitw_samples;
            c->seqbase = seq;
        } else {
            /* FIXME: beware, order matters ... */
            c->firstpos = xmitw_load_rbufidx(c, seq);
            c->firstidx = (c->firstidx + cnt) % c->xmitw_samples;
            c->seqbase = seq;
        }
#else
        /* Acking some samples, drop everything from seqbase up to but not including seq */
#ifndef NDEBUG
        seq_t cnt = (seq_t)(seq - c->seqbase) >> SEQNUM_SHIFT;
#endif
        while (c->seqbase != seq) {
            zhe_msgsize_t len;
            zhe_assert(cnt > 0);
#ifndef NDEBUG
            cnt--;
#endif
            c->seqbase += SEQNUM_UNIT;
            len = xmitw_load_msgsize(c, c->firstpos);
            c->firstpos = xmitw_pos_add(c, c->firstpos, len + sizeof(zhe_msgsize_t));
        }
        zhe_assert(cnt == 0);
#endif
        zhe_assert(((c->firstpos + sizeof(zhe_msgsize_t)) % c->xmitw_bytes == c->pos) == (c->seq == c->seqbase));
    }

    if (oc_get_nsamples(c) == 0) {
        c->draining_window = 0;
    }

    check_xmitw(c);
}

static const uint8_t *handle_macknack(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data, cid_t cid, zhe_time_t tnow)
{
    struct out_conduit * const c = zhe_out_conduit_from_cid(peeridx, cid);
    seq_t seq, seq_ack;
    uint8_t hdr;
    uint32_t mask;
    if (!zhe_unpack_byte(end, &data, &hdr) ||
        !zhe_unpack_seq(end, &data, &seq)) {
        return 0;
    }
    if (!(hdr & MMFLAG)) {
        mask = 0;
    } else if (!zhe_unpack_vle32(end, &data, &mask)) {
        return 0;
    } else {
        /* Make the retransmit request for message SEQ implied by the use of an ACKNACK
         explicit in the mask (which means we won't retransmit SEQ + 32). */
        mask = (mask << 1) | 1;
    }
    if (peers[peeridx].state != PEERST_ESTABLISHED) {
        return data;
    }

    DO_FOR_UNICAST_OR_MULTICAST(cid, seq_ack = seq, seq_ack = zhe_minseqheap_update_seq(peeridx, seq, c->seqbase, &out_mconduits[cid].seqbase));
    remove_acked_messages(c, seq_ack);

    if (mask == 0) {
        /* Pure ACK - no need to do anything else */
        if (seq != c->seq) {
            ZT(RELIABLE, ("handle_macknack peeridx %u cid %u seq %u ACK but we have [%u,%u]", peeridx, cid, seq >> SEQNUM_SHIFT, c->seqbase >> SEQNUM_SHIFT, (c->seq >> SEQNUM_SHIFT)-1));
        } else {
            ZT(RELIABLE, ("handle_macknack peeridx %u cid %u seq %u ACK", peeridx, cid, seq >> SEQNUM_SHIFT));
        }
    } else if (zhe_seq_lt(seq, c->seqbase) || zhe_seq_le(c->seq, seq)) {
        /* If the broker ACKs stuff we have dropped already, or if it NACKs stuff we have not
           even sent yet, send a SYNCH without the S flag (i.e., let the broker decide what to
           do with it) */
        ZT(RELIABLE, ("handle_macknack peeridx %u cid %u %p seq %u mask %08x - [%u,%u] - send synch", peeridx, cid, (void*)c, seq >> SEQNUM_SHIFT, mask, c->seqbase >> SEQNUM_SHIFT, (c->seq >> SEQNUM_SHIFT)-1));
        zhe_pack_msynch(&c->addr, 0, c->cid, c->seqbase, oc_get_nsamples(c), tnow);
        zhe_pack_msend();
    } else if ((zhe_timediff_t)(tnow - c->last_rexmit) <= ROUNDTRIP_TIME_ESTIMATE && zhe_seq_lt(seq, c->last_rexmit_seq)) {
        ZT(RELIABLE, ("handle_macknack peeridx %u cid %u seq %u mask %08x - suppress", peeridx, cid, seq >> SEQNUM_SHIFT, mask));
    } else {
        /* Retransmits can always be performed because they do not require buffering new
           messages, all we need to do is push out the buffered messages.  We want the S bit
           set on the last of the retransmitted ones, so we "clear" outspos and then set it
           before pushing out that last sample. */
        uint16_t p;
        zhe_msgsize_t sz, outspos_tmp = OUTSPOS_UNSET;
        ZT(RELIABLE, ("handle_macknack peeridx %u cid %u seq %u mask %08x", peeridx, cid, seq >> SEQNUM_SHIFT, mask));
#if MAX_PEERS == 0
        zhe_assert(seq == c->seqbase);
#endif
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
        sz = xmitw_load_msgsize(c, p);
        p = xmitw_pos_add(c, p, sizeof(zhe_msgsize_t));
#else
        p = c->firstpos;
        sz = xmitw_load_msgsize(c, p);
        p = xmitw_pos_add(c, p, sizeof(zhe_msgsize_t));
#if MAX_PEERS != 0
        {
            seq_t seqbase = c->seqbase;
            while (zhe_seq_lt(seqbase, seq)) {
                p = xmitw_pos_add(c, p, sz);
                seqbase += SEQNUM_UNIT;
                sz = xmitw_load_msgsize(c, p);
                p = xmitw_pos_add(c, p, sizeof(zhe_msgsize_t));
            }
        }
#endif
#endif
        while (mask && zhe_seq_lt(seq, c->seq)) {
            if ((mask & 1) == 0) {
                p = xmitw_pos_add(c, p, sz);
            } else {
                /* Out conduit is NULL so that the invariant that (outspos == OUTSPOS_UNSET) <=> 
                   (outc == NULL) is maintained, and also in consideration of the fact that keeping
                   track of the conduit and the position of the last reliable message is solely
                   for the purpose of setting the S flag and scheduling SYNCH messages.  Retransmits
                   are require none of that beyond what we do here locally anyway. */
                ZT(RELIABLE, ("handle_macknack   rx %u", seq >> SEQNUM_SHIFT));
                zhe_pack_reserve_mconduit(&c->addr, NULL, cid, sz, tnow);
                outspos_tmp = outp;
                while (sz--) {
                    zhe_pack1(c->rbuf[p]);
                    p = xmitw_pos_add(c, p, 1);
                }
            }
            mask >>= 1;
            seq += SEQNUM_UNIT;
            sz = xmitw_load_msgsize(c, p);
            p = xmitw_pos_add(c, p, sizeof(zhe_msgsize_t));
        }
        c->last_rexmit = tnow;
        c->last_rexmit_seq = seq;
        /* Asserting that seq <= c->seq is a somewhat nonsensical considering the guards for
           this block and the loop condition, but it clarifies the second zhe_assertion: if we got
           all the way to the most recent sample, then P should point to the first free
           position in the transmit window, a.k.a. c->pos.  */
        zhe_assert(zhe_seq_le(seq, c->seq));
        zhe_assert(seq != c->seq || p == c->pos);
        /* Since we must have sent at least one message, outspos_tmp must have been set.  Set
           the S flag in that final message. Also make sure we send a SYNCH not too long after
           (and so do all that pack_msend would otherwise have done for c). */
        zhe_assert(outspos_tmp != OUTSPOS_UNSET);
        /* Note: setting the S bit is not the same as a SYNCH, maybe it would be better to send
           a SYNCH instead? */
        outbuf[outspos_tmp] |= MSFLAG;
        zhe_pack_msend();
    }
    return data;
}

static const uint8_t *handle_mping(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data, zhe_time_t tnow)
{
    uint16_t hash;
    if (!zhe_unpack_skip(end, &data, 1) ||
        !zhe_unpack_u16(end, &data, &hash)) {
        return 0;
    }
    zhe_pack_mpong(&peers[peeridx].oc.addr, hash, tnow);
    zhe_pack_msend();
    return data;
}

static const uint8_t *handle_mpong(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data)
{
    if (!zhe_unpack_skip(end, &data, 3)) {
        return 0;
    }
    return data;
}

static const uint8_t *handle_mkeepalive(peeridx_t * restrict peeridx, const uint8_t * const end, const uint8_t *data, zhe_time_t tnow)
{
    zhe_paysize_t idlen;
    uint8_t id[PEERID_SIZE];
    if (!zhe_unpack_skip(end, &data, 1) ||
        !zhe_unpack_vec(end, &data, sizeof(id), &idlen, id)) {
        return 0;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        reset_peer(*peeridx, tnow);
        return 0;
    }
    (void)find_peeridx_by_id(*peeridx, idlen, id);
    return data;
}

static const uint8_t *handle_mconduit(peeridx_t peeridx, const uint8_t * const end, const uint8_t *data, cid_t * restrict cid, zhe_time_t tnow)
{
    uint8_t hdr, cid_byte;
    if (!zhe_unpack_byte(end, &data, &hdr)) {
        return 0;
    } else if (hdr & MZFLAG) {
        *cid = 1 + ((hdr >> 5) & 0x3);
    } else if (!zhe_unpack_byte(end, &data, &cid_byte)) {
        return 0;
    } else if (cid_byte > MAX_CID_T) {
        reset_peer(peeridx, tnow);
        return 0;
    } else {
        *cid = (cid_t)cid_byte;
    }
    if (*cid >= N_IN_CONDUITS) {
        reset_peer(peeridx, tnow);
        return 0;
    }
    return data;
}

static const uint8_t *handle_packet(peeridx_t * restrict peeridx, const uint8_t * const end, const uint8_t *data, zhe_time_t tnow)
{
    cid_t cid = 0;
    do {
        switch (*data & MKIND) {
            case MSCOUT:     data = handle_mscout(*peeridx, end, data, tnow); break;
            case MHELLO:     data = handle_mhello(*peeridx, end, data, tnow); break;
            case MOPEN:      data = handle_mopen(peeridx, end, data, tnow); break;
            case MACCEPT:    data = handle_maccept(peeridx, end, data, tnow); break;
            case MCLOSE:     data = handle_mclose(peeridx, end, data, tnow); break;
            case MDECLARE:   data = handle_mdeclare(*peeridx, end, data, cid, tnow); break;
            case MSDATA:     data = handle_msdata(*peeridx, end, data, cid, tnow); break;
            case MPING:      data = handle_mping(*peeridx, end, data, tnow); break;
            case MPONG:      data = handle_mpong(*peeridx, end, data); break;
            case MSYNCH:     data = handle_msynch(*peeridx, end, data, cid, tnow); break;
            case MACKNACK:   data = handle_macknack(*peeridx, end, data, cid, tnow); break;
            case MKEEPALIVE: data = handle_mkeepalive(peeridx, end, data, tnow); break;
            case MCONDUIT:   data = handle_mconduit(*peeridx, end, data, &cid, tnow); break;
            default:         data = 0; break;
        }
    } while (data < end && data != 0);
    return data;
}

int zhe_init(const struct zhe_config *config, struct zhe_transport *tp, zhe_time_t tnow)
{
    /* Is there a way to make the transport pluggable at run-time without dynamic allocation? I don't think so, not with the MTU so important ... */
    if (config->idlen == 0 || config->idlen > PEERID_SIZE) {
        return -1;
    }
    if (config->n_mconduit_dstaddrs != N_OUT_MCONDUITS) {
        /* these must match */
        return -1;
    }
    if (config->n_mcgroups_join > MAX_MULTICAST_GROUPS) {
        /* but you don't have to join MAX groups */
        return -1;
    }

    ownid_union.v_nonconst.len = (zhe_paysize_t)config->idlen;
    memcpy(ownid_union.v_nonconst.id, config->id, config->idlen);

    init_globals(tnow);
    transport = tp;
    scoutaddr = *config->scoutaddr;
#if MAX_MULTICAST_GROUPS > 0
    n_multicast_locators = (uint16_t)config->n_mcgroups_join;
    for (size_t i = 0; i < config->n_mcgroups_join; i++) {
        multicast_locators[i] = config->mcgroups_join[i];
    }
#endif
#if N_OUT_MCONDUITS > 0
    for (cid_t i = 0; i < N_OUT_MCONDUITS; i++) {
        out_mconduits[i].oc.addr = config->mconduit_dstaddrs[i];
    }
#endif
    return 0;
}

void zhe_start(zhe_time_t tnow)
{
#if MAX_PEERS == 0
    peers[0].tlease = tnow - SCOUT_INTERVAL;
#else
    tnextscout = tnow - SCOUT_INTERVAL;
#endif
}

static void maybe_send_scout(zhe_time_t tnow)
{
#if MAX_PEERS == 0
    if (peers[0].state == PEERST_UNKNOWN && (zhe_timediff_t)(tnow - peers[0].tlease) >= 0) {
        peers[0].tlease = tnow + SCOUT_INTERVAL;
        zhe_pack_mscout(&scoutaddr, tnow);
        zhe_pack_msend();
    }
    /* FIXME: send keepalive if connected to a broker? */
#else
    if ((zhe_timediff_t)(tnow - tnextscout) >= 0) {
        tnextscout = tnow + SCOUT_INTERVAL;
        zhe_pack_mscout(&scoutaddr, tnow);
        if (npeers > 0) {
            /* Scout messages are ignored by peers that have established a session with the source
               of the scout message, and then there is also the issue of potentially changing source
               addresses ... so we combine the scout with a keepalive if we know some peers */
            zhe_pack_mkeepalive(&scoutaddr, &ownid, tnow);
        }
        zhe_pack_msend();
    }
#endif
}

#if TRANSPORT_MODE == TRANSPORT_PACKET
int zhe_input(const void * restrict buf, size_t sz, const struct zhe_address *src, zhe_time_t tnow)
{
    char addrstr[TRANSPORT_ADDRSTRLEN];
    peeridx_t peeridx, free_peeridx = PEERIDX_INVALID;

    for (peeridx = 0; peeridx < MAX_PEERS_1; peeridx++) {
        if (transport->ops->addr_eq(src, &peers[peeridx].oc.addr)) {
            break;
        } else if (peers[peeridx].state == PEERST_UNKNOWN && free_peeridx == PEERIDX_INVALID) {
            free_peeridx = peeridx;
        }
    }

    if (ZTT(DEBUG)) {
        (void)transport->ops->addr2string(transport, addrstr, sizeof(addrstr), src);
    }

    if (peeridx == MAX_PEERS_1 && free_peeridx != PEERIDX_INVALID) {
        ZT(DEBUG, ("possible new peer %s @ %u", addrstr, free_peeridx));
        peeridx = free_peeridx;
        peers[peeridx].oc.addr = *src;
    }

    if (peeridx < MAX_PEERS_1) {
        ZT(DEBUG, ("handle message from %s @ %u", addrstr, peeridx));
        if (peers[peeridx].state == PEERST_ESTABLISHED) {
            peers[peeridx].tlease = tnow;
        }
        return (int)(handle_packet(&peeridx, buf + sz, buf, tnow) - (const uint8_t *)buf);
        /* note: peeridx need no longer be correct */
    } else {
        ZT(DEBUG, ("message from %s dropped: no available peeridx", addrstr));
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
        peeridx_t peeridx = 0;
        const uint8_t *datap = handle_packet(&peeridx, buf + sz, buf, tnow);
        /* note: peeridx need no longer be correct */
        int cons = (int)(datap - (const uint8_t *)buf);
        if (cons > 0 && peers[0].state == PEERST_ESTABLISHED) {
            /* any complete message is considered proof of liveliness of the broker once a connection has been established */
            peers[0].tlease = tnow;
        }
        return cons;
    }
}
#endif

static void maybe_send_msync_oc(struct out_conduit * const oc, zhe_time_t tnow)
{
    if (oc->seq != oc->seqbase && (zhe_timediff_t)(tnow - oc->tsynch) >= 0) {
        oc->tsynch = tnow + MSYNCH_INTERVAL;
        zhe_pack_msynch(&oc->addr, MSFLAG, oc->cid, oc->seqbase, oc_get_nsamples(oc), tnow);
        zhe_pack_msend();
    }
}

void zhe_flush(void)
{
    if (outp > 0) {
        zhe_pack_msend();
    }
}

void zhe_housekeeping(zhe_time_t tnow)
{
    /* FIXME: obviously, this is a big waste of CPU time if MAX_PEERS is biggish (but worst-case cost isn't affected) */
    for (peeridx_t i = 0; i < MAX_PEERS_1; i++) {
        switch(peers[i].state) {
            case PEERST_UNKNOWN:
                break;
            case PEERST_ESTABLISHED:
                if ((zhe_timediff_t)(tnow - peers[i].tlease) > peers[i].lease_dur) {
                    ZT(PEERDISC, ("lease expired on peer @ %u", i));
                    zhe_pack_mclose(&peers[i].oc.addr, 0, &ownid, tnow);
                    zhe_pack_msend();
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
                        ZT(PEERDISC, ("giving up on attempting to establish a session with peer @ %u", i));
                        reset_peer(i, tnow);
                    } else {
                        ZT(PEERDISC, ("retry opening a session with peer @ %u", i));
                        peers[i].state++;
                        peers[i].tlease = tnow;
                        zhe_pack_mopen(&peers[i].oc.addr, SEQNUM_LEN, &ownid, LEASE_DURATION, tnow);
                        zhe_pack_msend();
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
    maybe_send_scout(tnow);

    /* Flush any pending output if the latency budget has been exceeded */
#if LATENCY_BUDGET != 0 && LATENCY_BUDGET != LATENCY_BUDGET_INF
    if (outp > 0 && (zhe_timediff_t)(tnow - outdeadline) >= 0) {
        zhe_pack_msend();
    }
#endif
}
