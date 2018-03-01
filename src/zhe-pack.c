/* -*- mode: c; c-basic-offset: 4; fill-column: 95; -*- */
#include <string.h>
#include <limits.h>

#include "zhe-config-int.h"
#include "zhe-msg.h"
#include "zhe-pack.h"
#include "zhe-int.h"
#include "zhe-tracing.h"
#include "zhe-assert.h"

#define WORST_CASE_SEQ_SIZE ((8 * SEQNUM_LEN + 6) / 7)

static const uint8_t auth[] = { 2, 3 }; /* we don't do auth, but this matches Angelo's broker proto */

void zhe_pack_vle8(uint8_t x)
{
    if (x <= 0x7f) {
        zhe_pack1(x);
    } else {
        zhe_pack1(0x80 | (x & 0x7f));
        zhe_pack1(1);
    }
}

zhe_paysize_t zhe_pack_vle8req(uint8_t x)
{
    return (x <= 0x7f) ? 1 : 2;
}

void zhe_pack_vle16(uint16_t x)
{
    if (x <= 0x7f) {
        zhe_pack1((uint8_t)x);
    } else if (x <= 0x3fff) {
        zhe_pack1(0x80 | ((uint8_t)x & 0x7f));
        zhe_pack1((uint8_t)(x >> 7));
    } else {
        zhe_pack1(0x80 | ((uint8_t)x & 0x7f)); x >>= 7;
        zhe_pack1(0x80 | ((uint8_t)x & 0x7f)); x >>= 7;
        zhe_pack1((uint8_t)x);
    }
}

zhe_paysize_t zhe_pack_vle16req(uint16_t x)
{
    return (x <= 0x7f) ? 1 : (x <= 0x3fff) ? 2 : 3;
}

void zhe_pack_vle32(uint32_t x)
{
    do {
        zhe_pack1((x & 0x7f) | ((x > 0x7f) ? 0x80 : 0));
        x >>= 7;
    } while (x);
}

zhe_paysize_t zhe_pack_vle32req(uint32_t x)
{
    zhe_paysize_t n = 0;
    do { n++; x >>= 7; } while (x != 0);
    return n;
}

#if ZHE_RID_SIZE > 32 || SEQNUM_LEN > 28
void zhe_pack_vle64(uint64_t x)
{
    do {
        zhe_pack1((x & 0x7f) | ((x > 0x7f) ? 0x80 : 0));
        x >>= 7;
    } while (x);
}

zhe_paysize_t zhe_pack_vle64req(uint64_t x)
{
    zhe_paysize_t n = 0;
    do { n++; x >>= 7; } while (x != 0);
    return n;
}
#endif

void zhe_pack_seq(seq_t x)
{
#if SEQNUM_LEN <= 8
    zhe_pack1(x >> SEQNUM_SHIFT);
#elif SEQNUM_LEN <= 16
    zhe_pack_vle16(x >> SEQNUM_SHIFT);
#elif SEQNUM_LEN <= 32
    zhe_pack_vle32(x >> SEQNUM_SHIFT);
#elif SEQNUM_LEN <= 64
    zhe_pack_vle64(x >> SEQNUM_SHIFT);
#else
#error "zhe_pack_seq: invalid SEQNUM_LEN"
#endif
}

zhe_paysize_t zhe_pack_seqreq(seq_t x)
{
#if SEQNUM_LEN <= 8
    return 1;
#elif SEQNUM_LEN <= 16
    return zhe_pack_vle16req(x >> SEQNUM_SHIFT);
#elif SEQNUM_LEN <= 32
    return zhe_pack_vle32req(x >> SEQNUM_SHIFT);
#elif SEQNUM_LEN <= 64
    return zhe_pack_vle64req(x >> SEQNUM_SHIFT);
#else
#error "zhe_pack_seqreq: invalid SEQNUM_LEN"
#endif
}

void zhe_pack_rid(zhe_rid_t x)
{
    SUFFIX_WITH_SIZE(zhe_pack_vle, ZHE_RID_SIZE) ((zhe_rid_t)(x << 1));
}

zhe_paysize_t zhe_pack_ridreq(zhe_rid_t x)
{
    return INFIX_WITH_SIZE(zhe_pack_vle, ZHE_RID_SIZE, req) ((zhe_rid_t)(x << 1));
}

void zhe_pack_mscout(zhe_address_t *dst, zhe_time_t tnow)
{
    /* Client mode should only look for a broker, but a peer should look for peers and brokers
       (because a broker really can be considered a peer). */
#if MAX_PEERS == 0
    const uint8_t mask = MSCOUT_BROKER;
#else
    const uint8_t mask = MSCOUT_BROKER | MSCOUT_PEER;
#endif
    zhe_pack_reserve(dst, NULL, 2, tnow);
    zhe_pack2(MSCOUT, mask);
}

