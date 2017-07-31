/* -*- mode: c; c-basic-offset: 4; fill-column: 95; -*- */
#include <string.h>
#include <limits.h>
#include <assert.h>

#include "zeno.h"
#include "zeno-config-int.h"
#include "zeno-msg.h"
#include "pack.h"
#include "bitset.h"
#include "binheap.h"

void close_connection_and_scout(void); /* FIXME: not for p2p */
void reset_pubs_to_declare(void); /* FIXME: sort out close in p2p */
void reset_subs_to_declare(void); /* FIXME: sort out close in p2p */
void pre_panic_handler(void) {} /* FIXME: decide how to do this sort of thing */
ztime_t millis(void) { return 0; } /* FIXME: decide how to do this sort of thing */

void xrce_panic(uint16_t line, uint16_t code)
{
    pre_panic_handler();
    assert(0);
}

#define PANIC(code) do { xrce_panic(__LINE__, (code)); } while (0)
#define PANIC0 PANIC(0)

//////////////////////////////////////////////////////////////////////////////////////

#define WC_DRESULT_SIZE     8 /* worst-case result size: header, commitid, status, rid */
#define WC_DCOMMIT_SIZE     2 /* commit: header, commitid */
#define WC_DPUB_SIZE        6 /* pub: header, rid (not using properties) */
#define WC_DSUB_SIZE        7 /* sub: header, rid, mode (neither properties nor periodic modes) */

#define STATE_WAITINPUT      0
#define STATE_DRAININPUT     1
#define STATE_SCOUT          2
#define STATE_SCOUT_SENT     3
#define STATE_OPENING_MIN    4
#define STATE_OPENING_MAX  253
#define STATE_CONNECTED    254
#define STATE_OPERATIONAL  255

#if STATE_OPENING_MIN + OPEN_RETRIES > STATE_OPENING_MAX
#error "OPEN_RETRIES too large"
#endif

uint8_t zeno_state = 0;
ztime_t t_state_changed = 0;
#if TRANSPORT_MODE == TRANSPORT_STREAM
ztime_t t_progress;
#endif

const uint8_t peerid[] = { 'z', 'b', 'o', 't' };
const unsigned lease_dur = 300; /* 30 seconds */
uint8_t brokerid[PEERID_SIZE];
zpsize_t brokeridlen;
ztime_t brokerlease;

struct in_conduit {
    seq_t seq;                    /* next seq to be delivered */
    seq_t lseqpU;                 /* latest seq known to exist, plus UNIT */
    seq_t useq;                   /* next unreliable seq to be delivered */
};

struct out_conduit {
    zeno_address_t addr;          /* destination address */
    seq_t    seq;                 /* next seq to send */
    seq_t    seqbase;             /* latest seq ack'd + UNIT = first available */
    seq_t    useq;                /* next unreliable seq to send */
    uint16_t pos;                 /* next byte goes into rbuf[pos] */
    uint16_t spos;                /* starting pos of current sample for patching in size */
    uint16_t firstpos;            /* starting pos (actually, size) of oldest sample in window */
    ztime_t  tsynch;              /* next time to send out a SYNCH because of unack'd messages */
    uint8_t  nsamples;            /* number of samples in window */
    cid_t    cid;                 /* conduit id */
    uint8_t  rbuf[XMITW_BYTES];   /* reliable samples (or declarations); prepended by size (of type zmsize_t) */
};

struct peer {
    struct in_conduit ic[N_IN_CONDUITS];
    struct out_conduit oc;        /* conduit 0, unicast to this peer */
};

#if MAX_PEERS > 1 && (N_OUT_CONDUITS < 2 || N_IN_CONDUITS < 2)
#error "MAX_PEERS > 1 requires N_{IN,OUT}_CONDUITS > 1 for multicasting declarations"
#endif

#if N_OUT_CONDUITS > 1
#if MAX_PEERS == 0
#error "N_OUT_CONDUITS > 1 requires MAX_PEERS > 0"
#endif
struct out_mconduit {
    struct out_conduit oc;        /* same transmit window management as unicast */
    seq_t seqbase[MAX_PEERS];     /* oc.seqbase = min(seqbase) */
    peeridx_t seqbase_heap[MAX_PEERS]; /* heap indexing seqbase array */
};

/* out conduit 1 .. N-1 are mapped to out_mconduits[0 .. N-2] */
struct out_mconduit out_mconduits[N_OUT_CONDUITS - 1];
#endif

zeno_address_t scoutaddr;

/* FIXME: for packet-based we can do with a single input buffer; for stream-based we will probably need an input buffer per peer */
uint8_t inbuf[MTU];               /* where we buffer incoming packets */
zmsize_t inp;                     /* current position in inbuf while reading / size when processing */

/* Output buffer is a single packet; a single packet has a single destination and carries reliable data for at most one conduit */
uint8_t outbuf[MTU];              /* where we buffer next outgoing packet */
zmsize_t outp;                    /* current position in outbuf */
#define OUTSPOS_UNSET ((zmsize_t) -1)
zmsize_t outspos;                 /* OUTSPOS_UNSET or pos of last reliable SData/Declare header <=> outc == NULL*/
struct out_conduit *outc;         /* conduit over which reliable messages are carried in this packet, or NULL */
zeno_address_t *outdst;           /* destination address: &scoutaddr, &peer.oc.addr iff cid==0, &out_mconduits[cid-1].addr */
#if LATENCY_BUDGET != 0 && LATENCY_BUDGET != LATENCY_BUDGET_INF
#if N_OUT_CONDUITS > 1
#error "not yet for multiple conduits ..."
#endif
ztime_t outdeadline;              /* pack until full or this time passed */
#endif

/* FIXME: when deleting a peer: must clear output buffer if it is carrying data for that peer -- that is, if outdst points to peer.addr */

/* In client mode, we pretend the broker is peer 0.  It isn't really a peer, but the data structures
   for transmitting and receiving are the same. */
#define MAX_PEERS_1 (MAX_PEERS == 0 ? 1 : MAX_PEERS)
struct peer peers[MAX_PEERS_1];
peeridx_t npeers;

void oc_setup1(struct out_conduit *oc, uint8_t cid)
{
    memset(&oc->addr, 0, sizeof(oc->addr));
    oc->cid = cid;
    oc->seq = 0; /* FIXME: reset seq, or send SYNCH? the latter, I think */
    oc->useq = 0; /* FIXME: reset or SYNCH? -- SYNCH doesn't even cover at the moment */
    oc->seqbase = oc->seq; /* FIXME: I think */
    oc->pos = sizeof(zmsize_t);
    oc->spos = 0;
    oc->firstpos = 0;
    oc->nsamples = 0;
}

void oc_setup(void)
{
    for (peeridx_t i = 0; i < MAX_PEERS_1; i++) {
        oc_setup1(&peers[i].oc, 0);
    }
    for (cid_t i = 1; i < N_OUT_CONDUITS; i++) {
        oc_setup1(&out_mconduits[i-1].oc, i);
    }
    /* FIXME: keep outgoing packet builder as is? */
    outspos = OUTSPOS_UNSET;
    outp = 0;
    outc = NULL;
    outdst = NULL;
}

void ic_setup(void)
{
    for (peeridx_t i = 0; i < MAX_PEERS_1; i++) {
        for (cid_t j = 0; j < N_IN_CONDUITS; j++) {
            peers[i].ic[j].seq = 0;
            peers[i].ic[j].lseqpU = 0;
            peers[i].ic[j].useq = 0;
        }
    }
    /* FIXME: keep incoming packet buffer? I guess in packet mode that's ok, for streaming would probably need MAX_PEERS_1 */
    inp = 0;
}

