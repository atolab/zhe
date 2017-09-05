#ifndef ZHE_UNPACK_H
#define ZHE_UNPACK_H

#include "zhe-config-deriv.h"

int zhe_unpack_skip(uint8_t const * const end, uint8_t const * * const data, zhe_msgsize_t n);
int zhe_unpack_byte(uint8_t const * const end, uint8_t const * * const data, uint8_t * restrict u);
int zhe_unpack_vle16(uint8_t const * const end, uint8_t const * * const data, uint16_t * restrict u);
int zhe_unpack_vle32(uint8_t const * const end, uint8_t const * * const data, uint32_t * restrict u);
int zhe_unpack_vle64(uint8_t const * const end, uint8_t const * * const data, uint64_t * restrict u);
int zhe_unpack_seq(uint8_t const * const end, uint8_t const * * const data, seq_t * restrict u);
const uint8_t *zhe_skip_validated_vle(const uint8_t *data);
int zhe_unpack_rid(uint8_t const * const end, uint8_t const * * const data, zhe_rid_t * restrict u);
int zhe_unpack_vec(uint8_t const * const end, uint8_t const * * const data, size_t lim, zhe_paysize_t * restrict u, uint8_t * restrict v);
int zhe_unpack_props(uint8_t const * const end, uint8_t const * * const data);

struct unpack_locs_iter {
    uint16_t n;
    const uint8_t *data;
    const uint8_t *end;
};

int zhe_unpack_locs(uint8_t const * const end, uint8_t const * * const data, struct unpack_locs_iter *it);
int zhe_unpack_locs_iter(struct unpack_locs_iter *it, zhe_paysize_t *sz, const uint8_t **loc);

#endif