void zhe_pack_mhello(zhe_address_t *dst, zhe_time_t tnow)
{
#if MAX_PEERS == 0
    const uint8_t mask = MSCOUT_CLIENT;
#else
    const uint8_t mask = MSCOUT_PEER;
#endif
    zhe_pack_reserve(dst, NULL, 3 + zhe_pack_locs_calcsize(), tnow);
    zhe_pack2(MHELLO, mask);
    zhe_pack_locs();
}

static uint32_t conv_zhe_timediff_to_lease(zhe_timediff_t lease_dur)
{
    zhe_assert(lease_dur >= 0);
    return (uint32_t)(lease_dur / (100000000 / ZHE_TIMEBASE));
}

void zhe_pack_mopen(zhe_address_t *dst, uint8_t seqnumlen, const struct peerid *ownid, zhe_timediff_t lease_dur, zhe_time_t tnow)
{
    const uint32_t ld100 = conv_zhe_timediff_to_lease(lease_dur);
    zhe_paysize_t propsize = 0;
    uint8_t nprops = 0;
    /* Determine size of properties; if properties are present, also need space for number of properties
       (some of this stuff is VLE encoded, but for now we're only dealing with property codes <= 127 and
       fewer than 127 properties, so it will fit in a single byte */
    if (seqnumlen != 14) { /* 1 byte prop code + 1 byte payload size + 1 byte content */
        propsize += 3;
        nprops++;
    }
    if (sizeof(auth) != 0) { /* 1 byte prop code + arbitrary data vector */
        propsize += 1 + zhe_pack_vle16req(sizeof(auth)) + sizeof(auth);
        nprops++;
    }
    if (nprops > 0) {
        propsize += 1;
    }
    zhe_pack_reserve(dst, NULL, 2 + zhe_pack_vle16req(ownid->len) + ownid->len + zhe_pack_vle32req(ld100) + zhe_pack_locs_calcsize() + propsize, tnow);
    zhe_pack2((nprops ? MPFLAG : 0) | MOPEN, ZHE_VERSION);
    zhe_pack_vec(ownid->len, ownid->id);
    zhe_pack_vle32(ld100);
    zhe_pack_locs();
    if (nprops) {
        zhe_pack1(nprops);
        if (seqnumlen != 14) {
            zhe_pack1(PROP_SEQLEN); zhe_pack2(1, seqnumlen);
        }
        if (sizeof(auth) != 0) {
            zhe_pack1(PROP_AUTHDATA); zhe_pack_vec(sizeof(auth), auth);
        }
    }
}

void zhe_pack_maccept(zhe_address_t *dst, const struct peerid *ownid, const struct peerid *peerid, zhe_timediff_t lease_dur, zhe_time_t tnow)
{
    const uint32_t ld100 = conv_zhe_timediff_to_lease(lease_dur);
    zhe_paysize_t propsize = 0;
    uint8_t nprops = 0;
    if (sizeof(auth) != 0) { /* 1 byte prop code + arbitrary data vector */
        propsize += 1 + zhe_pack_vle16req(sizeof(auth)) + sizeof(auth);
        nprops++;
    }
    if (nprops > 0) {
        propsize += 1;
    }
    zhe_pack_reserve(dst, NULL, 1 + zhe_pack_vle16req(ownid->len) + ownid->len + zhe_pack_vle16req(peerid->len) + peerid->len + zhe_pack_vle32req(ld100) + propsize, tnow);
    zhe_pack1((nprops ? MPFLAG : 0) | MACCEPT);
    zhe_pack_vec(peerid->len, peerid->id);
    zhe_pack_vec(ownid->len, ownid->id);
    zhe_pack_vle32(ld100);
    if (nprops) {
        zhe_pack1(nprops);
        if (sizeof(auth) != 0) {
            zhe_pack1(PROP_AUTHDATA); zhe_pack_vec(sizeof(auth), auth);
        }
    }
}

void zhe_pack_mclose(zhe_address_t *dst, uint8_t reason, const struct peerid *ownid, zhe_time_t tnow)
{
    zhe_pack_reserve(dst, NULL, 2 + zhe_pack_vle16req(ownid->len) + ownid->len, tnow);
    zhe_pack1(MCLOSE);
    zhe_pack_vec(ownid->len, ownid->id);
    zhe_pack1(reason);
}

static zhe_paysize_t zhe_pack_mconduit_reqsize(cid_t cid)
{
#if N_OUT_CONDUITS > 127
#error "N_OUT_CONDUITS must be <= 127 or unconditionally packing a CID into a byte won't work"
#endif
    zhe_assert(cid >= 0 && cid <= 127);
    return (cid > 0) + (cid > 4);
}

