#ifndef ZENO_INT_H
#define ZENO_INT_H

#include "zeno.h"
#include "zeno-config-int.h"

#define SUFFIX_WITH_SIZE1(name, size) name##size
#define SUFFIX_WITH_SIZE(name, size) SUFFIX_WITH_SIZE1(name, size)

#define INFIX_WITH_SIZE1(name, size, suf) name##size##suf
#define INFIX_WITH_SIZE(name, size, suf) INFIX_WITH_SIZE1(name, size, suf)

struct out_conduit;
struct out_mconduit;
struct in_conduit;

struct peerid {
    uint8_t id[PEERID_SIZE];
    zpsize_t len;
};

uint16_t xmitw_bytesavail(const struct out_conduit *c);
void pack_reserve(zeno_address_t *dst, struct out_conduit *oc, zpsize_t cnt);
void pack1(uint8_t x);
void pack2(uint8_t x, uint8_t y);
void pack_u16(uint16_t x);
void pack_vec(zpsize_t n, const void *buf);
uint16_t pack_locs_calcsize(void);
void pack_locs(void);
void oc_hit_full_window(struct out_conduit *c);
int oc_am_draining_window(const struct out_conduit *c);
cid_t oc_get_cid(struct out_conduit *c);
int ocm_have_peers(const struct out_mconduit *mc);
void pack_msend(void);
zmsize_t oc_pack_payload_msgprep(seq_t *s, struct out_conduit *c, int relflag, zpsize_t sz);
void oc_pack_copyrel(struct out_conduit *c, zmsize_t from);
void oc_pack_payload(struct out_conduit *c, int relflag, zpsize_t sz, const void *vdata);
void oc_pack_payload_done(struct out_conduit *c, int relflag, ztime_t tnow);
int seq_lt(seq_t a, seq_t b);
int seq_le(seq_t a, seq_t b);
struct out_conduit *out_conduit_from_cid(peeridx_t peeridx, cid_t cid);

#endif
