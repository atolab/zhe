/* -*- mode: c; c-basic-offset: 4; fill-column: 95; -*- */
#include <string.h>
#include <limits.h>
#include <assert.h>

#include "zeno.h"
#include "zeno-config-int.h"
#include "zeno-msg.h"

#include "pack.h"
#include "zeno-int.h"
#include "zeno-tracing.h"

void pack_vle16(uint16_t x)
{
    do {
        pack1((x & 0x7f) | ((x > 127) ? 0x80 : 0));
        x >>= 7;
    } while (x);
}

zpsize_t pack_vle16req(uint16_t x)
{
    zpsize_t n = 0;
    do { n++; x >>= 7; } while (x != 0);
    return n;
}

void pack_vle32(uint32_t x)
{
    do {
        pack1((x & 0x7f) | ((x > 127) ? 0x80 : 0));
        x >>= 7;
    } while (x);
}

zpsize_t pack_vle32req(uint32_t x)
{
    zpsize_t n = 0;
    do { n++; x >>= 7; } while (x != 0);
    return n;
}

#if RID_T_SIZE > 32
void pack_vle64(uint64_t x)
{
    do {
        pack1((x & 0x7f) | ((x > 127) ? 0x80 : 0));
        x >>= 7;
    } while (x);
}

zpsize_t pack_vle64req(uint64_t x)
{
    zpsize_t n = 0;
    do { n++; x >>= 7; } while (x != 0);
    return n;
}
#endif

void pack_seq(seq_t x)
{
    pack_vle16(x >> SEQNUM_SHIFT);
}

zpsize_t pack_seqreq(seq_t x)
{
    return pack_vle16req(x >> SEQNUM_SHIFT);
}

void pack_rid(rid_t x)
{
    SUFFIX_WITH_SIZE(pack_vle, RID_T_SIZE) (x);
}

zpsize_t pack_ridreq(rid_t x)
{
    return INFIX_WITH_SIZE(pack_vle, RID_T_SIZE, req) (x);
}

void pack_text(zpsize_t n, const char *text)
{
    pack_vec(n, (const uint8_t *) text);
}

void pack_mscout(zeno_address_t *dst)
{
    /* Client mode should only look for a broker, but a peer should look for peers and brokers
       (because a broker really can be considered a peer).
       FIXME: I ought to make this a parameter and do it zeno.c ... */
#if MAX_PEERS == 0
    const uint8_t mask = MSCOUT_BROKER;
#else
    const uint8_t mask = MSCOUT_BROKER | MSCOUT_PEER;
#endif
    pack_reserve(dst, NULL, 2);
    pack2(MSFLAG | MSCOUT, mask);
}

void pack_mhello(zeno_address_t *dst)
{
    /* FIXME: format is header, mask (vle), locs, props - this implementation for constructing a hello message is rather too limited */
#if MAX_PEERS == 0
    const uint8_t mask = MSCOUT_CLIENT;
#else
    const uint8_t mask = MSCOUT_PEER;
#endif
    pack_reserve(dst, NULL, 3 + pack_locs_calcsize());
    pack2(MHELLO, mask);
    pack_locs();
    pack1(0);
}

void pack_mopen(zeno_address_t *dst, uint8_t seqnumlen, const struct peerid *ownid, ztimediff_t lease_dur)
{
    assert(lease_dur >= 0);
    const zpsize_t sizeof_auth = 0;
    const uint32_t ld100 = (uint32_t)(lease_dur / 100);
    pack_reserve(dst, NULL, 2 + pack_vle16req(ownid->len) + ownid->len + pack_vle32req(ld100) + pack_vle16req(sizeof_auth) + sizeof_auth + pack_locs_calcsize() + (seqnumlen != 14 ? 1 : 0));
    pack2(MSFLAG | (seqnumlen != 14 ? MLFLAG : 0) | MOPEN, ZENO_VERSION);
    pack_vec(ownid->len, ownid->id);
    pack_vle32(ld100);
    pack_text(0, NULL); /* auth */
    pack_locs();
    if (seqnumlen != 14) {
        pack1(seqnumlen);
    }
}

void pack_maccept(zeno_address_t *dst, const struct peerid *ownid, const struct peerid *peerid, ztimediff_t lease_dur)
{
    assert(lease_dur >= 0);
    const zpsize_t sizeof_auth = 0;
    const uint32_t ld100 = (uint32_t)(lease_dur / 100);
    pack_reserve(dst, NULL, 1 + pack_vle16req(ownid->len) + ownid->len + pack_vle16req(peerid->len) + peerid->len + pack_vle32req(ld100) + pack_vle16req(sizeof_auth) + sizeof_auth);
    pack1(MACCEPT);
    pack_vec(peerid->len, peerid->id);
    pack_vec(ownid->len, ownid->id);
    pack_vle32(ld100);
    pack_text(0, NULL); /* auth */
}

void pack_mclose(zeno_address_t *dst, uint8_t reason, const struct peerid *ownid)
{
    pack_reserve(dst, NULL, 2 + pack_vle16req(ownid->len) + ownid->len);
    pack1(MSFLAG | MCLOSE);
    pack_vec(ownid->len, ownid->id);
    pack1(reason);
}

void pack_reserve_mconduit(zeno_address_t *dst, struct out_conduit *oc, cid_t cid, zpsize_t cnt)
{
    zpsize_t cid_size = (cid > 0) + (cid > 4);
    assert(cid >= 0);
    assert(cid < N_OUT_CONDUITS);
#if N_OUT_CONDUITS > 127
#error "N_OUT_CONDUITS must be <= 127 or unconditionally packing a CID into a byte won't work"
#endif
    assert(oc == NULL || oc_get_cid(oc) == cid);
    pack_reserve(dst, oc, cid_size + cnt);
    if (cid > 4) {
        pack2(MCONDUIT, (uint8_t)cid);
    } else if (cid > 0) {
        uint8_t eid = (uint8_t)((cid - 1) << 5);
        pack1(MCONDUIT | MZFLAG | eid);
    }
}