uint16_t xmitw_pos_add(uint16_t p, uint16_t a)
{
    if ((p += a) >= XMITW_BYTES) {
        p -= XMITW_BYTES;
    }
    return p;
}

uint16_t xmitw_bytesused(const struct out_conduit *c)
{
    uint16_t res;
    assert(c->pos < XMITW_BYTES);
    assert(c->firstpos < XMITW_BYTES);
    res = c->pos - c->firstpos + (c->pos < c->firstpos ? XMITW_BYTES : 0);
    assert(res <= XMITW_BYTES);
    return res;
}

void msend(void)
{
    zmsize_t i;
    assert ((outspos == OUTSPOS_UNSET) == (outc == NULL));
    assert (outdst != NULL);
    if (outspos != OUTSPOS_UNSET) {
        if (xmitw_bytesused(outc) > 3 * XMITW_BYTES / 4) {
            outbuf[outspos] |= MSFLAG;
            outc->tsynch = millis() + MSYNCH_INTERVAL;
        }
    }
#if TRANSPORT_MODE == TRANSPORT_PACKET
    Serial.write(0xff);
    Serial.write(0x55);
#if MTU > 255
#error "PACKET mode currently has MTU limited to 255 because it writes the length as a single byte"
#endif
    assert(outp <= MTU);
    //Serial.write((uint8_t)outp);
#endif
    for (i = 0; i != outp; i++) {
        //Serial.write(outbuf[i]);
    }
#if TRANSPORT_MODE == TRANSPORT_PACKET
    Serial.println();
#endif
    outp = 0;
    outspos = OUTSPOS_UNSET;
    outc = NULL;
    outdst = NULL;
}

static void pack_check_avail(uint16_t n)
{
    assert(sizeof (outbuf) - outp >= n);
}

void pack_reserve(zeno_address_t *dst, zpsize_t cnt)
{
    /* make room by sending out current packet if requested number of bytes is no longer
       available, and also send out current packet if the destination changes */
    if (MTU - outp < cnt || (outdst != NULL && dst != outdst)) {
        /* we should never even try to generate a message that is too large for a packet */
        assert(outp != 0);
        msend();
    }
#if LATENCY_BUDGET != 0 && LATENCY_BUDGET != LATENCY_BUDGET_INF
    if (outp == 0) {
        /* packing deadline: note that no incomplete messages will ever be in the buffer when
           we check, because it is single-threaded and we always complete whatever message we
           start constructing */
        outdeadline = millis() + LATENCY_BUDGET;
    }
#endif
}

void pack1(uint8_t x)
{
    pack_check_avail(1);
    outbuf[outp++] = x;
}

void pack2(uint8_t x, uint8_t y)
{
    pack_check_avail(2);
    outbuf[outp++] = x;
    outbuf[outp++] = y;
}

void pack_u16(uint16_t x)
{
    pack2(x & 0xff, x >> 8);
}

void pack_vec(zpsize_t n, const uint8_t *buf)
{
    pack_vle16(n);
    pack_check_avail(n);
    while (n--) {
        outbuf[outp++] = *buf++;
    }
}

void oc_pack_copyrel(struct out_conduit *c, zmsize_t from)
{
    while (from < outp) {
        assert(c->pos != c->firstpos || c->nsamples == 0);
        c->rbuf[c->pos] = outbuf[from++];
        c->pos = xmitw_pos_add(c->pos, 1);
    }
}

zmsize_t oc_pack_payload_msgprep(seq_t *s, struct out_conduit *c, int relflag, zpsize_t sz)
{
    assert((c->spos + sizeof(zmsize_t)) % XMITW_BYTES == c->pos);
    pack_reserve_mconduit(&c->addr, c->cid, sz);
    if (!relflag) {
        *s = c->useq;
        c->useq += SEQNUM_UNIT;
    } else {
        *s = c->seq;
        c->seq += SEQNUM_UNIT;
        outspos = outp;
    }
    return outp;
}

void oc_pack_payload(struct out_conduit *c, int relflag, zpsize_t sz, const void *vdata)
{
    /* c->spos points to size byte, header byte immediately follows it, so reliability flag is
     easily located in the buffer */
    const uint8_t *data = (const uint8_t *)vdata;
    while (sz--) {
        outbuf[outp++] = *data;
        if (relflag) {
            assert(c->pos != c->firstpos);
            c->rbuf[c->pos] = *data;
            c->pos = xmitw_pos_add(c->pos, 1);
        }
        data++;
    }
}

