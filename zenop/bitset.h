#ifndef ZENO_BITSET_H
#define ZENO_BITSET_H

#include <stdint.h>

uint8_t popcnt8(uint8_t x);
void bitset_set(uint8_t *s, uint8_t idx);
void bitset_clear(uint8_t *s, uint8_t idx);
int bitset_test(const uint8_t *s, uint8_t idx);
uint8_t bitset_count(const uint8_t *s, uint8_t size);
int bitset_findfirst(const uint8_t *s, uint8_t size);

#endif /* BITSET_H */
