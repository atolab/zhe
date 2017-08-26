#ifndef ZENO_RID_H
#define ZENO_RID_H

#define ZENO_MAKE_UINT_T1(size) uint##size##_t
#define ZENO_MAKE_UINT_T(size) ZENO_MAKE_UINT_T1(size)
typedef ZENO_MAKE_UINT_T(RID_T_SIZE) rid_t;

#endif
