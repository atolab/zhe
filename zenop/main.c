//
//  main.c
//  zenop
//
//  Created by Erik Boasson on 20-06-2017.
//  Copyright © 2017 ADLINK ATO. All rights reserved.
//

#include <stdio.h>
#include <time.h>

#include "zeno.h"

static struct timespec toffset;

static ztime_t now(void)
{
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (ztime_t)((t.tv_sec - toffset.tv_sec) * 1000 + t.tv_nsec / 1000000);
}

int main(int argc, const char **argv)
{
    (void)clock_gettime(CLOCK_MONOTONIC, &toffset);
    (void)zeno_init();
    zeno_loop_init(now());
    do {
        const struct timespec sl = { 0, 10000000 };
        zeno_loop(now());
        nanosleep(&sl, NULL);
    } while(now() < 20000);
    return 0;
}
