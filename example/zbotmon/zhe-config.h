/* -*- mode: c; c-basic-offset: 4; fill-column: 95; -*- */
#ifndef ZHE_CONFIG_H
#define ZHE_CONFIG_H

#include <stdint.h>

#define ZHE_MAX_RID 65535
#define ZHE_MAX_SUBSCRIPTIONS 10
#define ZHE_MAX_PUBLICATIONS 10
#define ZHE_MAX_RESOURCES 0
#define ZHE_MAX_URILENGTH 0

/* Types for representing timestamps (with an arbitrary reference,
   no assumed time alignment, and roll-over perfectly acceptable),
   and the difference of two timestamps (which are, at least in
   principle, limited by the interval with which zhe_loop() is
   called and the time intervals configured in zeno-config-int.h
   and/or used elsewhere. Note that zhe_timediff_t may be wider than
   zhe_time_t. */
typedef uint32_t zhe_time_t;
typedef int32_t zhe_timediff_t;

/* Maximum representable time difference, limiting lease durations.
   Peers that specify a lease duration longer than ZTIMEDIFF_MAX
   are, quite simply, ignored. */
#define ZHE_TIMEDIFF_MAX INT32_MAX

/* The unit of zhe_time_t / zhe_timediff_t represents this many nanoseconds */
#define ZHE_TIMEBASE 1000000

/* Type used for representing payload sizes and sequence lengths (FIXME: not quite completely parameterised yet) */
typedef uint16_t zhe_paysize_t;

#endif
