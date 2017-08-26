#ifndef ZENO_PACK_H
#define ZENO_PACK_H

#include "zeno-config-deriv.h"

struct out_conduit;
struct peerid;

void pack_vle16(uint16_t x);
zpsize_t pack_vle16req(uint16_t x);
void pack_vle32(uint32_t x);
zpsize_t pack_vle32req(uint32_t x);
void pack_vle64(uint64_t x);
zpsize_t pack_vle64req(uint64_t x);
void pack_seq(seq_t x);
zpsize_t pack_seqreq(seq_t x);
void pack_rid(rid_t x);
zpsize_t pack_ridreq(rid_t x);
void pack_text(zpsize_t n, const char *text);
void pack_mscout(zeno_address_t *dst);
void pack_mhello(zeno_address_t *dst);
void pack_mopen(zeno_address_t *dst, uint8_t seqnumlen, const struct peerid *ownid, ztimediff_t lease_dur);
void pack_maccept(zeno_address_t *dst, const struct peerid *ownid, const struct peerid *peerid, ztimediff_t lease_dur);
void pack_mclose(zeno_address_t *dst, uint8_t reason, const struct peerid *ownid);
void pack_reserve_mconduit(zeno_address_t *dst, struct out_conduit *oc, cid_t cid, zpsize_t cnt);
void pack_msynch(zeno_address_t *dst, uint8_t sflag, cid_t cid, seq_t seqbase, seq_t cnt);
void pack_macknack(zeno_address_t *dst, cid_t cid, seq_t seq, uint32_t mask);
void pack_mping(zeno_address_t *dst, uint16_t hash);
void pack_mpong(zeno_address_t *dst, uint16_t hash);
void pack_mkeepalive(zeno_address_t *dst, const struct peerid *ownid);
int oc_pack_msdata(struct out_conduit *c, int relflag, rid_t rid, zpsize_t payloadlen);
void oc_pack_msdata_payload(struct out_conduit *c, int relflag, zpsize_t sz, const void *vdata);
void oc_pack_msdata_done(struct out_conduit *c, int relflag, ztime_t tnow);
int oc_pack_mdeclare(struct out_conduit *c, uint8_t ndecls, uint8_t decllen, zmsize_t *from);
void oc_pack_mdeclare_done(struct out_conduit *c, zmsize_t from, ztime_t tnow);
void pack_dresource(rid_t rid, const char *res);
void pack_dpub(rid_t rid);
void pack_dsub(rid_t rid);
void pack_dcommit(uint8_t commitid);
void pack_dresult(uint8_t commitid, uint8_t status, rid_t rid);

#endif
