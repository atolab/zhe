/* -*- mode: c; c-basic-offset: 4; fill-column: 95; -*- */
#ifndef ZENO_CONFIG_H
#define ZENO_CONFIG_H

#include <stdint.h>

/* Size of unsigned integer used to represent a resource/selection
   id internally (externally it is always variable-length encoded) */
#define RID_T_SIZE 32

/* Highest allowed RID (because of temporary direct mapping; also ignoring selection IDs for the moment); also at the moment there are no resource declarations. */
#define MAX_RID 553 /* FIXME: TBD */

/* Maximum number of subscriptions, publications. Having multiple subscriptions per resource may well make sense becasue they have different associated callbacks/arguments, having more than a reliable and an unreliable publication for a single resource (currently) seems unnecessary as no state is (currently) maintained for a publication */
#define MAX_SUBSCRIPTIONS 6
#define MAX_PUBLICATIONS 9

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
