#ifndef ZHE_BITSET_H
#define ZHE_BITSET_H

#include <stdint.h>

#define DECL_BITSET(name_, size_) uint8_t name_[(size_)+7/8]

unsigned zhe_popcnt8(uint8_t x);
void zhe_bitset_set(uint8_t *s, unsigned idx);
void zhe_bitset_clear(uint8_t *s, unsigned idx);
int zhe_bitset_test(const uint8_t *s, unsigned idx);
unsigned zhe_bitset_count(const uint8_t *s, unsigned size);
int zhe_bitset_findfirst(const uint8_t *s, unsigned size);

#endif /* BITSET_H */