void oc_pack_payload_done(struct out_conduit *c, int relflag)
{
    if (relflag) {
        zmsize_t len = (zmsize_t) (c->pos - c->spos + (c->pos < c->spos ? XMITW_BYTES : 0) - sizeof(zmsize_t));
        memcpy(&c->rbuf[c->spos], &len, sizeof(len));
        c->spos = c->pos;
        c->pos = xmitw_pos_add(c->pos, sizeof(zmsize_t));
        if (c->nsamples++ == 0) {
            /* first unack'd sample, schedule SYNCH */
            c->tsynch = millis() + MSYNCH_INTERVAL;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////

#define MAX_SUBS 10
#define MAX_PUBS 10

struct subtable {
    /* ID of the resource subscribed to (could also be a SID, actually) */
    rid_t rid;

    /* Minimum number of bytes that must be available in transmit window in the given conduit
       before calling, must include message overhead (for writing SDATA -- that is, no PRID
       present -- worst case is 9 bytes with a payload limit of 127 bytes and 32-bit RIDs) */
    struct out_conduit *oc;
    zpsize_t xmitneed;

    /* */
    void *arg;
    subhandler_t handler;
};
struct subtable subs[MAX_SUBS];

struct pubtable {
    struct out_conduit *oc;
    rid_t rid;
};
struct pubtable pubs[MAX_PUBS];

/* FIXME: should switch from publisher determines reliability to subscriber determines
   reliability, i.e., publisher reliability bit gets set to 
   (foldr or False $ map isReliableSub subs).  Keeping the reliability information
   separate from pubs has the advantage of saving quite a few bytes. */
uint8_t pubs_isrel[(MAX_PUBS + 7) / 8];
uint8_t rsubs[(MAX_PUBS + 7) / 8];

uint8_t precommit_rsubs[(MAX_PUBS + 7) / 8];
uint8_t precommit_result;
rid_t precommit_invalid_rid;

uint8_t precommit_curpkt_rsubs[(MAX_PUBS + 7) / 8];
uint8_t precommit_curpkt_result;
rid_t precommit_curpkt_invalid_rid;

void rsub_register(rid_t rid, uint8_t submode)
{
    uint8_t pubidx;
    assert(rid != 0);
    for (pubidx = 0; pubidx < MAX_PUBS; pubidx++) {
        if (pubs[pubidx].rid == rid) {
            break;
        }
    }
    if (submode == SUBMODE_PUSH && pubidx < MAX_PUBS) {
        bitset_set(precommit_curpkt_rsubs, pubidx);
    } else {
        if (precommit_curpkt_result == 0) {
            precommit_curpkt_invalid_rid = rid;
        }
        if (submode != SUBMODE_PUSH) {
            precommit_curpkt_result |= 1;
        }
        if (pubidx >= MAX_PUBS) {
            precommit_curpkt_result |= 2;
        }
    }
}

uint8_t rsub_precommit(rid_t *err_rid)
{
    assert (precommit_curpkt_result == 0);
    if (precommit_result == 0) {
        return 0;
    } else {
        size_t i;
        *err_rid = precommit_invalid_rid;
        for (i = 0; i < sizeof (precommit_rsubs); i++) {
            precommit_rsubs[i] = 0;
        }
        precommit_result = 0;
        precommit_invalid_rid = 0;
        return precommit_result;
    }
}

void rsub_commit(void)
{
    size_t i;
    assert (precommit_result == 0);
    for (i = 0; i < sizeof (rsubs); i++)
        rsubs[i] |= precommit_rsubs[i];
    for (i = 0; i < sizeof (precommit_rsubs); i++) {
        precommit_rsubs[i] = 0;
    }
    precommit_result = 0;
    precommit_invalid_rid = 0;
}

int rsub_exists(uint8_t pubidx)
{
    assert(pubidx < MAX_PUBS);
    return bitset_test(rsubs, pubidx);
}

void rsub_precommit_curpkt_abort(void)
{
    size_t i;
    for (i = 0; i < sizeof (precommit_rsubs); i++) {
        precommit_curpkt_rsubs[i] = 0;
    }
    precommit_curpkt_invalid_rid = 0;
    precommit_curpkt_result = 0;
}

void rsub_precommit_curpkt_done(void)
{
    size_t i;
    for (i = 0; i < sizeof (precommit_rsubs); i++) {
        precommit_rsubs[i] |= precommit_curpkt_rsubs[i];
    }
    if (precommit_invalid_rid == 0) {
        precommit_invalid_rid = precommit_curpkt_invalid_rid;
    }
    precommit_result |= precommit_curpkt_result;
    rsub_precommit_curpkt_abort();
}

void rsub_clear(void)
{
    size_t i;
    for (i = 0; i < sizeof (rsubs); i++) {
        rsubs[i] = 0;
    }
    for (i = 0; i < sizeof (precommit_rsubs); i++) {
        precommit_rsubs[i] = 0;
    }
    precommit_result = 0;
    precommit_invalid_rid = 0;
    rsub_precommit_curpkt_abort();
}

///////////////////////////////////////////////////////////////////////////////////////////

void xrce_setup(void)
{
    oc_setup();
    ic_setup();
}

#if TRANSPORT_MODE == TRANSPORT_PACKET
int read_serial(void)
{
    static zmsize_t serst = 0;
    if (Serial.available()) {
        uint8_t c = Serial.read();
        switch (serst) {
            case 0:
                serst = (c == 0xff) ? 255 : 0;
                break;
            case 255:
                serst = (c == 0x55) ? 254 : 0;
                break;
            case 254:
                if (c == 0 || c > MTU) {
                    serst = 0; /* ERROR */
                } else {
                    serst = c;
                    inp = 0;
                }
                break;
            default:
                inbuf[inp++] = c;
                if (--serst == 0) {
                    return 1;
                }
                break;
        }
    }
    return 0;
}
#endif

int unpack_skip(zmsize_t *sz, const uint8_t **data, zmsize_t n)
{
    if (*sz < n) {
        return 0;
    }
    *sz -= n;
    *data += n;
    return 1;
}

int unpack_byte(zmsize_t *sz, const uint8_t **data, zmsize_t *u)
{
    if (*sz < 1) {
        return 0;
    }
    if (u) {
        *u = **data;
    }
    *sz -= 1;
    *data += 1;
    return 1;
}

int unpack_u16(zmsize_t *sz, const uint8_t **data, uint16_t *u)
{
    if (*sz < 2) {
        return 0;
    }
    if (u) {
        *u = (*data)[0] | ((uint16_t)((*data)[1]) << 8);
    }
    *sz -= 2;
    *data += 2;
    return 1;
}

int unpack_vle16(zmsize_t *sz, const uint8_t **data, uint16_t *u)
{
    typeof(*u) n;
    uint8_t shift = 7;
    uint8_t x;
    if (*sz == 0) {
        return 0;
    }
    x = **data; (*data)++; (*sz)--;
    n = x & 0x7f;
    while (x & 0x80) {
        if (*sz == 0) {
            return 0;
        }
        x = **data; (*data)++; (*sz)--;
        if (shift < CHAR_BIT * sizeof(*u)) { /* else behaviour is undefined */
            n |= ((typeof(n))(x & 0x7f)) << shift;
            shift += 7;
        }
    }
    if (u) *u = n;
    return 1;
}

int unpack_vle32(zmsize_t *sz, const uint8_t **data, uint32_t *u)
{
    typeof(*u) n;
    uint8_t shift = 7;
    uint8_t x;
    if (*sz == 0) { return 0; }
    x = **data; (*data)++; (*sz)--;
    n = x & 0x7f;
    while (x & 0x80) {
        if (*sz == 0) { return 0; }
        x = **data; (*data)++; (*sz)--;
        if (shift < CHAR_BIT * sizeof(*u)) { /* else behaviour is undefined */
            n |= ((typeof(n))(x & 0x7f)) << shift;
            shift += 7;
        }
    }
    if (u) *u = n;
    return 1;
}

int unpack_vle64(zmsize_t *sz, const uint8_t **data, uint64_t *u)
{
    typeof(*u) n;
    uint8_t shift = 7;
    uint8_t x;
    if (*sz == 0) { return 0; }
    x = **data; (*data)++; (*sz)--;
    n = x & 0x7f;
    while (x & 0x80) {
        if (*sz == 0) { return 0; }
        x = **data; (*data)++; (*sz)--;
        if (shift < CHAR_BIT * sizeof(*u)) { /* else behaviour is undefined */
            n |= ((typeof(n))(x & 0x7f)) << shift;
            shift += 7;
        }
    }
    if (u) *u = n;
    return 1;
}

int unpack_seq(zmsize_t *sz, const uint8_t **data, seq_t *u)
{
    if (!unpack_vle16(sz, data, u)) {
        return 0;
    }
    *u <<= SEQNUM_SHIFT;
    return 1;
}

const uint8_t *skip_validated_vle(const uint8_t *data)
{
    while (*data & 0x80) {
        data++;
    }
    return data;
}

int unpack_rid(zmsize_t *sz, const uint8_t **data, rid_t *u)
{
    return SUFFIX_WITH_SIZE(unpack_vle, RID_T_SIZE)(sz, data, u);
}

int unpack_vec(zmsize_t *sz, const uint8_t **data, size_t lim, zpsize_t *u, uint8_t *v)
{
    zpsize_t i;
    if (!unpack_vle16(sz, data, u)) { return 0; }
    if (*sz < *u) { return 0; }
    if (*u < lim) { lim = *u; }
    for (i = 0; i < lim; i++) {
        v[i] = **data;
        (*data)++;
    }
    (*data) += *u - lim;
    (*sz) -= *u;
    return 1;
}

int unpack_locs(zmsize_t *sz, const uint8_t **data)
{
    uint16_t n;
    zpsize_t dummy;
    if (!unpack_vle16(sz, data, &n)) {
        return 0;
    }
    while (n--) {
        if (!unpack_vec(sz, data, 0, &dummy, NULL)) {
            return 0;
        }
    }
    return 1;
}

int unpack_props(zmsize_t *sz, const uint8_t **data)
{
    uint16_t n;
    zpsize_t dummy;
    if (!unpack_vle16(sz, data, &n)) {
        return 0;
    }
    while (n--) {
        if (!unpack_vec(sz, data, 0, &dummy, NULL) || !unpack_vec(sz, data, 0, &dummy, NULL)) {
            return 0;
        }
    }
    return 1;
}

zmsize_t handle_mscout(peeridx_t peeridx, zmsize_t sz, const uint8_t *data)
{
    const uint8_t *data0 = data;
    if (!unpack_skip(&sz, &data, 3)) {
        return 0;
    }
    return (zmsize_t)(data - data0);
}

zmsize_t handle_mhello(peeridx_t peeridx, zmsize_t sz, const uint8_t *data)
{
    const uint8_t *data0 = data;
    uint8_t hdr;
    uint32_t mask;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_vle32(&sz, &data, &mask) ||
        !unpack_locs(&sz, &data) ||
        !unpack_props(&sz, &data)) {
        return 0;
    }
    if (zeno_state == STATE_SCOUT_SENT && (mask & MSCOUT_BROKER)) {
        zeno_state = STATE_OPENING_MIN;
        t_state_changed = millis();
    }
    return (zmsize_t)(data - data0);
}

zmsize_t handle_mopen(peeridx_t peeridx, zmsize_t sz, const uint8_t *data)
{
    const uint8_t *data0 = data;
    zpsize_t dummy;
    if (!unpack_skip(&sz, &data, 3) ||
        !unpack_vec(&sz, &data, 0, &dummy, NULL) ||
        !unpack_skip(&sz, &data, 1) ||
        !unpack_vle32(&sz, &data, NULL) ||
        !unpack_vec(&sz, &data, 0, &dummy, NULL)) {
        return 0;
    }
    return (zmsize_t)(data - data0);
}

zmsize_t handle_maccept(peeridx_t peeridx, zmsize_t sz, const uint8_t *data)
{
    const uint8_t *data0 = data;
    zpsize_t idlen;
    uint8_t id[PEERID_SIZE];
    uint32_t lease;
    zpsize_t dummy;
    int forme;
    if (!unpack_skip(&sz, &data, 1) ||
        !unpack_vec(&sz, &data, sizeof (id), &idlen, id)) {
        return 0;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        close_connection_and_scout();
        return 0;
    }
    forme = (idlen == sizeof (peerid) && memcmp(id, peerid, sizeof (peerid)) == 0);
    if (!unpack_vec(&sz, &data, sizeof (id), &idlen, id) ||
        !unpack_vle32(&sz, &data, &lease) ||
        !unpack_vec(&sz, &data, 0, &dummy, NULL)) {
        return 0;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        close_connection_and_scout();
        return 0;
    }
    if (forme && zeno_state >= STATE_OPENING_MIN && zeno_state <= STATE_OPENING_MAX) {
        brokeridlen = idlen;
        memcpy(brokerid, id, sizeof(id));
        brokerlease = lease;
        zeno_state = STATE_CONNECTED;
        t_state_changed = millis();
    }
    return (zmsize_t)(data - data0);
}

void close_connection_and_scout(void)
{
    ic_setup();
    oc_setup();
    rsub_clear();
    reset_pubs_to_declare();
    reset_subs_to_declare();
    zeno_state = STATE_SCOUT;
    t_state_changed = millis();
}

zmsize_t handle_mclose(peeridx_t peeridx, zmsize_t sz, const uint8_t *data)
{
    /* FIXME: should check id of sender */
    zpsize_t dummy;
    uint8_t reason;
    if (!unpack_skip(&sz, &data, 1) ||
        !unpack_vec(&sz, &data, 0, &dummy, NULL) ||
        !unpack_byte(&sz, &data, &reason)) {
        return 0;
    }
    close_connection_and_scout();
    return 0;
}

zmsize_t handle_dresource(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, int interpret)
{
    /* No use for a broker declaring its resources, but we don't bug out over it */
    const uint8_t *data0 = data;
    uint8_t hdr;
    zpsize_t dummy;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_rid(&sz, &data, NULL) ||
        !unpack_vec(&sz, &data, 0, &dummy, NULL)) {
        return 0;
    }
    if ((hdr & DPFLAG) && !unpack_props(&sz, &data)) {
        return 0;
    }
    return (zmsize_t)(data - data0);
}

zmsize_t handle_dpub(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, int interpret)
{
    /* No use for a broker declaring its publications, but we don't bug out over it */
    const uint8_t *data0 = data;
    uint8_t hdr;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_rid(&sz, &data, NULL)) {
        return 0;
    }
    if ((hdr & DPFLAG) && !unpack_props(&sz, &data)) {
        return 0;
    }
    return (zmsize_t)(data - data0);
}

