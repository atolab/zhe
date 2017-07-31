#ifndef ZENO_BINHEAP_H
#define ZENO_BINHEAP_H

#include "zeno-config-int.h"

void minseqheap_insert(peeridx_t k, peeridx_t *n, peeridx_t *permute, const seq_t *values);
seq_t minseqheap_increased_key(peeridx_t i, peeridx_t n, peeridx_t *permute, const seq_t *values);
void minseqheap_delete(peeridx_t i, peeridx_t *n, peeridx_t *permute, const seq_t *values);
seq_t minseqheap_get_min(peeridx_t n, const peeridx_t *permute, const seq_t *values);

#endif
