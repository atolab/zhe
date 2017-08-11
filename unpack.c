#include <limits.h>
#include "unpack.h"

int unpack_skip(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, zmsize_t n)
{
    if (*sz < n) {
        return 0;
    }
    *sz -= n;
    *data += n;
    return 1;
}

int unpack_byte(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, uint8_t * restrict u)
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

int unpack_u16(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, uint16_t * restrict u)
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

int unpack_vle16(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, uint16_t * restrict u)
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

int unpack_vle32(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, uint32_t * restrict u)
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

int unpack_vle64(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, uint64_t * restrict u)
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

int unpack_seq(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, seq_t * restrict u)
{
    if (!unpack_vle16(sz, data, u)) {
        return 0;
    }
    *u <<= SEQNUM_SHIFT;
    return 1;
}

const uint8_t *skip_validated_vle(const uint8_t * restrict data)
{
    while (*data & 0x80) {
        data++;
    }
    return data;
}

int unpack_rid(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, rid_t * restrict u)
{
    return SUFFIX_WITH_SIZE(unpack_vle, RID_T_SIZE)(sz, data, u);
}

int unpack_vec(zmsize_t * restrict sz, const uint8_t * restrict * restrict data, size_t lim, zpsize_t * restrict u, uint8_t * restrict v)
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

int unpack_locs(zmsize_t * restrict sz, const uint8_t * restrict * restrict data)
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

int unpack_props(zmsize_t * restrict sz, const uint8_t * restrict * restrict data)
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
