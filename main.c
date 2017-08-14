//
//  main.c
//  zenop
//
//  Created by Erik Boasson on 20-06-2017.
//  Copyright © 2017 ADLINK ATO. All rights reserved.
//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include "zeno.h"
#include "zeno-tracing.h"
#include "zeno-time.h"

static size_t getrandomid(unsigned char *ownid, size_t ownidsize)
{
    FILE *fp;
    if ((fp = fopen("/dev/urandom", "rb")) == NULL) {
        perror("can't open /dev/urandom\n");
        exit(1);
    }
    if (fread(ownid, ownidsize, 1, fp) != 1) {
        fprintf(stderr, "can't read %zu random bytes from /dev/urandom\n", ownidsize);
        fclose(fp);
        exit(1);
    }
    fclose(fp);
    return ownidsize;
}

static size_t getidfromarg(unsigned char *ownid, size_t ownidsize, const char *in)
{
    size_t i = 0;
    int pos = 0, dpos;
    while(i < ownidsize && in[pos] && sscanf(in + pos, "%hhx%n", &ownid[i], &dpos) == 1) {
        pos += dpos;
        if (in[pos] == ':') {
            pos++;
        } else if (in[pos] != 0) {
            fprintf(stderr, "junk in explicit peer id\n");
            exit(1);
        }
        i++;
    }
    if (in[pos]) {
        fprintf(stderr, "junk at end of explicit peer id\n");
        exit(1);
    }
    return i;
}

int main(int argc, char * const *argv)
{
    unsigned char ownid[16];
    size_t ownidsize;
    int opt;

    zeno_trace_cats = ~0u;
    zeno_time_init();
    ownidsize = getrandomid(ownid, sizeof(ownid));

    while((opt = getopt(argc, argv, "h:")) != EOF) {
        switch(opt) {
            case 'h': ownidsize = getidfromarg(ownid, sizeof(ownid), optarg); break;
            default: fprintf(stderr, "invalid options given\n"); exit(1); break;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "extraneous parameters given\n");
        exit(1);
    }

    (void)zeno_init(ownidsize, ownid);
    zeno_loop_init();
    do {
        const struct timespec sl = { 0, 10000000 };
        zeno_loop();
        nanosleep(&sl, NULL);
    } while(zeno_time() < 20000);
    return 0;
}
