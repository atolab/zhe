#include <time.h>
#include "zeno-time.h"

static struct timespec toffset;

void zeno_time_init(void)
{
    (void)clock_gettime(CLOCK_MONOTONIC, &toffset);
    toffset.tv_sec -= toffset.tv_sec % 10000;
}

ztime_t zeno_time(void)
{
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (ztime_t)((t.tv_sec - toffset.tv_sec) * (1000000000 / ZENO_TIMEBASE) + t.tv_nsec / ZENO_TIMEBASE);
}
