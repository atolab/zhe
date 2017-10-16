#ifndef ZHE_ICGCB_H
#define ZHE_ICGCB_H

#include <stdint.h>

typedef uint16_t uripos_t; /* FIXME: the usual */

enum icgcb_alloc_result {
    IAR_OK,
    IAR_AGAIN,
    IAR_NOSPACE
};

struct icgcb;

void zhe_icgcb_init(struct icgcb * const b, uripos_t size);
void zhe_icgcb_free(struct icgcb * const b, void * const ptr);
enum icgcb_alloc_result zhe_icgcb_alloc(void ** const ptr, struct icgcb * const b, uripos_t size, uripos_t ref);
void zhe_icgcb_gc(struct icgcb * const b, void (*move_cb)(uripos_t ref, void *newptr, void *arg), void *arg);

#endif
