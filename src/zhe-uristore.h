#ifndef ZHE_URISTORE_H
#define ZHE_URISTORE_H

#if ZHE_MAX_URISPACE > 0
#include <stdbool.h>
#include "zhe-icgcb.h"

#if ZHE_MAX_RESOURCES <= UINT8_MAX-1
typedef uint8_t zhe_residx_t;
#elif ZHE_MAX_RESOURCES <= UINT16_MAX-1
typedef uint16_t zhe_residx_t;
#elif ZHE_MAX_RESOURCES <= UINT32_MAX-1
typedef uint32_t zhe_residx_t;
#endif

enum uristore_result {
    USR_OK,       /* stored */
    USR_AGAIN,    /* not stored, but GC will eventually free up space (barring any intervening allocations) */
    USR_NOSPACE,  /* not stored, insufficient total free space */
    USR_MISMATCH, /* not stored, different URI known for this RID */
    USR_OVERSIZE  /* URI is longer than supported */
};

void zhe_uristore_init(void);
void zhe_uristore_gc(void);
#define URISTORE_PEERIDX_SELF MAX_PEERS
enum uristore_result zhe_uristore_store(peeridx_t peeridx, zhe_rid_t rid, const uint8_t *uri, size_t urilen_in);
void zhe_uristore_drop(peeridx_t peeridx, zhe_rid_t rid);
void zhe_uristore_reset_peer(peeridx_t peeridx);
/* FIXME: need a proper type for the cursor */
bool zhe_uristore_geturi(unsigned idx, zhe_rid_t *rid, zhe_paysize_t *sz, const uint8_t **uri);
#endif

#endif
