#ifndef ZHE_TRACING_H
#define ZHE_TRACING_H

#define ZTCAT_ERROR      1u
#define ZTCAT_DEBUG      2u
#define ZTCAT_PEERDISC   4u
#define ZTCAT_TRANSPORT  8u
#define ZTCAT_RELIABLE  16u
#define ZTCAT_PUBSUB    32u

#include "zhe-config.h"

extern unsigned zhe_trace_cats;

#if !(defined ZHE_NTRACE || defined ARDUINO)

void zhe_trace(const char *fmt, ...);

#define ZTT(catsimple_) (zhe_trace_cats & ZTCAT_##catsimple_)
#define ZT(catsimple_, trace_args_) ((zhe_trace_cats & ZTCAT_##catsimple_) ? zhe_trace trace_args_ : (void)0)
#define ZTC(cats_, trace_args_) ((zhe_trace_cats & (cats_)) ? zhe_trace trace_args_ : (void)0)

#else

#define ZTT(catsimple_) (0)
#define ZT(catsimple_, trace_args_) ((void)0)
#define ZTC(cats_, trace_args_) ((void)0)

#endif

#endif
