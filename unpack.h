#ifndef ZENO_UNPACK_H
#define ZENO_UNPACK_H

#include "zeno-config-int.h"

int unpack_skip(zmsize_t *sz, const uint8_t **data, zmsize_t n);
int unpack_byte(zmsize_t *sz, const uint8_t **data, uint8_t *u);
int unpack_u16(zmsize_t *sz, const uint8_t **data, uint16_t *u);
int unpack_vle16(zmsize_t *sz, const uint8_t **data, uint16_t *u);
int unpack_vle32(zmsize_t *sz, const uint8_t **data, uint32_t *u);
int unpack_vle64(zmsize_t *sz, const uint8_t **data, uint64_t *u);
int unpack_seq(zmsize_t *sz, const uint8_t **data, seq_t *u);
const uint8_t *skip_validated_vle(const uint8_t *data);
int unpack_rid(zmsize_t *sz, const uint8_t **data, rid_t *u);
int unpack_vec(zmsize_t *sz, const uint8_t **data, size_t lim, zpsize_t *u, uint8_t *v);
int unpack_locs(zmsize_t *sz, const uint8_t **data);
int unpack_props(zmsize_t *sz, const uint8_t **data);

#endif
