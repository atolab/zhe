/* -*- mode: c; c-basic-offset: 4; fill-column: 95; -*- */
#include <string.h>
#include <limits.h>
#include <assert.h>

#include "zeno.h"
#include "zeno-config-int.h"
#include "zeno-msg.h"

#include "pack.h"
#include "zeno-int.h"

void pack_vle16(uint16_t x)
{
    do {
        pack1((x & 0x7f) | ((x > 127) ? 0x80 : 0));
        x >>= 7;
    } while (x);
}

size_t pack_vle16req(uint16_t x)
{
    size_t n = 0;
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

size_t pack_vle32req(uint32_t x)
{
    size_t n = 0;
    do { n++; x >>= 7; } while (x != 0);
    return n;
}

void pack_vle64(uint64_t x)
{
    do {
        pack1((x & 0x7f) | ((x > 127) ? 0x80 : 0));
        x >>= 7;
    } while (x);
}

size_t pack_vle64req(uint64_t x)
{
    size_t n = 0;
    do { n++; x >>= 7; } while (x != 0);
    return n;
}

void pack_seq(seq_t x)
{
    pack_vle16(x >> SEQNUM_SHIFT);
}

void pack_rid(rid_t x)
{
    SUFFIX_WITH_SIZE(pack_vle, RID_T_SIZE) (x);
}

size_t pack_ridreq(rid_t x)
{
    return INFIX_WITH_SIZE(pack_vle, RID_T_SIZE, req) (x);
}

void pack_text(zpsize_t n, const char *text)
{
    pack_vec(n, (const uint8_t *) text);
}

void pack_mscout(zeno_address_t *dst)
{
    pack_reserve(dst ,2);
    pack2(MSFLAG | MSCOUT, MSCOUT_BROKER);
}

void pack_mopen(zeno_address_t *dst, uint8_t seqnumlen, zmsize_t peeridlen, const void *peerid, uint32_t lease_dur)
{
    const size_t sizeof_auth = 0;
    pack_reserve(dst, 2 + pack_vle16req(peeridlen) + peeridlen + pack_vle32req(lease_dur) + pack_vle16req(sizeof_auth) + sizeof_auth);
    pack2(MSFLAG | (seqnumlen != 14 ? MLFLAG : 0) | MOPEN, ZENO_VERSION);
    pack_vec(peeridlen, peerid);
    pack_vle32(lease_dur);
    pack_text(0, NULL); /* auth */
    if (seqnumlen != 14) {
        pack1(seqnumlen);
    }
}

void pack_mclose(zeno_address_t *dst, uint8_t reason, zmsize_t peeridlen, const void *peerid)
{
    pack_reserve(dst, 2 + pack_vle16req(peeridlen) + peeridlen);
    pack1(MSFLAG | MCLOSE);
    pack_vec(peeridlen, peerid);
    pack1(reason);
}

void pack_reserve_mconduit(zeno_address_t *dst, cid_t cid, zpsize_t cnt)
{
    unsigned cid_size = (cid > 0) + (cid > 4);
    pack_reserve(dst, cid_size + cnt);
    if (cid > 4) {
        pack2(MCONDUIT, cid);
    } else if (cid > 0) {
        uint8_t eid = (cid - 1) << 4;
        pack1(MCONDUIT | MZFLAG | eid);
    }
}

void pack_msynch(zeno_address_t *dst, uint8_t sflag, cid_t cid, seq_t seqbase, seq_t cnt)
{
    pack_reserve_mconduit(dst, cid, 4 + pack_vle16req(cnt));
    pack1(MRFLAG | sflag | MSYNCH);
    pack_seq(seqbase);
    pack_seq(cnt);
}

void pack_macknack(zeno_address_t *dst, cid_t cid, seq_t seq, uint32_t mask)
{
    pack_reserve_mconduit(dst, cid, 4 + (mask ? pack_vle32req(mask) : 0));
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
    pack_reserve(dst, 3);
    pack1(MSFLAG | MPING);
    pack_u16(hash);
}

void pack_mpong(zeno_address_t *dst, uint16_t hash)
{
    pack_reserve(dst, 3);
    pack1(MPONG);
    pack_u16(hash);
}

void pack_mkeepalive(zeno_address_t *dst, zmsize_t peeridlen, const void *peerid)
{
    pack_reserve(dst, 1 + pack_vle16req(peeridlen) + peeridlen);
    pack1(MKEEPALIVE);
    pack_vec(peeridlen, peerid);
}

int oc_pack_msdata(struct out_conduit *c, int relflag, rid_t rid, zpsize_t payloadlen)
{
    const zpsize_t sz = 4 + pack_ridreq(rid) + pack_vle16req(payloadlen) + payloadlen;
    uint8_t hdr = MSDATA | (relflag ? MRFLAG : 0);
    zmsize_t from;
    seq_t s;

    if (relflag && XMITW_BYTES - xmitw_bytesused(c) < sizeof(zmsize_t) + sz) {
        /* Reliable, insufficient space in transmit window (accounting for preceding length byte) */
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
    if (XMITW_BYTES - xmitw_bytesused(c) < sizeof(zmsize_t) + 5 + decllen) {
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
    zpsize_t ressz = strlen(res);
    pack1(DRESOURCE);
    pack_rid(rid);
    pack_text(ressz, res);
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
