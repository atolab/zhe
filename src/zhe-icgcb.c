#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>

#include "zhe-icgcb.h"

/* Memory layout is:

 ADMIN (SIZE REF ...)+ (0 URIPOS_INVALID)

 ADMIN is the part of struct icgcb up to but not including field "u"
 SIZE  is the size in bytes of the contents of the block (the ...),
       with the block size obtained by rounding SIZE up to a multiple
       of UNIT; and with free blocks always having SIZE a multiple of
       UNIT -- XX and including the header (of size UNIT)
 REF   is a reference value for the user of the allocator to update
       pointers/references whenever the GC moves an allocated block
       it is of the same unsigned type as SIZE and all values but
       URIPOS_INVALID (a.k.a. the max representable value) are allowed

 FIXME: uripos_t, URIPOS_INVALID need to be renamed still!

 The final entry is a sentinel free block of 0 bytes.

 ADMIN tracks total free space, defined as size between ADMIN and
 sentinel, less the used memory including headers, less one header.
 This is used to decide whether an attempted but failed memory allocation
 is worth retrying: if total free space is sufficient, then, barring any
 intervening allocations, eventually the GC will free up the space.

 Allocations always happen at the high end of memory - in "open space",
 and GC starts from the first available block. If first available block
 equals open space, there is no garbage to collect; if the two are
 pointing to the sentinel, all memory is used. */

#define URIPOS_INVALID ((uripos_t)-1)

/* Limits on GC: number of blocks inspected & number of bytes moved */
#define MAX_BLOCKS 10
#define MAX_BYTES  4096

struct hdr {
    uripos_t size;      /* includes header, multiple of UNIT unless allocated; sentinel is exception and has size = 0 */
    uripos_t ref;       /* URIPOS_INVALID if free, else whatever the user puts in */
    unsigned char data[];
};

struct icgcb {
    uripos_t size;
    uripos_t freespace; /* total free space, that is, buf size - allocated (incl. headers) - 1 header */
    uripos_t firstfree; /* index in e[], first free block, GC starts here */
    uripos_t openspace; /* index in e[], nothing to the end of the buffer, allocations happen here */
    uripos_t sentinel;  /* index in e[] of sentinel at the end of memory */
    union {
        struct hdr e;   /* really e[] ... C is so overrated ... */
        char buf;       /* really buf[] ... */
    } u;
};

#define UNIT (sizeof(struct hdr))
struct static_assertions {
    /* if UNIT is not a power of two, bitmasking won't work and alignmnet and rounding calculations need to be rewritten */
    char UNIT_is_power_of_two[(UNIT & -UNIT) == UNIT ? 1 : -1];
};

static uripos_t alignup(uripos_t size)
{
    return (size + UNIT - 1) & ~(UNIT - 1);
}

static void check(struct icgcb const * const b)
{
    uripos_t idx = 0, freespace = 0;
    assert(b->firstfree <= b->openspace);
    assert(b->openspace <= b->sentinel);
    assert(b->freespace <= b->size - offsetof(struct icgcb, u));
    assert((&b->u.e)[b->sentinel].ref == URIPOS_INVALID);
    assert((&b->u.e)[b->sentinel].size == 0);
    assert((&b->u.e)[b->firstfree].ref == URIPOS_INVALID);
    assert(((&b->u.e)[b->firstfree].size % UNIT) == 0);
    assert((&b->u.e)[b->openspace].ref == URIPOS_INVALID);
    assert(((&b->u.e)[b->openspace].size % UNIT) == 0);
    assert(b->openspace == b->sentinel || ((&b->u.e)[b->openspace].size > 0));
    while (idx < b->firstfree) {
        struct hdr const * const e = &b->u.e + idx;
        assert(e->ref != URIPOS_INVALID);
        assert(e->size > UNIT);
        idx = idx + alignup(e->size) / UNIT;
    }
    assert(idx == b->firstfree);
    while (idx < b->openspace) {
        struct hdr const * const e = &b->u.e + idx;
        assert(e->size > UNIT && (e->ref != URIPOS_INVALID || (e->size % UNIT) == 0));
        if (e->ref == URIPOS_INVALID) {
            freespace += e->size;
        }
        idx = idx + alignup(e->size) / UNIT;
    }
    assert(idx == b->openspace);
    freespace += (&b->u.e)[idx].size;
    idx += (&b->u.e)[idx].size / UNIT;
    assert(idx == b->sentinel);
    assert(b->freespace == freespace);
}

