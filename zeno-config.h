/* -*- mode: c; c-basic-offset: 4; fill-column: 95; -*- */
#ifndef ZENO_CONFIG_H
#define ZENO_CONFIG_H

#include <stdint.h>

/* Size of unsigned integer used to represent a resource/selection
   id internally (externally it is always variable-length encoded) */
#define RID_T_SIZE 32

/* Types for representing timestamps (with an arbitrary reference,
   no assumed time alignment, and roll-over perfectly acceptable),
   and the difference of two timestamps (which are, at least in
   principle, limited by the interval with which zeno_loop() is
   called and the time intervals configured in zeno-config-int.h
   and/or used elsewhere. Note that ztimediff_t may be wider than
   ztime_t. */
typedef uint32_t ztime_t;
typedef int32_t ztimediff_t;

/* Maximum representable time difference, limiting lease durations.
   Peers that specify a lease duration longer than ZTIMEDIFF_MAX
   are, quite simply, ignored. */
#define ZTIMEDIFF_MAX INT32_MAX

/* The unit of ztime_t / ztimediff_t represents this many nanoseconds */
#define ZENO_TIMEBASE 1000000

/* Type used for representing payload sizes and sequence lengths (FIXME: not quite completely parameterised yet) */
typedef uint16_t zpsize_t;

#endif