zmsize_t handle_dsub(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, int interpret)
{
    const uint8_t *data0 = data;
    rid_t rid;
    uint8_t hdr, mode;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_rid(&sz, &data, &rid) ||
        !unpack_byte(&sz, &data, &mode)) {
        return 0;
    }
    if (mode == 0 || mode > SUBMODE_MAX) {
        return 0;
    }
    if (mode == SUBMODE_PERIODPULL || mode == SUBMODE_PERIODPUSH) {
        if (!unpack_vle32(&sz, &data, NULL) ||
            !unpack_vle32(&sz, &data, NULL))
            return 0;
    }
    if ((hdr & DPFLAG) && !unpack_props(&sz, &data)) {
        return 0;
    }
    if (interpret) {
        rsub_register(rid, mode);
    }
    return (zmsize_t)(data - data0);
}

zmsize_t handle_dselection(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, int interpret)
{
    /* FIXME: support selections?  I don't think there's a need. */
    const uint8_t *data0 = data;
    rid_t sid;
    uint8_t hdr;
    zpsize_t dummy;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_rid(&sz, &data, &sid) ||
        !unpack_vec(&sz, &data, 0, &dummy, NULL)) {
        return 0;
    }
    if ((hdr & DPFLAG) && !unpack_props(&sz, &data)) {
        return 0;
    }
    if (interpret) {
        if (precommit_curpkt_result == 0) {
            precommit_curpkt_invalid_rid = sid;
        }
        precommit_curpkt_result |= 4;
    }
    return (zmsize_t)(data - data0);
}

zmsize_t handle_dbindid(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, int interpret)
{
    /* FIXME: support bindings?  I don't think there's a need. */
    const uint8_t *data0 = data;
    rid_t sid;
    if (!unpack_skip(&sz, &data, 1) ||
        !unpack_rid(&sz, &data, &sid) ||
        !unpack_rid(&sz, &data, NULL)) {
        return 0;
    }
    if (interpret) {
        if (precommit_curpkt_result == 0) {
            precommit_curpkt_invalid_rid = sid;
        }
        precommit_curpkt_result |= 8;
    }
    return (zmsize_t)(data - data0);
}

zmsize_t handle_dcommit(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, int interpret)
{
    const uint8_t *data0 = data;
    uint8_t commitid;
    uint8_t res;
    rid_t err_rid;
    if (!unpack_skip(&sz, &data, 1) ||
        !unpack_byte(&sz, &data, &commitid)) {
        return 0;
    }
    if (interpret) {
        /* If we can't reserve space in the transmit window, pretend we never received the
           DECLARE message and abandon the rest of the packet.  Eventually we'll get a
           retransmit and retry.  Use worst-case size for result */
#if MAX_PEERS <= 1
        struct out_conduit * const oc = &peers[0].oc;
#else
        struct out_conduit * const oc = &out_mconduits[0].oc;
#endif
        /* FIXME: need to make sure no peer present is ok, too */
        if (!oc_pack_mdeclare(oc, 1, WC_DRESULT_SIZE)) {
            return 0;
        } else {
            rsub_precommit_curpkt_done();
            if ((res = rsub_precommit(&err_rid)) == 0) {
                rsub_commit();
            }
            pack_dresult(commitid, res, err_rid);
            oc_pack_mdeclare_done(oc);
            msend();
        }
    }
    return (zmsize_t)(data - data0);
}

