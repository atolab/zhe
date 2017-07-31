#include <assert.h>
#include "binheap.h"

/* so can safely use "native" unsigneds for indexing */
struct static_assertions {
    char sizeof_unsigned_geq_sizeof_peeridx[sizeof(unsigned) >= sizeof(peeridx_t) ? 1 : -1];
};

/* FIXME: this is a copy of zeno.c:seq_lt */
static int seq_lt(seq_t a, seq_t b)
{
    return (sseq_t) (a - b) < 0;
}

static void minseqheap_heapify(unsigned j, peeridx_t n, peeridx_t *p, const seq_t *v)
{
    unsigned k;
    for (k = 2*j+1; k < n; j = k, k += k + 1) {
        if (k+1 < n && seq_lt(v[p[k+1]], v[p[k]])) {
            k++;
        }
        if (seq_lt(v[p[k]], v[p[j]])) {
            peeridx_t t;
            t = p[j]; p[j] = p[k]; p[k] = t;
        }
    }
}

#if 0
void minseqheap_build(peeridx_t n, peeridx_t *permute, const seq_t *values)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        permute[i] = i;
    }
    i = n / 2;
    while (i > 0) {
        minseqheap_heapify(--i, n, permute, values);
    }
}
#endif

void minseqheap_insert(peeridx_t k, peeridx_t *n, peeridx_t *permute, const seq_t *values)
{
    unsigned i = (*n)++;
    while (i > 0 && seq_lt(values[k], values[permute[(i-1)/2]])) {
        permute[i] = permute[(i-1)/2];
        i = (i-1)/2;
    }
    permute[i] = k;
}

seq_t minseqheap_get_min(peeridx_t n, const peeridx_t *permute, const seq_t *values)
{
    assert (n > 0);
    return values[permute[0]];
}

#if 0
void minseqheap_decreased_key(peeridx_t i, peeridx_t *permute, const seq_t *values)
{
    if (i > 0 && seq_lt(values[permute[i]], values[permute[(i-1)/2]])) {
        peeridx_t k = permute[i];
        seq_t v = values[k];
        do {
            permute[i] = permute[(i-1)/2];
            i = (i-1)/2;
        } while (i > 0 && seq_lt(v, values[permute[(i-1)/2]]));
        permute[i] = k;
    }
}

seq_t minseqheap_extract_min(peeridx_t *n, peeridx_t *permute, const seq_t *values)
{
    seq_t min;
    assert (*n > 0);
    min = values[permute[0]];
    (*n)--;
    permute[0] = permute[*n];
    minseqheap_heapify(0, *n, permute, values);
    return min;
}
#endif

seq_t minseqheap_increased_key(peeridx_t i, peeridx_t n, peeridx_t *permute, const seq_t *values)
{
    minseqheap_heapify(0, n, permute, values);
    return values[permute[0]];
}

void minseqheap_delete(peeridx_t i, peeridx_t *n, peeridx_t *permute, const seq_t *values)
{
    (*n)--;
    permute[i] = permute[*n];
    minseqheap_heapify(0, *n, permute, values);
}
