#include <limits.h>
#include <assert.h>
#include "unpack.h"
#include "zeno-int.h"

int unpack_skip(uint8_t const * const end, uint8_t const * * const data, zmsize_t n)
{
    if (end - *data < n) {
        return 0;
    }
    *data += n;
    return 1;
}

int unpack_byte(uint8_t const * const end, uint8_t const * * const data, uint8_t * restrict u)
{
    if (end - *data < 1) {
        return 0;
    }
    if (u) {
        *u = **data;
    }
    *data += 1;
    return 1;
}

int unpack_u16(uint8_t const * const end, uint8_t const * * const data, uint16_t * restrict u)
{
    if (end - *data < 2) {
        return 0;
    }
    if (u) {
        *u = (uint16_t)((*data)[0] | ((uint16_t)((*data)[1]) << 8));
    }
    *data += 2;
    return 1;
}

int unpack_vle16(uint8_t const * const end, uint8_t const * * const data, uint16_t * restrict u)
{
    uint16_t n;
    uint8_t shift = 7;
    uint8_t x;
    if (end == *data) {
        return 0;
    }
    x = **data; (*data)++;
    n = x & 0x7f;
    while (x & 0x80) {
        if (end == *data) {
            return 0;
        }
        x = **data; (*data)++;
        if (shift < CHAR_BIT * sizeof(*u)) { /* else behaviour is undefined */
            n |= ((uint16_t)(x & 0x7f)) << shift;
            shift += 7;
        }
    }
    if (u) *u = n;
    return 1;
}

int unpack_vle32(uint8_t const * const end, uint8_t const * * const data, uint32_t * restrict u)
{
    uint32_t n;
    uint8_t shift = 7;
    uint8_t x;
    if (end == *data) { return 0; }
    x = **data; (*data)++;
    n = x & 0x7f;
    while (x & 0x80) {
        if (end == *data) { return 0; }
        x = **data; (*data)++;
        if (shift < CHAR_BIT * sizeof(*u)) { /* else behaviour is undefined */
            n |= ((uint32_t)(x & 0x7f)) << shift;
            shift += 7;
        }
    }
    if (u) *u = n;
    return 1;
}

#if RID_T_SIZE > 32 || SEQNUM_LEN > 28
int unpack_vle64(uint8_t const * const end, uint8_t const * * const data, uint64_t * restrict u)
{
    uint64_t n;
    uint8_t shift = 7;
    uint8_t x;
    if (end == *data) { return 0; }
    x = **data; (*data)++;
    n = x & 0x7f;
    while (x & 0x80) {
        if (end == *data) { return 0; }
        x = **data; (*data)++;
        if (shift < CHAR_BIT * sizeof(*u)) { /* else behaviour is undefined */
            n |= ((uint64_t)(x & 0x7f)) << shift;
            shift += 7;
        }
    }
    if (u) *u = n;
    return 1;
}
#endif

int unpack_seq(uint8_t const * const end, uint8_t const * * const data, seq_t * restrict u)
{
    int res;
#if SEQNUM_LEN == 7
    res = unpack_byte(end, data, u);
#elif SEQNUM_LEN == 14
    res = unpack_vle16(end, data, u);
#elif SEQNUM_LEN == 28
    res = unpack_vle32(end, data, u);
#elif SEQNUM_LEN == 56
    res = unpack_vle64(end, data, u);
#else
#error "unpack_seq: invalid SEQNUM_LEN"
#endif
    if (!res) {
        return 0;
    }
    *u <<= SEQNUM_SHIFT;
    return 1;
}

const uint8_t *skip_validated_vle(const uint8_t *data)
{
    uint8_t d;
    do {
        d = *data++;
    } while (d & 0x80);
    return data;
}

int unpack_rid(uint8_t const * const end, uint8_t const * * const data, rid_t * restrict u)
{
    return SUFFIX_WITH_SIZE(unpack_vle, RID_T_SIZE)(end, data, u);
}

int unpack_vec(uint8_t const * const end, uint8_t const * * const data, size_t lim, zpsize_t * restrict u, uint8_t * restrict v)
{
    zpsize_t i;
    if (!unpack_vle16(end, data, u)) { return 0; }
    if (end - *data < *u) { return 0; }
    if (*u < lim) { lim = *u; }
    for (i = 0; i < lim; i++) {
        v[i] = **data;
        (*data)++;
    }
    (*data) += *u - lim;
    return 1;
}

int unpack_locs(uint8_t const * const end, uint8_t const * * const data, struct unpack_locs_iter *it)
{
    uint16_t n;
    zpsize_t dummy;
    if (!unpack_vle16(end, data, &n)) {
        return 0;
    }
    it->n = n;
    it->data = *data;
    while (n--) {
        if (!unpack_vec(end, data, 0, &dummy, NULL)) {
            return 0;
        }
    }
    it->end = *data;
    return 1;
}

int unpack_locs_iter(struct unpack_locs_iter *it, zpsize_t *sz, const uint8_t **loc)
{
    if (it->n == 0) {
        return 0;
    } else {
        int x = unpack_vle16(it->end, &it->data, sz);
        assert(x);
        *loc = it->data;
        it->data += *sz;
        it->n--;
        return 1;
    }
}

int unpack_props(uint8_t const * const end, uint8_t const * * const data)
{
    uint16_t n;
    zpsize_t dummy;
    if (!unpack_vle16(end, data, &n)) {
        return 0;
    }
    while (n--) {
        if (!unpack_vec(end, data, 0, &dummy, NULL) || !unpack_vec(end, data, 0, &dummy, NULL)) {
            return 0;
        }
    }
    return 1;
}