zmsize_t handle_dresult(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, int interpret)
{
    const uint8_t *data0 = data;
    uint8_t commitid, status;
    rid_t rid;
    if (!unpack_skip(&sz, &data, 1) ||
        !unpack_byte(&sz, &data, &commitid) ||
        !unpack_byte(&sz, &data, &status)) {
        return 0;
    }
    if (status && !unpack_rid(&sz, &data, &rid)) {
        return 0;
    }
    if (interpret && status != 0) {
        /* Don't know what to do when the broker refuses my declarations - although I guess it
           would make some sense to close the connection and try again.  But even if that is
           the right thing to do, don't do that just yet, because it shouldn't fail.

           Also note that we're not looking at the commit id at all, I am not sure yet what
           problems that may cause ... */
        PANIC(status);
    }
    return (zmsize_t)(data - data0);
}

zmsize_t handle_ddeleteres(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, int interpret)
{
    const uint8_t *data0 = data;
    uint8_t hdr;
    zpsize_t dummy;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_vec(&sz, &data, 0, &dummy, NULL)) {
        return 0;
    }
    if ((hdr & DPFLAG) && !unpack_props(&sz, &data)) {
        return 0;
    }
    if (interpret) {
        precommit_curpkt_result |= 16;
    }
    return (zmsize_t)(data - data0);
}

int seq_lt(seq_t a, seq_t b)
{
    return (sseq_t) (a - b) < 0;
}

int seq_le(seq_t a, seq_t b)
{
    return (sseq_t) (a - b) <= 0;
}

int ic_may_deliver_seq(struct in_conduit *ic, uint8_t hdr, seq_t seq)
{
    if (hdr & MRFLAG) {
        return (ic->seq == seq);
    } else {
        return seq_le(ic->useq, seq);
    }
}

void ic_update_seq (struct in_conduit *ic, uint8_t hdr, seq_t seq)
{
    assert(ic_may_deliver_seq(ic, hdr, seq));
    if (hdr & MRFLAG) {
        assert(seq_lt(ic->seq, ic->lseqpU));
        ic->seq = seq + SEQNUM_UNIT;
    } else {
        assert(seq_le(ic->seq, ic->lseqpU));
        ic->useq = seq + SEQNUM_UNIT;
    }
}

void acknack_if_needed(peeridx_t peeridx, cid_t cid, int wantsack)
{
    seq_t cnt = (peers[peeridx].ic[cid].lseqpU - peers[peeridx].ic[cid].seq) >> SEQNUM_SHIFT;
    uint32_t mask;
    assert(seq_le(peers[peeridx].ic[cid].seq, peers[peeridx].ic[cid].lseqpU));
    if (cnt == 0) {
        mask = 0;
    } else {
        mask = ~(uint32_t)0;
        if (cnt < 32) { /* avoid undefined behaviour */
            mask >>= 32 - cnt;
        }
    }
    if (mask != 0 || wantsack) {
        /* ACK goes out over unicast path; the conduit used for sending it doesn't have
           much to do with it other than administrative stuff */
        pack_macknack(&peers[peeridx].oc.addr, cid, peers[peeridx].ic[cid].seq, mask);
        msend();
    }
}

zmsize_t handle_mdeclare(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, uint8_t cid)
{
    /* Note 1: not buffering data received out-of-order, so but need to decode everything to
       find next message, which we may "have to" interpret - we don't really "have to", but to
       simply pretend we never received it is a bit rough.

       Note 2: a commit requires us to send something in reply, but we may be unable to because
       of a full transmit window in the reliable channel.  The elegant option is to suspend
       further input processing, until space is available again, the inelegant one is to verify
       we have space beforehand, and pretend we never received the DECLARE if we don't. */
    const uint8_t *data0 = data;
    uint8_t hdr, ncons;
    uint16_t ndecls;
    seq_t seq;
    int intp;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_seq(&sz, &data, &seq) ||
        !unpack_vle16(&sz, &data, &ndecls)) {
        return 0;
    }
    if (seq_le(peers[peeridx].ic[cid].lseqpU, seq)) {
        peers[peeridx].ic[cid].lseqpU = seq + SEQNUM_UNIT;
    }
    intp = (zeno_state >= STATE_CONNECTED && ic_may_deliver_seq(&peers[peeridx].ic[cid], hdr, seq));
    if (ndecls > 0) {
        do {
            uint8_t kind = *data & DKIND;
            switch (kind) {
                case DRESOURCE:  ncons = handle_dresource(peeridx, sz, data, intp); break;
                case DPUB:       ncons = handle_dpub(peeridx, sz, data, intp); break;
                case DSUB:       ncons = handle_dsub(peeridx, sz, data, intp); break;
                case DSELECTION: ncons = handle_dselection(peeridx, sz, data, intp); break;
                case DBINDID:    ncons = handle_dbindid(peeridx, sz, data, intp); break;
                case DCOMMIT:    ncons = handle_dcommit(peeridx, sz, data, intp); break;
                case DRESULT:    ncons = handle_dresult(peeridx, sz, data, intp); break;
                case DDELETERES: ncons = handle_ddeleteres(peeridx, sz, data, intp); break;
                default:         ncons = 0; break;
            }
            sz -= ncons;
            data += ncons;
            if (ncons > 0) {
                --ndecls;
            }
        } while (sz > 0 && ncons > 0);
        if (ndecls != 0) {
            rsub_precommit_curpkt_abort();
            return 0;
        }
    }
    if (intp) {
        /* Merge uncommitted declaration state resulting from this DECLARE message into
           uncommitted state accumulator, as we have now completely and successfully processed
           this message.  */
        rsub_precommit_curpkt_done();
        (void)ic_update_seq(&peers[peeridx].ic[cid], hdr, seq);
    }
    /* FIXME: or should it only do this if S flag set? */
    acknack_if_needed(peeridx, cid, hdr & MSFLAG);
    return (zmsize_t)(data - data0);
}

zmsize_t handle_msynch(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, uint8_t cid)
{
    const uint8_t *data0 = data;
    uint8_t hdr;
    seq_t cnt;
    seq_t seqbase;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_seq(&sz, &data, &seqbase) ||
        !unpack_seq(&sz, &data, &cnt)) {
        return 0;
    }
    peers[peeridx].ic[cid].seq = seqbase;
    peers[peeridx].ic[cid].lseqpU = seqbase + (cnt << SEQNUM_SHIFT);
    acknack_if_needed(peeridx, cid, hdr & MSFLAG);
    return (zmsize_t)(data - data0);
}