static void zhe_pack_mconduit(cid_t cid)
{
    zhe_assert(cid >= 0 && cid <= 127);
    if (cid > 4) {
        zhe_pack2(MCONDUIT, (uint8_t)cid);
    } else if (cid > 0) {
        uint8_t eid = (uint8_t)((cid - 1) << 5);
        zhe_pack1(MCONDUIT | MZFLAG | eid);
    }
}

void zhe_pack_reserve_mconduit(zhe_address_t *dst, cid_t cid, bool track_cid, zhe_paysize_t cnt, zhe_time_t tnow)
{
#if HAVE_UNICAST_CONDUIT
    const cid_t norm_cid = (cid >= 0) ? cid : UNICAST_CID;
#else
    const cid_t norm_cid = cid;
#endif
    const zhe_paysize_t norm_cid_size = zhe_pack_mconduit_reqsize(norm_cid);
    zhe_pack_reserve(dst, track_cid ? zhe_out_conduit_from_cid(cid) : NULL, norm_cid_size + cnt, tnow);
    zhe_pack_mconduit(norm_cid);
}

void zhe_pack_reserve_mconduit_rawcid(zhe_address_t *dst, cid_t cid, zhe_paysize_t cnt, zhe_time_t tnow)
{
    const zhe_paysize_t cid_size = zhe_pack_mconduit_reqsize(cid);
    zhe_pack_reserve(dst, NULL, cid_size + cnt, tnow);
    zhe_pack_mconduit(cid);
}

unsigned zhe_synch_sent;

void zhe_pack_msynch(zhe_address_t *dst, uint8_t sflag, cid_t cid, seq_t seqbase, seq_t cnt, zhe_time_t tnow)
{
    seq_t cnt_shifted = (seq_t)(cnt << SEQNUM_SHIFT);
    seq_t seq_msg = seqbase + cnt_shifted;
    ZT(RELIABLE, "pack_msynch cid %d sflag %u seqbase %u cnt %u", cid, (unsigned)sflag, seqbase >> SEQNUM_SHIFT, (unsigned)cnt);
    zhe_pack_reserve_mconduit(dst, cid, false, 1 + zhe_pack_seqreq(seq_msg) + zhe_pack_seqreq(cnt_shifted), tnow);
    zhe_pack1(MRFLAG | sflag | (cnt > 0 ? MUFLAG : 0) | MSYNCH);
    zhe_pack_seq(seq_msg);
    if (cnt > 0) {
        zhe_pack_seq(cnt_shifted);
    }
    zhe_synch_sent++;
}

void zhe_pack_macknack(zhe_address_t *dst, cid_t cid, seq_t seq, uint32_t mask, zhe_time_t tnow)
{
    zhe_pack_reserve_mconduit_rawcid(dst, cid, 1 + zhe_pack_seqreq(seq) + (mask ? zhe_pack_vle32req(mask) : 0), tnow);
    zhe_pack1((mask == 0 ? 0 : MMFLAG) | MACKNACK);
    zhe_pack_seq(seq);
    if (mask != 0) {
        /* MFLAG implies a NACK of message SEQ, but the provided mask has the lsb correspond to
           a retransmit request of that message for uniformity. */
        zhe_pack_vle32(mask >> 1);
    }
}

void zhe_pack_mping(zhe_address_t *dst, uint16_t hash, zhe_time_t tnow)
{
    zhe_pack_reserve(dst, NULL, 1 + zhe_pack_vle16req(hash), tnow);
    zhe_pack1(MPING);
    zhe_pack_vle16(hash);
}

void zhe_pack_mpong(zhe_address_t *dst, uint16_t hash, zhe_time_t tnow)
{
    zhe_pack_reserve(dst, NULL, 1 + zhe_pack_vle16req(hash), tnow);
    zhe_pack1(MPONG);
    zhe_pack_vle16(hash);
}

void zhe_pack_mkeepalive(zhe_address_t *dst, const struct peerid *ownid, zhe_time_t tnow)
{
    zhe_pack_reserve(dst, NULL, 1 + zhe_pack_vle16req(ownid->len) + ownid->len, tnow);
    zhe_pack1(MKEEPALIVE);
    zhe_pack_vec(ownid->len, ownid->id);
}

int zhe_oc_pack_msdata(struct out_conduit *c, int relflag, zhe_rid_t rid, zhe_paysize_t payloadlen, zhe_time_t tnow)
{
    /* Use worst-case number of bytes for sequence number, instead of getting the sequence number
       earlier than as an output of oc_pack_payload_msgprep and using the exact value */
    const zhe_paysize_t sz = 1 + WORST_CASE_SEQ_SIZE + zhe_pack_ridreq(rid) + zhe_pack_vle16req(payloadlen) + payloadlen;
    const uint8_t hdr = MSDATA | (relflag ? MRFLAG : 0);
    zhe_msgsize_t from;
    seq_t s;

    if (relflag && !zhe_xmitw_hasspace(c, sz)) {
        /* Reliable, insufficient space in transmit window (accounting for preceding length byte) */
        zhe_oc_hit_full_window(c, tnow);
        return 0;
    }

    from = zhe_oc_pack_payload_msgprep(&s, c, relflag, sz, tnow);
    zhe_pack1(hdr);
    zhe_pack_seq(s);
    zhe_pack_rid(rid);
    zhe_pack_vle16(payloadlen);
    if (relflag) {
        zhe_oc_pack_copyrel(c, from);
    }
    return 1;
}

