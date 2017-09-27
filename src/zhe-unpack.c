#include <limits.h>
#include "zhe-assert.h"
#include "zhe-int.h"
#include "zhe-unpack.h"

enum zhe_unpack_result zhe_unpack_skip(uint8_t const * const end, uint8_t const * * const data, zhe_msgsize_t n)
{
    if (end - *data < n) {
        return ZUR_SHORT;
    }
    *data += n;
    return ZUR_OK;
}

enum zhe_unpack_result zhe_unpack_byte(uint8_t const * const end, uint8_t const * * const data, uint8_t * restrict u)
{
    if (end - *data < 1) {
        return ZUR_SHORT;
    }
    if (u) {
        *u = **data;
    }
    *data += 1;
    return ZUR_OK;
}

#define DEF_UNPACK_VLE(size_) \
enum zhe_unpack_result zhe_unpack_vle##size_(uint8_t const * const end, uint8_t const * * const data, uint##size_##_t * restrict u) \
{ \
    uint##size_##_t n; \
    uint8_t shift = 7; \
    uint8_t x; \
    if (end == *data) { \
        return ZUR_SHORT; \
    } \
    x = **data; (*data)++; \
    n = x & 0x7f; \
    while (x & 0x80 && shift < CHAR_BIT * sizeof(*u)) { \
        if (end == *data) { \
            return ZUR_SHORT; \
        } \
        x = **data; (*data)++; \
        n |= ((uint##size_##_t)(x & 0x7f)) << shift; \
        shift += 7; \
    } \
    if (u) { \
        *u = n; \
    } \
    if (shift < CHAR_BIT * sizeof(*u)) { \
        return ZUR_OK; \
    } else { \
        uint8_t overflow = ((sizeof(*u) % 7) == 0) ? 0 : (x >> (sizeof(*u) % 7)); \
        while (x & 0x80) { \
            if (end == *data) { \
                return ZUR_SHORT; \
            } \
            x = **data; (*data)++; \
            overflow |= x & 0x7f; \
        } \
        return (overflow ? ZUR_OVERFLOW : ZUR_OK); \
    } \
}
DEF_UNPACK_VLE(8)
DEF_UNPACK_VLE(16)
DEF_UNPACK_VLE(32)
#if ZHE_RID_SIZE > 32 || SEQNUM_LEN > 28
DEF_UNPACK_VLE(64)
#endif
#undef DEF_UNPACK_VLE

enum zhe_unpack_result zhe_unpack_seq(uint8_t const * const end, uint8_t const * * const data, seq_t * restrict u)
{
    enum zhe_unpack_result res;
#if SEQNUM_LEN == 7
    res = zhe_unpack_vle8(end, data, u);
#elif SEQNUM_LEN == 14
    res = zhe_unpack_vle16(end, data, u);
#elif SEQNUM_LEN == 28
    res = zhe_unpack_vle32(end, data, u);
#elif SEQNUM_LEN == 56
    res = zhe_unpack_vle64(end, data, u);
#else
#error "unpack_seq: invalid SEQNUM_LEN"
#endif
    if (res != ZUR_OK) {
        return res;
    } else if (*u & (seq_t)((seq_t)-1 << SEQNUM_LEN)) {
        /* Oy vey! the unpacked number fits in seq_t, but it has some of the msbs
           that we will shift out set, which really makes it an overflow */
        return ZUR_OVERFLOW;
    } else {
        *u <<= SEQNUM_SHIFT;
        return ZUR_OK;
    }
}

const uint8_t *zhe_skip_validated_vle(const uint8_t *data)
{
    uint8_t d;
    do {
        d = *data++;
    } while (d & 0x80);
    return data;
}

enum zhe_unpack_result zhe_unpack_rid(uint8_t const * const end, uint8_t const * * const data, zhe_rid_t * restrict u)
{
    /* Not verifying that the RID is less than MAX_RID, only that it fits in zhe_rid_t */
    enum zhe_unpack_result res;
    zhe_rid_t tmp;
    if ((res = SUFFIX_WITH_SIZE(zhe_unpack_vle, ZHE_RID_SIZE)(end, data, &tmp)) != ZUR_OK) {
        return res;
    }
    zhe_assert(!(tmp & 1)); /* while not doing SIDs yet */
    if (u != NULL) {
        *u = tmp >> 1;
    }
    return ZUR_OK;
}

enum zhe_unpack_result zhe_unpack_vec(uint8_t const * const end, uint8_t const * * const data, size_t lim, zhe_paysize_t * restrict u, uint8_t * restrict v)
{
    enum zhe_unpack_result res;
    zhe_paysize_t i;
    if ((res = zhe_unpack_vle16(end, data, u)) != ZUR_OK) {
        return res;
    }
    if (end - *data < *u) {
        return ZUR_SHORT;
    }
    if (*u < lim) {
        lim = *u;
    }
    for (i = 0; i < lim; i++) {
        v[i] = **data;
        (*data)++;
    }
    (*data) += *u - lim;
    return ZUR_OK;
}

enum zhe_unpack_result zhe_unpack_locs(uint8_t const * const end, uint8_t const * * const data, struct unpack_locs_iter *it)
{
    enum zhe_unpack_result res;
    uint16_t n;
    zhe_paysize_t dummy;
    if ((res = zhe_unpack_vle16(end, data, &n)) != ZUR_OK) {
        return res;
    }
    it->n = n;
    it->data = *data;
    while (n--) {
        if ((res = zhe_unpack_vec(end, data, 0, &dummy, NULL)) != ZUR_OK) {
            return res;
        }
    }
    it->end = *data;
    return ZUR_OK;
}

int zhe_unpack_locs_iter(struct unpack_locs_iter *it, zhe_paysize_t *sz, const uint8_t **loc)
{
    /* The only way to get a valid iterator is a successful call to zhe_unpack_locs(), therefore we
       know the structure of the sequence to be valid */
    if (it->n == 0) {
        return 0;
    } else {
        enum zhe_unpack_result x = zhe_unpack_vle16(it->end, &it->data, sz);
        zhe_assert(x == ZUR_OK);
        (void)x;
        *loc = it->data;
        it->data += *sz;
        it->n--;
        return 1;
    }
}

enum zhe_unpack_result zhe_unpack_props(uint8_t const * const end, uint8_t const * * const data)
{
    enum zhe_unpack_result res;
   uint16_t n;
    zhe_paysize_t dummy;
    if ((res = zhe_unpack_vle16(end, data, &n)) != ZUR_OK) {
        return res;
    }
    while (n--) {
        if ((res = zhe_unpack_vec(end, data, 0, &dummy, NULL)) != ZUR_OK ||
            (res = zhe_unpack_vec(end, data, 0, &dummy, NULL)) != ZUR_OK) {
            return res;
        }
    }
    return ZUR_OK;
}