zmsize_t handle_msdata(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, uint8_t cid)
{
    const uint8_t *data0 = data;
    uint8_t hdr;
    const uint8_t *pay;
    zpsize_t paysz;
    seq_t seq;
    rid_t rid, prid;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_seq(&sz, &data, &seq) ||
        !unpack_rid(&sz, &data, &rid)) {
        return 0;
    }
    if (!(hdr & MPFLAG)) {
        prid = rid;
    } else if (!unpack_rid(&sz, &data, &prid)) {
        return 0;
    }
    /* Attempt to "extract" payload -- we don't extract it but leave it in place to save memory
       and time.  If it is fully present, datacopy will still point to the payload size and all
       we need to redo is skip the VLE encoded length in what we know to be a valid buffer */
    pay = data;
    if (!unpack_vec(&sz, &data, 0, &paysz, NULL)) {
        return 0;
    }
    pay = skip_validated_vle(pay);
    if ((hdr & MRFLAG) && seq_le(peers[peeridx].ic[cid].lseqpU, seq)) {
        peers[peeridx].ic[cid].lseqpU = seq + SEQNUM_UNIT;
    }
    if (ic_may_deliver_seq(&peers[peeridx].ic[cid], hdr, seq)) {
        /* Call the handler synchronously */
        uint8_t k;
        for (k = 0; k < MAX_SUBS; k++) {
            if (subs[k].rid == prid) {
                break;
            }
        }
        if (k == MAX_SUBS) {
            /* Not subscribed, ignore */
            ic_update_seq(&peers[peeridx].ic[cid], hdr, seq);
        } else if (XMITW_BYTES - xmitw_bytesused(subs[k].oc) < subs[k].xmitneed) {
            /* Not enough space available -- do note that "xmitneed" had better include
               overhead! */
            PANIC0; /* FIXME: panicking where we could calmly continue, just so I know it when it happens */
        } else {
            subs[k].handler(prid, paysz, pay, subs[k].arg);
            ic_update_seq(&peers[peeridx].ic[cid], hdr, seq);
        }
    }
    if (hdr & MRFLAG) {
        /* FIXME: or should it only do this if S flag set? */
        acknack_if_needed(peeridx, cid, hdr & MSFLAG);
    }
    return (zmsize_t)(data - data0);
}

void remove_acked_messages(struct out_conduit *c, seq_t seq)
{
    if (seq_lt(c->seq, seq)) {
        /* Broker is ACKing samples we haven't even sent yet, use the opportunity to drain the
           transmit window */
        seq = c->seq;
    }

    if(seq_lt(c->seqbase, seq)) {
        /* Acking some samples, drop everything from seqbase up to but not including seq */
#ifndef NDEBUG
        seq_t cnt = (seq - c->seqbase) >> SEQNUM_SHIFT;
#endif
        while (c->seqbase != seq) {
            zmsize_t len;
            assert(c->nsamples > 0);
            assert(cnt > 0);
#ifndef NDEBUG
            cnt--;
#endif
            c->seqbase += SEQNUM_UNIT;
            c->nsamples--;
            memcpy(&len, &c->rbuf[c->firstpos], sizeof(len));
            c->firstpos = xmitw_pos_add(c->firstpos, len + sizeof(zmsize_t));
        }
        assert(cnt == 0);
        assert(((c->firstpos + sizeof(zmsize_t)) % XMITW_BYTES == c->pos) == (c->nsamples == 0));
        assert((c->seqbase == c->seq) == (c->nsamples == 0));
    }
}

void oc_pack_msynch(struct out_conduit *c, uint8_t sflag)
{
    seq_t cnt = (c->seq - c->seqbase) >> SEQNUM_SHIFT;
    pack_msynch(&c->addr, sflag, c->cid, c->seqbase, cnt);
}

seq_t update_mconduit_seqbase(struct out_mconduit *mc, peeridx_t peeridx, seq_t seq)
{
    if (seq_le(seq, mc->seqbase[peeridx])) {
        /* ACK doesn't introduce new information, short-circuit */
        return mc->oc.seqbase;
    } else {
        mc->seqbase[peeridx] = seq;
        return minseqheap_increased_key(peeridx, npeers, mc->seqbase_heap, mc->seqbase);
    }
}

zmsize_t handle_macknack(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, uint8_t cid)
{
    const uint8_t *data0 = data;
    struct out_conduit * const c = (cid == 0) ? &peers[peeridx].oc : &out_mconduits[cid-1].oc;
    seq_t seq, seq_ack;
    uint8_t hdr;
    uint32_t mask;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_seq(&sz, &data, &seq)) {
        return 0;
    }

    if (cid == 0) {
        seq_ack = seq;
    } else {
        seq_ack = update_mconduit_seqbase(&out_mconduits[cid-1], peeridx, seq);
    }
    remove_acked_messages(c, seq_ack);

    if (!(hdr & MMFLAG)) {
        /* Pure ACK - no need to do anything else */
        return (zmsize_t)(data - data0);
    } else if (!unpack_vle32(&sz, &data, &mask)) {
        return 0;
    } else if (seq_lt(seq, c->seqbase) || seq_le(c->seq, seq)) {
        /* If the broker ACKs stuff we have dropped already, or if it NACKs stuff we have not
           even sent yet, send a SYNCH without the S flag (i.e., let the broker decide what to
           do with it) */
        oc_pack_msynch(c, 0);
    } else {
        /* Retransmits can always be performed because they do not require buffering new
           messages, all we need to do is push out the buffered messages.  We want the S bit
           set on the last of the retransmitted ones, so we "clear" outspos and then set it
           before pushing out that last sample. */
        uint16_t p;
        zmsize_t sz, outspos_tmp = OUTSPOS_UNSET;
        assert (seq == c->seqbase);
        /* Make the retransmit request for message SEQ implied by the use of an ACKNACK
           explicit in the mask (which means we won't retransmit SEQ + 32). */
        mask = (mask << 1) | 1;
        /* Do not set the S bit on anything that happens to currently be in the output buffer,
           as we by now know that we will retransmit at least one message */
        outspos = OUTSPOS_UNSET;
        /* Note: transmit window is formatted as SZ1 [MSG2 x SZ1] SZ2 [MSG2 x SZ2], &c,
           wrapping around at XMITW_BYTES.  */
        memcpy(&sz, &c->rbuf[c->firstpos], sizeof(sz));
        p = xmitw_pos_add(c->firstpos, sizeof(zmsize_t));
        while (mask && seq_lt(seq, c->seq)) {
            if ((mask & 1) == 0) {
                p = xmitw_pos_add(p, sz);
            } else {
                pack_reserve(&c->addr, sz);
                outspos_tmp = outp;
                while (sz--) {
                    pack1(c->rbuf[p]);
                    p = xmitw_pos_add(p, 1);
                }
            }
            mask >>= 1;
            seq += SEQNUM_UNIT;
            memcpy(&sz, &c->rbuf[p], sizeof(sz));
            p = xmitw_pos_add(p, sizeof(zmsize_t));
        }
        /* Asserting that seq <= c->seq is a somewhat nonsensical considering the guards for
           this block and the loop condition, but it clarifies the second assertion: if we got
           all the way to the most recent sample, then P should point to the first free
           position in the transmit window, a.k.a. c->pos.  */
        assert(seq_le(seq, c->seq));
        assert(seq != c->seq || p == c->pos);
        /* Since we must have sent at least one message, outspos_tmp must have been set.  Set
           the S flag in that final message.  */
        assert(outspos_tmp != OUTSPOS_UNSET);
        outbuf[outspos_tmp] |= MSFLAG;
        c->tsynch = millis() + MSYNCH_INTERVAL;
        msend();
    }
    return (zmsize_t)(data - data0);
}