unsigned zeno_synch_sent;

void pack_msynch(zeno_address_t *dst, uint8_t sflag, cid_t cid, seq_t seqbase, seq_t cnt)
{
    seq_t cnt_shifted = (seq_t)(cnt << SEQNUM_SHIFT);
    ZT(RELIABLE, ("pack_msynch cid %d sflag %u seq %u cnt %u", cid, (unsigned)sflag, seqbase >> SEQNUM_SHIFT, (unsigned)cnt));
    pack_reserve_mconduit(dst, NULL, cid, 1 + pack_seqreq(seqbase) + pack_seqreq(cnt_shifted));
    pack1(MRFLAG | sflag | MSYNCH);
    pack_seq(seqbase);
    pack_seq(cnt_shifted);
    zeno_synch_sent++;
}

void pack_macknack(zeno_address_t *dst, cid_t cid, seq_t seq, uint32_t mask)
{
    pack_reserve_mconduit(dst, NULL, cid, 1 + pack_seqreq(seq) + (mask ? pack_vle32req(mask) : 0));
    pack1(MSFLAG | (mask == 0 ? 0 : MMFLAG) | MACKNACK);
    pack_seq(seq);
    if (mask != 0) {
        /* MFLAG implies a NACK of message SEQ, but the provided mask has the lsb correspond to
           a retransmit request of that message for uniformity. */
        pack_vle32(mask >> 1);
    }
}

void pack_mping(zeno_address_t *dst, uint16_t hash)
{
    pack_reserve(dst, NULL, 3);
    pack1(MSFLAG | MPING);
    pack_u16(hash);
}

void pack_mpong(zeno_address_t *dst, uint16_t hash)
{
    pack_reserve(dst, NULL, 3);
    pack1(MPONG);
    pack_u16(hash);
}

void pack_mkeepalive(zeno_address_t *dst, const struct peerid *ownid)
{
    pack_reserve(dst, NULL, 1 + pack_vle16req(ownid->len) + ownid->len);
    pack1(MKEEPALIVE);
    pack_vec(ownid->len, ownid->id);
}

int oc_pack_msdata(struct out_conduit *c, int relflag, rid_t rid, zpsize_t payloadlen)
{
    /* FIXME: should use pack_seqreq instead of worst-case of 2 */
    const zpsize_t sz = 3 + pack_ridreq(rid) + pack_vle16req(payloadlen) + payloadlen;
    uint8_t hdr = MSDATA | (relflag ? MRFLAG : 0);
    zmsize_t from;
    seq_t s;

    if (relflag && xmitw_bytesavail(c) < sizeof(zmsize_t) + sz) {
        /* Reliable, insufficient space in transmit window (accounting for preceding length byte) */
        oc_hit_full_window(c);
        return 0;
    }

    from = oc_pack_payload_msgprep(&s, c, relflag, sz);
    pack1(hdr);
    pack_seq(s);
    pack_rid(rid);
    pack_vle16(payloadlen);
    if (relflag) {
        oc_pack_copyrel(c, from);
    }
    return 1;
}

void oc_pack_msdata_payload(struct out_conduit *c, int relflag, zpsize_t sz, const void *vdata)
{
    oc_pack_payload(c, relflag, sz, vdata);
}

void oc_pack_msdata_done(struct out_conduit *c, int relflag)
{
    oc_pack_payload_done(c, relflag);
}

int oc_pack_mdeclare(struct out_conduit *c, uint8_t ndecls, uint8_t decllen)
{
    zmsize_t from;
    seq_t s;
    assert(ndecls <= 127);
    if (xmitw_bytesavail(c) < sizeof(zmsize_t) + 5 + decllen) {
        /* no space in transmit window (1 byte size, 5 bytes header, decllen) */
        return 0;
    }
    from = oc_pack_payload_msgprep(&s, c, 1, 5 + decllen);
    pack1(MRFLAG | MSFLAG | MDECLARE);
    pack_seq(s);
    pack1(ndecls); /* VLE, but we limit it to 127 so always one byte */
    oc_pack_copyrel(c, from);
    return 1;
}

void oc_pack_mdeclare_done(struct out_conduit *c)
{
    oc_pack_payload_done(c, 1);
}

/* FIXME: not doing properties at the moment */

void pack_dresource(rid_t rid, const char *res)
{
    size_t ressz = strlen(res);
    assert(ressz <= (zpsize_t)-1);
    pack1(DRESOURCE);
    pack_rid(rid);
    pack_text((zpsize_t)ressz, res);
}

/* FIXME: do I need DELETE? Not yet anyway */

void pack_dpub(rid_t rid)
{
    pack1(DPUB);
    pack_rid(rid);
}

void pack_dsub(rid_t rid)
{
    pack1(DSUB);
    pack_rid(rid);
    pack1(SUBMODE_PUSH); /* FIXME: should be a parameter */
}

/* Do I need SELECTION, BINDID? Probably not, certainly not yet */

void pack_dcommit(uint8_t commitid)
{
    pack2(DCOMMIT, commitid);
}

void pack_dresult(uint8_t commitid, uint8_t status, rid_t rid)
{
    pack1(DRESULT);
    pack2(commitid, status);
    if (status) {
        pack_rid(rid);
    }
}
