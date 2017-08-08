/* -*- mode: c; c-basic-offset: 4; fill-column: 95; -*- */
#ifndef ZENO_CONFIG_H
#define ZENO_CONFIG_H

#include <stdint.h>

/* Size of unsigned integer used to represent a resource/selection
   id internally (externally it is always variable-length encoded) */
#define RID_T_SIZE 32

#define ZENO_MAKE_UINT_T1(size) uint##size##_t
#define ZENO_MAKE_UINT_T(size) ZENO_MAKE_UINT_T1(size)

typedef ZENO_MAKE_UINT_T(RID_T_SIZE) rid_t;
typedef uint32_t ztime_t;
typedef int32_t ztimediff_t; /* for calculating intervals (may be also be wider than ztime_t) */
typedef uint16_t zpsize_t; /* type used for representing payload sizes (including the length of sequences) */

#endif