zmsize_t handle_mping(peeridx_t peeridx, zmsize_t sz, const uint8_t *data)
{
    const uint8_t *data0 = data;
    uint16_t hash;
    if (!unpack_skip(&sz, &data, 1) ||
        !unpack_u16(&sz, &data, &hash)) {
        return 0;
    }
    pack_mpong(&peers[peeridx].oc.addr, hash);
    msend();
    return (zmsize_t)(data - data0);
}

zmsize_t handle_mpong(peeridx_t peeridx, zmsize_t sz, const uint8_t *data)
{
    const uint8_t *data0 = data;
    if (!unpack_skip(&sz, &data, 3)) {
        return 0;
    }
    return (zmsize_t)(data - data0);
}

zmsize_t handle_mkeepalive(peeridx_t peeridx, zmsize_t sz, const uint8_t *data)
{
    const uint8_t *data0 = data;
    zpsize_t idlen;
    uint8_t id[PEERID_SIZE];
    int frombroker;
    if (!unpack_skip(&sz, &data, 1) ||
        !unpack_vec(&sz, &data, sizeof(id), &idlen, id)) {
        return 0;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        close_connection_and_scout();
        return 0;
    }
    frombroker = (idlen == brokeridlen && memcmp(id, brokerid, idlen) == 0);
    if (zeno_state >= STATE_CONNECTED && frombroker) {
        /* Nothing to do really, because we consider any message coming in as a proof of
           liveliness of the broker. */
    }
    return (zmsize_t)(data - data0);
}

zmsize_t handle_mconduit(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, uint8_t *cid)
{
    /* FIXME: should check id of sender */
    uint8_t hdr;
    if (!unpack_byte(&sz, &data, &hdr)) {
        return 0;
    } else if (hdr & MZFLAG) {
        *cid = (hdr >> 4) & 0x3;
    } else if (!unpack_byte(&sz, &data, cid)) {
        return 0;
    }
    if (*cid >= N_IN_CONDUITS) {
        close_connection_and_scout();
        return 0;
    }
    return 1;
}

uint8_t *handle_packet(peeridx_t peeridx, zmsize_t sz, const uint8_t *data)
{
    zmsize_t ncons;
    uint8_t cid = 0;
    do {
        uint8_t kind = *data & MKIND;
        switch (kind) {
            case MSCOUT:     ncons = handle_mscout(peeridx, sz, data); break;
            case MHELLO:     ncons = handle_mhello(peeridx, sz, data); break;
            case MOPEN:      ncons = handle_mopen(peeridx, sz, data); break;
            case MACCEPT:    ncons = handle_maccept(peeridx, sz, data); break;
            case MCLOSE:     ncons = handle_mclose(peeridx, sz, data); break;
            case MDECLARE:   ncons = handle_mdeclare(peeridx, sz, data, cid); break;
            case MSDATA:     ncons = handle_msdata(peeridx, sz, data, cid); break;
            case MPING:      ncons = handle_mping(peeridx, sz, data); break;
            case MPONG:      ncons = handle_mpong(peeridx, sz, data); break;
            case MSYNCH:     ncons = handle_msynch(peeridx, sz, data, cid); break;
            case MACKNACK:   ncons = handle_macknack(peeridx, sz, data, cid); break;
            case MKEEPALIVE: ncons = handle_mkeepalive(peeridx, sz, data); break;
            case MCONDUIT:   ncons = handle_mconduit(peeridx, sz, data, &cid); break;
            default:         ncons = 0; break;
        }
        sz -= ncons;
        data += ncons;
    } while (ncons > 0 && sz > 0);
    return (uint8_t *)data;
}

/////////////////////////////////////////////////////////////////////////////////////

/* Not currently implementing cancelling subscriptions or stopping publishing, but once that is
   included, should clear pubs_to_declare if it so happens that the publication hasn't been
   declared yet by the time it is deleted */
uint8_t pubs_to_declare[(MAX_PUBS + 7) / 8];
uint8_t subs_to_declare[(MAX_SUBS + 7) / 8];
uint8_t must_commit;
uint8_t gcommitid;

void reset_pubs_to_declare(void)
{
    uint8_t i;
    for (i = 0; i < sizeof(pubs_to_declare); i++) {
        pubs_to_declare[i] = 0;
    }
    for (i = 0; i < MAX_PUBS; i++) {
        if (pubs[i].rid != 0) {
            bitset_set(pubs_to_declare, i);
        }
    }
}

void reset_subs_to_declare(void)
{
    uint8_t i;
    for (i = 0; i < sizeof(subs_to_declare); i++) {
        subs_to_declare[i] = 0;
    }
    for (i = 0; i < MAX_SUBS; i++) {
        if (subs[i].rid != 0) {
            bitset_set(subs_to_declare, i);
        }
    }
}

pubidx_t publish(rid_t rid, int reliable)
{
    /* We will be publishing rid, dynamically allocating a "pubidx" for it and scheduling a
       DECLARE message that informs the broker of this.  By scheduling it, we avoid the having
       to send a reliable message when the transmit window is full.  */
    uint8_t i;
    for (i = 0; i < MAX_PUBS; i++) {
        if (pubs[i].rid == 0) {
            break;
        }
    }
    assert(i < MAX_PUBS);
    assert(!bitset_test(pubs_isrel, i));
    pubs[i].rid = rid;
    if (reliable) {
        bitset_set(pubs_isrel, i);
    }
    bitset_set(pubs_to_declare, i);
    return (pubidx_t){i};
}

subidx_t subscribe(rid_t rid, zpsize_t xmitneed, subhandler_t handler, void *arg)
{
    uint8_t i;
    for (i = 0; i < MAX_SUBS; i++) {
        if (subs[i].rid == 0) {
            break;
        }
    }
    assert(i < MAX_SUBS);
    subs[i].rid = rid;
    subs[i].xmitneed = xmitneed;
    subs[i].handler = handler;
    subs[i].arg = arg;
    bitset_set(subs_to_declare, i);
    return (subidx_t){i};
}

int zeno_write(pubidx_t pubidx, zpsize_t sz, const void *data)
{
    /* returns 0 on failure and 1 on success; the only defined failure case is a full transmit
       window for reliable pulication while remote subscribers exist */
    struct out_conduit * const oc = pubs[pubidx.idx].oc;
    int relflag;
    assert(pubs[pubidx.idx].rid != 0);
    if (!bitset_test(rsubs, pubidx.idx)) {
        /* success is assured if there are no subscribers */
        return 1;
    }

    relflag = bitset_test(pubs_isrel, pubidx.idx);
    if (!oc_pack_msdata(oc, relflag, pubs[pubidx.idx].rid, sz)) {
        /* for reliable, a full window means failure; for unreliable it is a non-issue */
        return !relflag;
    } else {
        oc_pack_msdata_payload(oc, relflag, sz, data);
        oc_pack_msdata_done(oc, relflag);
#if LATENCY_BUDGET == 0
        msend();
#endif
        /* not flushing to allow packing */
        return 1;
    }
}

/////////////////////////////////////////////////////////////////////////////////////

void flush_output(ztime_t tnow)
{
    /* Flush any pending output if the latency budget has been exceeded */
#if LATENCY_BUDGET != 0 && LATENCY_BUDGET != LATENCY_BUDGET_INF
    if (outp > 0 && tnow >= outdeadline) {
        /* FIXME: oc_msend confuses out buffer, deadline & conduit -- we get away with it
           because we only have a single conduit, but it MUST be cleaned up */
        oc_msend();
    }
#endif
}

