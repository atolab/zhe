#ifndef ZENO_TRACING_H
#define ZENO_TRACING_H

#define ZTCAT_ERROR      1u
#define ZTCAT_DEBUG      2u
#define ZTCAT_PEERDISC   4u
#define ZTCAT_TRANSPORT  8u

#include "zeno-config.h"

extern unsigned zeno_trace_cats;

void zeno_trace(const char *fmt, ...);

#define ZT(catsimple_, trace_args_) ((zeno_trace_cats & ZTCAT_##catsimple_) ? zeno_trace trace_args_ : (void)0)
#define ZTC(cats_, trace_arsg_) ((zeno_trace_cats & (cats_)) ? zeno_trace trace_args_ : (void)0)

#endif
