#include <assert.h>
#include "binheap.h"
#include "zeno-config-int.h"

/* for seq_l() */
#include "zeno.h"
#include "zeno-int.h"

static void minseqheap_heapify(peeridx_t j, peeridx_t n, peeridx_t * restrict p, minseqheap_idx_t * restrict q, const seq_t * restrict v)
{
    peeridx_t k;
    for (k = 2*j+1; k < n; j = k, k += k + 1) {
        if (k+1 < n && seq_lt(v[p[k+1]], v[p[k]])) {
            k++;
        }
        if (seq_lt(v[p[k]], v[p[j]])) {
            peeridx_t t;
            t = p[j]; p[j] = p[k]; p[k] = t;
            q[p[j]].i = j; q[p[k]].i = k;
        }
    }
}

void minseqheap_insert(peeridx_t peeridx, seq_t seqbase, struct minseqheap * const h)
{
    peeridx_t i;
#ifndef NDEBUG
    for (peeridx_t j = 0; j < h->n; j++) {
        assert(h->hx[j] != peeridx);
    }
#endif
    h->vs[peeridx] = seqbase;
    i = h->n++;
    while (i > 0 && seq_lt(h->vs[peeridx], h->vs[h->hx[(i-1)/2]])) {
        h->hx[i] = h->hx[(i-1)/2];
        h->ix[h->hx[i]].i = i;
        i = (i-1)/2;
    }
    h->hx[i] = peeridx;
    h->ix[h->hx[i]].i = i;
}

seq_t minseqheap_get_min(struct minseqheap const * const h)
{
    assert (h->n > 0);
    return h->vs[h->hx[0]];
}

seq_t minseqheap_update_seq(peeridx_t peeridx, seq_t seqbase, seq_t seqbase_if_discarded, struct minseqheap * const h)
{
    /* peeridx must be contained in heap and new seqbase must be >= h->vs[peeridx] */
    if (h->ix[peeridx].i == PEERIDX_INVALID || seq_le(seqbase, h->vs[peeridx])) {
        return seqbase_if_discarded;
    } else {
        assert(h->hx[h->ix[peeridx].i] == peeridx);
        h->vs[peeridx] = seqbase;
        minseqheap_heapify(h->ix[peeridx].i, h->n, h->hx, h->ix, h->vs);
        return h->vs[h->hx[0]];
    }
}

int minseqheap_delete(peeridx_t peeridx, struct minseqheap * const h)
{
    /* returns 0 if peeridx not contained in heap; 1 if it is contained */
    const peeridx_t i = h->ix[peeridx].i;
    if (i == PEERIDX_INVALID) {
#ifndef NDEBUG
        for (peeridx_t j = 0; j < h->n; j++) {
            assert(h->hx[j] != peeridx);
        }
#endif
        return 0;
    } else {
        assert(h->hx[i] == peeridx);
        h->ix[peeridx].i = PEERIDX_INVALID;
        h->hx[i] = h->hx[--h->n];
        h->ix[h->hx[i]].i = i;
        minseqheap_heapify(i, h->n, h->hx, h->ix, h->vs);
        return 1;
    }
}

int minseqheap_isempty(struct minseqheap const * const h)
{
    return h->n == 0;
}
