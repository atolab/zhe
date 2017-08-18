#ifndef ZENO_UNPACK_H
#define ZENO_UNPACK_H

#include "zeno-config-deriv.h"

int unpack_skip(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, zmsize_t n);
int unpack_byte(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, uint8_t * restrict u);
int unpack_u16(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, uint16_t * restrict u);
int unpack_vle16(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, uint16_t * restrict u);
int unpack_vle32(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, uint32_t * restrict u);
int unpack_vle64(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, uint64_t * restrict u);
int unpack_seq(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, seq_t * restrict u);
const uint8_t *skip_validated_vle(const uint8_t * restrict data);
int unpack_rid(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, rid_t * restrict u);
int unpack_vec(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, size_t lim, zpsize_t * restrict u, uint8_t * restrict v);
int unpack_props(zmsize_t * restrict sz, const uint8_t * restrict * restrict data);

struct unpack_locs_iter {
    uint16_t n;
    zmsize_t sz;
    const uint8_t *data;
};

int unpack_locs(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, struct unpack_locs_iter *it);
int unpack_locs_iter(struct unpack_locs_iter *it, zpsize_t *sz, const uint8_t **loc);

#endif
