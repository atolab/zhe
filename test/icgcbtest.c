#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <string.h>
#include "zhe-icgcb.h"

#define N 100
#define S 500

static void randomX (uripos_t *size, uripos_t *slot)
{
  *size = (uripos_t)(1 + (random() % S));
  *slot = (uripos_t)(random() % N);
}

static uripos_t alignup(uripos_t size)
{
  return (size + 4 - 1) & ~(4 - 1);
}

static void move_cb (uripos_t ref, void *newp, void *varg)
{
  void **bb = varg;
  assert (bb[ref]);
  bb[ref] = newp;
}

#define UNIT 4
#define ICGCBSIZE 10

int main ()
{
    char buf[10240];
    struct icgcb *b = (struct icgcb *)buf;
#ifndef NDEBUG
    const uripos_t bufsz = ((sizeof(buf) - (ICGCBSIZE + UNIT)) & -(size_t)UNIT);
#endif
    void *bb[N];
    uripos_t bbsize[N];
    uripos_t size, slot;
    size_t inuse = 0;
    int again = 0, maxagain = 0;
    unsigned long ops = 0, agains = 0, nospaces = 0;
    clock_t tnow, tnextprint, tstart;
    srandomdev ();
    for (int i = 0; i < N; i++)
    {
        bb[i] = NULL;
        bbsize[i] = 0;
    }
    zhe_icgcb_init(b, sizeof (buf));
    randomX(&size, &slot);
    tstart = tnow = clock();
    tnextprint = tstart + CLOCKS_PER_SEC;
    while (tnow < tstart + 100 * CLOCKS_PER_SEC) {
        ops++;
        if ((ops % 1024) == 0 && (tnow = clock()) > tnextprint) {
            printf("%lu maxagain %d again %lu nospace %lu\n", ops, maxagain, agains, nospaces);
            maxagain = 0;
            ops = agains = nospaces = 0;
            tnextprint = tnow + CLOCKS_PER_SEC;
        }
        if (bb[slot] == NULL) {
            enum icgcb_alloc_result res = zhe_icgcb_alloc(&bb[slot], b, size, slot);
            switch (res) {
                case IAR_OK:
                    memset(bb[slot], slot % 256, size);
                    bbsize[slot] = size;
                    inuse += 4 + alignup(bbsize[slot]);
                    again = 0;
                    randomX(&size, &slot);
                    break;
                case IAR_AGAIN:
                    assert(inuse + alignup(size) + UNIT <= bufsz);
                    again++;
                    agains++;
                    assert(again < 100);
                    if (again > maxagain) {
                        maxagain = again;
                    }
                    break;
                case IAR_NOSPACE:
                    assert(inuse + alignup(size) + UNIT > bufsz);
                    again = 0;
                    nospaces++;
                    randomX(&size, &slot);
                    break;
            }
        } else {
            for (uripos_t i = 0; i < bbsize[slot]; i++) {
                assert(((unsigned char *)bb[slot])[i] == slot % 256);
            }
            zhe_icgcb_free(b, bb[slot]);
            inuse -= 4 + alignup(bbsize[slot]);
            bb[slot] = NULL;
        }
        zhe_icgcb_gc(b, move_cb, bb);
    }
}