static void send_msync_oc(struct out_conduit * const oc, ztime_t tnow)
{
    if (oc->nsamples && tnow >= oc->tsynch) {
        oc_pack_msynch(oc, MSFLAG);
        oc->tsynch = tnow + MSYNCH_INTERVAL;
        msend();
    }
}

void send_msynch(ztime_t tnow)
{
    /* Send a SYNCH messages on all output conduits until all reliable messages have been
       acknowledged, if the deadline for doing so has passed */
    cid_t i;
    for (i = 0; i < MAX_PEERS_1; i++) {
        /* FIXME: only if peer exists */
        send_msync_oc(&peers[i].oc, tnow);
    }
    for (i = 1; i < N_OUT_CONDUITS; i++) {
        send_msync_oc(&out_mconduits[i-1].oc, tnow);
    }
}

void send_declares(ztime_t tnow)
{
#if MAX_PEERS <= 1
    struct out_conduit * const oc = &peers[0].oc;
#else
    struct out_conduit * const oc = &out_mconduits[0].oc;
#endif
    int first;

    /* Push out any pending declarations.  We keep trying until the transmit window has room.
       It may therefore be a while before the broker is informed of a new publication, and
       conceivably data could be published that will be lost.  */
    if ((first = bitset_findfirst(pubs_to_declare, MAX_PUBS)) >= 0) {
        if (oc_pack_mdeclare(oc, 1, WC_DPUB_SIZE)) {
            assert(pubs[first].rid != 0);
            pack_dpub(pubs[first].rid);
            oc_pack_mdeclare_done(oc);
            bitset_clear(pubs_to_declare, first);
            must_commit = 1;
        }
    } else if ((first = bitset_findfirst(subs_to_declare, MAX_SUBS)) >= 0) {
        if (oc_pack_mdeclare(oc, 1, WC_DSUB_SIZE)) {
            assert(subs[first].rid != 0);
            pack_dsub(subs[first].rid);
            oc_pack_mdeclare_done(oc);
            bitset_clear(subs_to_declare, first);
            must_commit = 1;
        }
    } else if (must_commit && oc_pack_mdeclare(oc, 1, WC_DCOMMIT_SIZE)) {
        pack_dcommit(gcommitid++);
        oc_pack_mdeclare_done(oc);
        msend();
        must_commit = 0;
    }
}

void xrce_loop_init(void)
{
    t_state_changed = millis();
}

void xrce_loop(void)
{
    /* 115200 baud, MTU = 128 + 3 bytes header, so < 1.4ms per packet.  That's good enough to
       treat writing a packet as an atomic operation. */
    ztime_t tnow = millis();

    switch (zeno_state) {
        case STATE_WAITINPUT:
        case STATE_DRAININPUT:
            {
                /* On startup, wait up to 5s for some input, and if some is received, drain
                   the input until nothing is received for 1s.  For some reason, garbage
                   seems to come in a few seconds after waking up.  */
                ztime_t timeout = (zeno_state == STATE_WAITINPUT) ? 5000 : 1000;
                if (tnow - t_state_changed > timeout) {
                    zeno_state = STATE_SCOUT;
                    t_state_changed = tnow;
                } else if (0 /*Serial.available()*/) { /* FIXME */
                    //(void)Serial.read();
                    zeno_state = STATE_DRAININPUT;
                    t_state_changed = tnow;
                }
            }
            break;

        case STATE_SCOUT:
            /* SCOUT sends the first scout message, SCOUT_SENT sends it periodically.  The
               distinction exists solely to guarantee scouting starts immediately after
               transitioning to the scouting state without having to worry mess with
               t_state_changed. */
            pack_mscout(&scoutaddr);
            msend();
            zeno_state = STATE_SCOUT_SENT;
            t_state_changed = tnow;
            break;
        case STATE_SCOUT_SENT:
            if (tnow > t_state_changed + SCOUT_INTERVAL) {
                pack_mscout(&scoutaddr);
                msend();
                t_state_changed = tnow;
            }
            break;

        case STATE_CONNECTED:
            /* After a connection to broker has been established, send initial
               declarations; initially just one sub listening for the resource id to use.
               Note that "oc_pack_mdeclare" necessarily succeeds because the transmit
               window and the remote subscriptions have both been reset, and therefore no
               one can be using any space in the reliable transmit window yet.  That also
               means we transition to OPERATIONAL really quickly and there is no need to
               check leases here.

               FIXME: should publish all declarations (or reset pubs_to_declare so that it will
               happen automagically afterward) */
            if (!oc_pack_mdeclare(&peers[0].oc, 2, WC_DSUB_SIZE + WC_DCOMMIT_SIZE)) {
                PANIC0;
            }
            pack_dsub(1);
            pack_dcommit(gcommitid++);
            oc_pack_mdeclare_done(&peers[0].oc);
            msend();
            zeno_state = STATE_OPERATIONAL;
            t_state_changed = tnow;
            break;

        case STATE_OPERATIONAL:
            if ((tnow - t_state_changed) / 100 > brokerlease) {
                close_connection_and_scout();
                break;
            }
            send_msynch(tnow);
            flush_output(tnow);
            send_declares(tnow);
            break;

        default:
            /* opening: repeatedly send open until accepted, closed or timeout */
            assert(zeno_state >= STATE_OPENING_MIN && zeno_state <= STATE_OPENING_MAX);
            if (zeno_state == STATE_OPENING_MIN || tnow > t_state_changed + OPEN_INTERVAL) {
                if (zeno_state == STATE_OPENING_MIN + OPEN_RETRIES) {
                    zeno_state = STATE_SCOUT;
                } else {
                    pack_mopen(&peers[0].oc.addr, SEQNUM_LEN, sizeof(peerid), peerid, lease_dur);
                    msend();
                    zeno_state++;
                }
                t_state_changed = tnow;
            }
            break;
    }

    if (zeno_state >= STATE_SCOUT) {
#if TRANSPORT_MODE == TRANSPORT_PACKET
        if (read_serial()) {
            handle_packet(inp, inbuf);
        }
#elif TRANSPORT_MODE == TRANSPORT_STREAM
        uint8_t read_something = 0;
        while (inp < sizeof (inbuf) /*&& Serial.available()*/) {
            inbuf[inp++] = 0;//Serial.read();
            read_something = 1;
        }
        if (inp == 0) {
            t_progress = tnow;
        } else {
            /* No point in repeatedly trying to decode the same incomplete data */
            if (read_something) {
                uint8_t *datap = handle_packet(0, inp, inbuf); /* FIXME: need peer idx */
                zmsize_t cons = (zmsize_t) (datap - inbuf);
                if (cons > 0) {
                    t_progress = tnow;
                    if (zeno_state == STATE_OPERATIONAL) {
                        /* any packet is considered proof of liveliness of the broker (the
                           state of course doesn't really change ...) */
#if TRANSPORT_MODE == TRANSPORT_PACKET
                        /* must make sure the source address matches that of the broker */
                        PANIC0;
#endif
                        t_state_changed = t_progress;
                    }
                    if (cons < inp) {
                        memmove(inbuf, datap, inp - cons);
                    }
                    inp -= cons;
                }
            }

            if (inp == sizeof(inbuf) || (inp > 0 && tnow > t_progress + 300)) {
                /* No progress: discard whatever we have buffered and hope for the best. */
                inp = 0;
            }
        }
#else
#error "TRANSPORT_MODE not handled"
#endif
    }
}
