#include "zhe-bitset.h"

static unsigned zhe_popcnt8(uint8_t x)
{
    unsigned n = 0;
    for (n = 0; x; n++) {
        x &= x - 1;
    }
    return n;
}

void zhe_bitset_set(uint8_t *s, unsigned idx)
{
    s[idx / 8] |= 1 << (idx % 8);
}

void zhe_bitset_clear(uint8_t *s, unsigned idx)
{
    s[idx / 8] &= (uint8_t)~(1 << (idx % 8));
}

int zhe_bitset_test(const uint8_t *s, unsigned idx)
{
    return (s[idx / 8] & (1 << (idx % 8))) != 0;
}

unsigned zhe_bitset_count(const uint8_t *s, unsigned size)
{
    uint8_t i, n = 0;
    for (i = 0; i < (size + 7) / 8; i++) {
        n += zhe_popcnt8(s[i]);
    }
    return n;
}

int zhe_bitset_findfirst(const uint8_t *s, unsigned size)
{
    unsigned i, j, m;
    for (i = 0; i < (size + 7) / 8; i++) {
        if (s[i] == 0) {
            continue;
        }
        for (j = 0, m = 1; j < 8; j++, m <<= 1) {
            if (s[i] & m) {
                return (int)(8 * i + j);
            }
        }
    }
    return -1;
}

void zhe_bitset_iter_init(bitset_iter_t *it, const uint8_t *s, unsigned size)
{
    it->s = s;
    it->size = size;
    it->cursor = 0;
}

bool zhe_bitset_iter_next(bitset_iter_t *it, unsigned *idx)
{
    while (it->cursor < it->size && !zhe_bitset_test(it->s, it->cursor)) {
        it->cursor++;
    }
    *idx = it->cursor;
    return it->cursor++ < it->size;
}