void zhe_oc_pack_msdata_payload(struct out_conduit *c, int relflag, zhe_paysize_t sz, const void *vdata)
{
    zhe_oc_pack_payload(c, relflag, sz, vdata);
}

void zhe_oc_pack_msdata_done(struct out_conduit *c, int relflag, zhe_time_t tnow)
{
    zhe_oc_pack_payload_done(c, relflag, tnow);
}

int zhe_oc_pack_mwdata(struct out_conduit *c, int relflag, zhe_paysize_t urisz, const void *uri, zhe_paysize_t payloadlen, zhe_time_t tnow)
{
    /* Use worst-case number of bytes for sequence number, instead of getting the sequence number
     earlier than as an output of oc_pack_payload_msgprep and using the exact value */
    const zhe_paysize_t sz = 1 + WORST_CASE_SEQ_SIZE + zhe_pack_vle16req(urisz) + urisz + zhe_pack_vle16req(payloadlen) + payloadlen;
    const uint8_t hdr = MWDATA | (relflag ? MRFLAG : 0);
    zhe_msgsize_t from;
    seq_t s;

    if (relflag && !zhe_xmitw_hasspace(c, sz)) {
        /* Reliable, insufficient space in transmit window (accounting for preceding length byte) */
        zhe_oc_hit_full_window(c, tnow);
        return 0;
    }

    from = zhe_oc_pack_payload_msgprep(&s, c, relflag, sz, tnow);
    zhe_pack1(hdr);
    zhe_pack_seq(s);
    zhe_pack_vec(urisz, uri);
    zhe_pack_vle16(payloadlen);
    if (relflag) {
        zhe_oc_pack_copyrel(c, from);
    }
    return 1;
}

void zhe_oc_pack_mwdata_payload(struct out_conduit *c, int relflag, zhe_paysize_t sz, const void *vdata)
{
    zhe_oc_pack_msdata_payload(c, relflag, sz, vdata);
}

void zhe_oc_pack_mwdata_done(struct out_conduit *c, int relflag, zhe_time_t tnow)
{
    zhe_oc_pack_msdata_done(c, relflag, tnow);
}

int zhe_oc_pack_mdeclare(struct out_conduit *c, bool committed, uint8_t ndecls, zhe_paysize_t decllen, zhe_msgsize_t *from, zhe_time_t tnow)
{
    const zhe_paysize_t sz = 1 + WORST_CASE_SEQ_SIZE + zhe_pack_vle16req(ndecls) + decllen;
    seq_t s;
    zhe_assert(ndecls <= 127);
    if (!zhe_xmitw_hasspace(c, sz)) {
        return 0;
    }
    *from = zhe_oc_pack_payload_msgprep(&s, c, 1, sz, tnow);
    zhe_pack1(MDECLARE | (committed ? MCFLAG : 0));
    zhe_pack_seq(s);
    zhe_pack_vle16(ndecls);
    return 1;
}

void zhe_oc_pack_mdeclare_done(struct out_conduit *c, zhe_msgsize_t from, zhe_time_t tnow)
{
    zhe_oc_pack_copyrel(c, from);
    zhe_oc_pack_payload_done(c, 1, tnow);
}

void zhe_pack_dresource(zhe_rid_t rid, zhe_paysize_t urisz, const uint8_t *res)
{
    zhe_pack1(DRESOURCE);
    zhe_pack_rid(rid);
    zhe_pack_vec(urisz, res);
}

void zhe_pack_dpub(zhe_rid_t rid)
{
    zhe_pack1(DPUB);
    zhe_pack_rid(rid);
}

void zhe_pack_dsub(zhe_rid_t rid)
{
    zhe_pack1(DSUB);
    zhe_pack_rid(rid);
    zhe_pack1(SUBMODE_PUSH); /* FIXME: should be a parameter */
}

void zhe_pack_dcommit(uint8_t commitid)
{
    zhe_pack2(DCOMMIT, commitid);
}

void zhe_pack_dresult(uint8_t commitid, uint8_t status, zhe_rid_t rid)
{
    zhe_pack1(DRESULT);
    zhe_pack2(commitid, status);
    if (status) {
        zhe_pack_rid(rid);
    }
}
