#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>

#include "zeno-tracing.h"
#include "zeno-time.h"
#include "zeno-config-deriv.h"

unsigned zeno_trace_cats;

#ifndef ARDUINO

void zeno_trace(const char *fmt, ...)
{
    uint32_t t = (uint32_t)zeno_time();
    va_list ap;
    va_start(ap, fmt);
    flockfile(stdout);
    printf("%4"PRIu32".%03"PRIu32" ", ZTIME_TO_SECu32(t), ZTIME_TO_MSECu32(t));
    (void)vprintf(fmt, ap);
    printf("\n");
    funlockfile(stdout);
    va_end(ap);
}

#endif
