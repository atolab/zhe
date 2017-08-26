#include <time.h>
#include "zhe-time.h"

static struct timespec toffset;

void zhe_time_init(void)
{
    (void)clock_gettime(CLOCK_MONOTONIC, &toffset);
    toffset.tv_sec -= toffset.tv_sec % 10000;
}

zhe_time_t zhe_time(void)
{
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (zhe_time_t)((t.tv_sec - toffset.tv_sec) * (1000000000 / ZHE_TIMEBASE) + t.tv_nsec / ZHE_TIMEBASE);
}
