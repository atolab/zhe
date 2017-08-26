#ifndef ZHE_RID_H
#define ZHE_RID_H

#define ZHE_MAKE_UINT_T1(size) uint##size##_t
#define ZHE_MAKE_UINT_T(size) ZHE_MAKE_UINT_T1(size)
typedef ZHE_MAKE_UINT_T(ZHE_RID_SIZE) zhe_rid_t;

#endif
