#ifndef ZENO_BITSET_H
#define ZENO_BITSET_H

#include <stdint.h>

unsigned popcnt8(uint8_t x);
void bitset_set(uint8_t *s, unsigned idx);
void bitset_clear(uint8_t *s, unsigned idx);
int bitset_test(const uint8_t *s, unsigned idx);
unsigned bitset_count(const uint8_t *s, unsigned size);
int bitset_findfirst(const uint8_t *s, unsigned size);

#endif /* BITSET_H */
