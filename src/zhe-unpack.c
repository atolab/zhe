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
    *u = **data;
    *data += 1;
    return ZUR_OK;
}

/* zhe_unpack_vle_..._overflow is the fallback routine once we know there is overflow (we're simply declaring padding with 0s at the most-significant end is not allowed) */
#define DEF_UNPACK_VLE_OVERFLOW(size_)                                  \
    enum zhe_unpack_result zhe_unpack_vle##size_##_overflow(uint8_t const * const end, uint8_t const * * const data, uint##size_##_t * restrict u) \
    {                                                                   \
        uint8_t const * const start = *data;                            \
        const uint8_t *c = start;                                       \
        while (c != end && *c > 0x7f) {                                 \
            c++;                                                        \
        }                                                               \
        if (c == end) {                                                 \
            return ZUR_SHORT;                                           \
        }                                                               \
        *data = c+1;                                                    \
        uint##size_##_t n = *c;                                         \
        while (c-- != start) {                                          \
            n = (uint##size_##_t)(n << 7) | (*c & 0x7f);                \
        }                                                               \
        *u = n;                                                         \
        return ZUR_OVERFLOW;                                            \
    }
DEF_UNPACK_VLE_OVERFLOW(8)
DEF_UNPACK_VLE_OVERFLOW(16)
DEF_UNPACK_VLE_OVERFLOW(32)

#define ADD_SEPTET(size_, msb_, lsb_) ((uint##size_##_t)(((uint##size_##_t)msb_ << 7) | (lsb_ & 0x7f)))

enum zhe_unpack_result zhe_unpack_vle8(uint8_t const * const end, uint8_t const * * const data, uint8_t * restrict u)
{
    const uint8_t *c = *data;
    if (c+0 == end) {
        return ZUR_SHORT;
    } else if (c[0] <= 0x7f) {
        *u = c[0];
        (*data) += 1;
        return ZUR_OK;
    } else if (c+1 == end) {
        return ZUR_SHORT;
    } else if (c[1] <= 0x1) {
        *u = ADD_SEPTET(8, c[1], c[0]);
        (*data) += 2;
        return ZUR_OK;
    } else {
        return zhe_unpack_vle8_overflow(end, data, u);
    }
}

enum zhe_unpack_result zhe_unpack_vle16(uint8_t const * const end, uint8_t const * * const data, uint16_t * restrict u)
{
    const uint8_t *c = *data;
    if (c+0 == end) {
        return ZUR_SHORT;
    } else if (c[0] <= 0x7f) {
        *u = c[0];
        (*data) += 1;
        return ZUR_OK;
    } else if (c+1 == end) {
        return ZUR_SHORT;
    } else if (c[1] <= 0x7f) {
        *u = ADD_SEPTET(16, c[1], c[0]);
        (*data) += 2;
        return ZUR_OK;
    } else if (c+2 == end) {
        return ZUR_SHORT;
    } else if (c[2] <= 0x3) {
        *u = ADD_SEPTET(16, ADD_SEPTET(16, c[2], c[1]), c[0]);
        (*data) += 3;
        return ZUR_OK;
    } else {
        return zhe_unpack_vle16_overflow(end, data, u);
    }
}

enum zhe_unpack_result zhe_unpack_vle32(uint8_t const * const end, uint8_t const * * const data, uint32_t * restrict u)
{
    const uint8_t *c = *data;
    if (c+0 == end) {
        return ZUR_SHORT;
    } else if (c[0] <= 0x7f) {
        *u = c[0];
        (*data) += 1;
        return ZUR_OK;
    } else if (c+1 == end) {
        return ZUR_SHORT;
    } else if (c[1] <= 0x7f) {
        *u = ADD_SEPTET(32, c[1], c[0]);
        (*data) += 2;
        return ZUR_OK;
    } else if (c+2 == end) {
        return ZUR_SHORT;
    } else if (c[2] <= 0x7f) {
        *u = ADD_SEPTET(32, ADD_SEPTET(32, c[2], c[1]), c[0]);
        (*data) += 3;
        return ZUR_OK;
    } else if (c+3 == end) {
        return ZUR_SHORT;
    } else if (c[3] <= 0x7f) {
        *u = ADD_SEPTET(32, ADD_SEPTET(32, ADD_SEPTET(32, c[3], c[2]), c[1]), c[0]);
        (*data) += 4;
        return ZUR_OK;
    } else if (c+4 == end) {
        return ZUR_SHORT;
    } else if (c[4] <= 0xf) {
        *u = ADD_SEPTET(32, ADD_SEPTET(32, ADD_SEPTET(32, ADD_SEPTET(32, c[4], c[3]), c[2]), c[1]), c[0]);
        (*data) += 5;
        return ZUR_OK;
    } else {
        return zhe_unpack_vle32_overflow(end, data, u);
    }
}

#undef ADD_SEPTET

#if ZHE_RID_SIZE > 32 || SEQNUM_LEN > 28
/* 64-bit case is fairly rare, and I'm ok with it being a bit slower in return for being a bit smaller */
enum zhe_unpack_result zhe_unpack_vle64(uint8_t const * const end, uint8_t const * * const data, uint64_t * restrict u)
{
    uint8_t const * const start = *data;
    const uint8_t *c = start;
    while (c != end && *c > 0x7f) {
        c++;
    }
    if (c == end) {
        return ZUR_SHORT;
    }
    *data = c+1;
    bool overflow;
    if (c+1 - start < (8*sizeof(*u)+6)/7) {
        overflow = false;
    } else if (c+1 - start == (8*sizeof(*u)+6)/7) {
        overflow = (((8*sizeof(*u)) % 7) == 0) ? false : (*c >> ((8*sizeof(*u)) % 7) != 0);
    } else {
        overflow = true;
    }
    uint64_t n = *c;
    while (c-- != start) {
        n = (uint64_t)(n << 7) | (*c & 0x7f);
    }
    *u = n;
    return (overflow ? ZUR_OVERFLOW : ZUR_OK);
}
#endif

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
    if ((res = SUFFIX_WITH_SIZE(zhe_unpack_vle, ZHE_RID_SIZE)(end, data, u)) != ZUR_OK) {
        return res;
    }
    zhe_assert(!(*u & 1)); /* while not doing SIDs yet */
    *u >>= 1;
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

enum zhe_unpack_result zhe_unpack_vecref(uint8_t const * const end, uint8_t const * * const data, zhe_paysize_t *u, uint8_t const **v)
{
    enum zhe_unpack_result res;
    if ((res = zhe_unpack_vle16(end, data, u)) != ZUR_OK) {
        return res;
    }
    if (end - *data < *u) {
        return ZUR_SHORT;
    }
    *v = *data;
    (*data) += *u;
    return ZUR_OK;
}

enum zhe_unpack_result zhe_unpack_locs(uint8_t const * const end, uint8_t const * * const data, struct unpack_locs_iter *it)
{
    enum zhe_unpack_result res;
    uint16_t n;
    zhe_paysize_t dummy;
    const uint8_t *dummydata;
    if ((res = zhe_unpack_vle16(end, data, &n)) != ZUR_OK) {
        return res;
    }
    it->n = n;
    it->data = *data;
    while (n--) {
        if ((res = zhe_unpack_vecref(end, data, &dummy, &dummydata)) != ZUR_OK) {
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

enum zhe_unpack_result zhe_unpack_props(uint8_t const * const end, uint8_t const * * const data, struct unpack_props_iter *it)
{
    enum zhe_unpack_result res;
    uint16_t n;
    uint8_t propid;
    zhe_paysize_t dummy;
    const uint8_t *dummydata;
    if ((res = zhe_unpack_vle16(end, data, &n)) != ZUR_OK) {
        return res;
    }
    it->n = n;
    it->data = *data;
    while (n--) {
        /* overflow is ok on property ids, we don't interpret them, but we can skip them */
        if ((res = zhe_unpack_vle8(end, data, &propid)) != ZUR_OK && res != ZUR_OVERFLOW) {
            return res;
        }
        if ((res = zhe_unpack_vecref(end, data, &dummy, &dummydata)) != ZUR_OK) {
            return res;
        }
    }
    it->end = *data;
    return ZUR_OK;
}

int zhe_unpack_props_iter(struct unpack_props_iter *it, uint8_t *propid, zhe_paysize_t *sz, const uint8_t **data)
{
    /* The only way to get a valid iterator is a successful call to zhe_unpack_props(), therefore we
       know the structure of the sequence to be valid; skips props with out-of-range propids */
    while (it->n != 0) {
        enum zhe_unpack_result res;
        res = zhe_unpack_vle8(it->end, &it->data, propid);
        zhe_assert(res == ZUR_OK || res == ZUR_OVERFLOW);
        (void)zhe_unpack_vecref(it->end, &it->data, sz, data);
        it->n--;
        if (res == ZUR_OK) {
            return 1;
        }
    }
    return 0;
}
