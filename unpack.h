#ifndef ZENO_UNPACK_H
#define ZENO_UNPACK_H

#include "zeno.h"
#include "zeno-config-deriv.h"

int unpack_skip(uint8_t const * const end, uint8_t const * * const data, zmsize_t n);
int unpack_byte(uint8_t const * const end, uint8_t const * * const data, uint8_t * restrict u);
int unpack_u16(uint8_t const * const end, uint8_t const * * const data, uint16_t * restrict u);
int unpack_vle16(uint8_t const * const end, uint8_t const * * const data, uint16_t * restrict u);
int unpack_vle32(uint8_t const * const end, uint8_t const * * const data, uint32_t * restrict u);
int unpack_vle64(uint8_t const * const end, uint8_t const * * const data, uint64_t * restrict u);
int unpack_seq(uint8_t const * const end, uint8_t const * * const data, seq_t * restrict u);
const uint8_t *skip_validated_vle(const uint8_t *data);
int unpack_rid(uint8_t const * const end, uint8_t const * * const data, rid_t * restrict u);
int unpack_vec(uint8_t const * const end, uint8_t const * * const data, size_t lim, zpsize_t * restrict u, uint8_t * restrict v);
int unpack_props(uint8_t const * const end, uint8_t const * * const data);

struct unpack_locs_iter {
    uint16_t n;
    const uint8_t *data;
    const uint8_t *end;
};

int unpack_locs(uint8_t const * const end, uint8_t const * * const data, struct unpack_locs_iter *it);
int unpack_locs_iter(struct unpack_locs_iter *it, zpsize_t *sz, const uint8_t **loc);

#endif
