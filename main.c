#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>

#include "zeno.h"
#include "zeno-tracing.h"
#include "zeno-time.h"

static zpsize_t getrandomid(unsigned char *ownid, size_t ownidsize)
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
    return (zpsize_t)ownidsize;
}

static zpsize_t getidfromarg(unsigned char *ownid, size_t ownidsize, const char *in)
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
    return (zpsize_t)i;
}

static void shandler(rid_t rid, zpsize_t size, const void *payload, void *arg)
{
    static ztime_t tprint;
    uint32_t k;
    assert(size == 4);
    k = *(uint32_t *)payload;
    if ((k % 16384) == 0) {
        ztime_t tnow = zeno_time();
        if ((ztimediff_t)(tnow - tprint) >= 1000) {
            printf ("%4u.%03u ** %u\n", tnow / 1000, tnow % 1000, k);
            tprint = tnow;
        }
    }
}

int main(int argc, char * const *argv)
{
    unsigned char ownid[16];
    zpsize_t ownidsize;
    int opt;
    int mode = 0;

    zeno_time_init();
    ownidsize = getrandomid(ownid, sizeof(ownid));
    zeno_trace_cats = ~0u;

    while((opt = getopt(argc, argv, "h:psq")) != EOF) {
        switch(opt) {
            case 'h': ownidsize = getidfromarg(ownid, sizeof(ownid), optarg); break;
            case 'p': mode = 1; break;
            case 's': mode = -1; break;
            case 'q': zeno_trace_cats = 0; break;
            default: fprintf(stderr, "invalid options given\n"); exit(1); break;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "extraneous parameters given\n");
        exit(1);
    }

    (void)zeno_init(ownidsize, ownid);
    zeno_loop_init();
    switch (mode) {
        case 0:
            do {
                const struct timespec sl = { 0, 10000000 };
                zeno_loop();
                nanosleep(&sl, NULL);
            } while(zeno_time() < 20000);
            break;
        case -1: {
            (void)subscribe(1, 0, shandler, 0);
            while (1) {
                zeno_loop();
                zeno_wait_input(10);
            }
            break;
        }
        case 1: {
            uint32_t k = 0;
            pubidx_t p = publish(1, 1);
            ztime_t tprint = zeno_time();
            while (1) {
                int i;
                zeno_loop();
                /* Loop means we don't call zeno_loop for each sample, which dramatically reduces the
                   number of (non-blocking) recvfrom calls and speeds things up a fair bit */
                for (i = 0; i < 50; i++) {
                    if (zeno_write(p, sizeof(k), &k)) {
                        if ((k % 16384) == 0) {
                            ztime_t tnow = zeno_time();
                            if ((ztimediff_t)(tnow - tprint) >= 1000) {
                                printf ("%4u.%03u ** %u\n", tnow / 1000, tnow % 1000, k);
                                tprint = tnow;
                            }
                        }
                        k++;
                    } else {
                        break;
                    }
                }
                if (i < 16) {
                    /* zeno_write failed => no space in transmit window => must first process incoming
                       packets or expire a lease to make further progress */
                    zeno_wait_input(10);
                }
            }
            break;
        }
        default:
            fprintf(stderr, "mode = %d?", mode);
            exit(1);
    }
    return 0;
}
