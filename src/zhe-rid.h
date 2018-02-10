#ifndef ZHE_RID_H
#define ZHE_RID_H

#if ZHE_MAX_RID <= UINT8_MAX/2
#define ZHE_RID_SIZE 8
typedef uint8_t zhe_rid_t;
#elif ZHE_MAX_RID <= UINT16_MAX/2
#define ZHE_RID_SIZE 16
typedef uint16_t zhe_rid_t;
#elif ZHE_MAX_RID <= UINT32_MAX/2
#define ZHE_RID_SIZE 32
typedef uint32_t zhe_rid_t;
#elif ZHE_MAX_RID <= UINT64_MAX/2
#define ZHE_RID_SIZE 64
typedef uint64_t zhe_rid_t;
#else
#error "ZHE_MAX_RID out of range"
#endif

/* FIXME: I am of the opinion that a client can rely on the broker to track all this, but perhaps others have different ideas */
#if ZHE_MAX_URISPACE > 0 && MAX_PEERS > 0
#if ZHE_MAX_RESOURCES > UINT64_MAX/MAX_PEERS - 1
#error "ZHE_MAX_RESOURCES & MAX_PEERS conspire to push ZHE_MAX_RSUBCOUNT out of range"
#endif
#define ZHE_MAX_RSUBCOUNT (ZHE_MAX_RESOURCES * MAX_PEERS)
#if ZHE_MAX_RSUBCOUNT <= UINT8_MAX
typedef uint8_t zhe_rsubcount_t;
#elif ZHE_MAX_RSUBCOUNT <= UINT16_MAX
typedef uint16_t zhe_rsubcount_t;
#elif ZHE_MAX_RSUBCOUNT <= UINT32_MAX
typedef uint32_t zhe_rsubcount_t;
#else
typedef uint64_t zhe_rsubcount_t;
#endif
#endif

#endif
