#include "bitset.h"

unsigned popcnt8(uint8_t x)
{
    unsigned n = 0;
    for (n = 0; x; n++) {
        x &= x - 1;
    }
    return n;
}

void bitset_set(uint8_t *s, unsigned idx)
{
    s[idx / 8] |= 1 << (idx % 8);
}

void bitset_clear(uint8_t *s, unsigned idx)
{
    s[idx / 8] &= (uint8_t)~1 << (idx % 8);
}

int bitset_test(const uint8_t *s, unsigned idx)
{
    return (s[idx / 8] & (1 << (idx % 8))) != 0;
}

unsigned bitset_count(const uint8_t *s, unsigned size)
{
    uint8_t i, n = 0;
    for (i = 0; i < (size + 7) / 8; i++) {
        n += popcnt8(s[i]);
    }
    return n;
}

int bitset_findfirst(const uint8_t *s, unsigned size)
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