void zhe_icgcb_init(struct icgcb * const b, uripos_t size)
{
    assert(size < (URIPOS_INVALID & ~(UNIT - 1)) - UNIT);
    b->freespace = (uripos_t)(size - offsetof(struct icgcb, u) - UNIT) & ~(UNIT - 1);
    b->size      = b->freespace + offsetof(struct icgcb, u) + UNIT;
    b->firstfree = 0;
    b->openspace = 0;
    b->sentinel  = b->freespace / UNIT;
    b->u.e.size  = b->freespace;
    b->u.e.ref   = URIPOS_INVALID;
    struct hdr * const s = &b->u.e + b->sentinel;
    s->size = 0;
    s->ref  = URIPOS_INVALID;
    check(b);
}

void zhe_icgcb_free(struct icgcb * const b, void * const ptr)
{
    assert((char *)ptr >= (char *)(&b->u.e + 1) && (char *)ptr < (char*)b + b->size);
    struct hdr * const e = (struct hdr *)ptr - 1;
    uripos_t hdrpos = (uripos_t)(e - &b->u.e);
    e->ref = URIPOS_INVALID;
    e->size = alignup(e->size);
    b->freespace += e->size;
    if (hdrpos < b->firstfree) {
        b->firstfree = hdrpos;
    }
    check(b);
}

enum icgcb_alloc_result zhe_icgcb_alloc(void ** const ptr, struct icgcb * const b, uripos_t size, uripos_t ref)
{
    enum icgcb_alloc_result res;
    assert(0 < size && size < b->size);
    assert((b->freespace % UNIT) == 0);
    /* block sizes include header size, so for the allocator we're allocating size + UNIT */
    const uripos_t sizeA = UNIT + alignup(size);
    if (b->freespace < sizeA) {
        res = IAR_NOSPACE;
    } else if ((&b->u.e)[b->openspace].size < sizeA) {
        res = IAR_AGAIN;
    } else {
        struct hdr * const e = &b->u.e + b->openspace;
        const bool firstfree_is_openspace = (b->firstfree == b->openspace);
        assert(e->size > UNIT && (e->size % UNIT) == 0);
        if (e->size - sizeA < UNIT) {
            b->openspace = b->sentinel;
        } else {
            struct hdr * const ne = e + sizeA / UNIT;
            ne->size = e->size - sizeA;
            ne->ref = URIPOS_INVALID;
            b->openspace = (uripos_t)(ne - &b->u.e);
        }
        if (firstfree_is_openspace) {
            b->firstfree = b->openspace;
        }
        b->freespace -= sizeA;
        e->size = UNIT + size;
        e->ref = ref;
        *ptr = e->data;
        res = IAR_OK;
    }
    check(b);
    return res;
}

void zhe_icgcb_gc(struct icgcb * const b, void (*move_cb)(uripos_t ref, void *newptr, void *arg), void *arg)
{
    uripos_t blocks = 0, bytes = 0;
    while (b->firstfree != b->openspace && blocks++ < MAX_BLOCKS && bytes < MAX_BYTES) {
        struct hdr * const e = &b->u.e + b->firstfree;
        struct hdr * const ne = e + e->size / UNIT;
        if (ne->ref != URIPOS_INVALID) {
            /* next is in use, shift it */
            const uripos_t esize = e->size;
            struct hdr * const nne = e + alignup(ne->size) / UNIT;
            bytes += ne->size;
            memmove(e, ne, ne->size);
            nne->size = esize;
            nne->ref = URIPOS_INVALID;
            b->firstfree = (uripos_t)(nne - &b->u.e);
            move_cb(e->ref, e->data, arg);
        } else {
            /* next is some free block, coalesce, adjust openspace if needed; if next is sentinel,
               ne->size = 0 and coalescing is a no-op */
            assert((ne->size == 0) == (ne == &b->u.e + b->sentinel));
            e->size += ne->size;
            if (ne == &b->u.e + b->openspace) {
                b->openspace = b->firstfree;
            }
        }
        check(b);
    }
}
