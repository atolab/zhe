#ifndef ZENO_BINHEAP_H
#define ZENO_BINHEAP_H

#include "zeno-config-int.h"

typedef struct { peeridx_t i; } minseqheap_idx_t;

struct minseqheap {
    peeridx_t n;              /* number of peers "attached" to mconduit, valid hx indices in 0 .. n-1 */
    seq_t vs[MAX_PEERS];      /* oc.seqbase = min(vs), vs[i] = next unacked for peer i */
    peeridx_t hx[MAX_PEERS];  /* vs[hx[i]] <= vs[hx[2i+1]] && vs[hx[i]] <= vs[hx[2i+2]] */
    minseqheap_idx_t ix[MAX_PEERS]; /* (ix[i].i != X => hx[ix[i].i] == i) && (ix[i].i == X => !exist(hx[k] s.t. hx[k] == i) */
};

int minseqheap_isempty(struct minseqheap const * const h);
void minseqheap_insert(peeridx_t peeridx, seq_t seqbase, struct minseqheap * const h);
seq_t minseqheap_update_seq(peeridx_t peeridx, seq_t seqbase, seq_t seqbase_if_discarded, struct minseqheap * const h);
int minseqheap_delete(peeridx_t peeridx, struct minseqheap * const h);
seq_t minseqheap_get_min(struct minseqheap const * const h);

#endif
