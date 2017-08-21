#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <time.h>

#include "zeno.h"
#include "zeno-tracing.h"
#include "zeno-time.h"

#include "zeno-config-deriv.h" /* for N_OUT_CONDUITS, ZTIME_TO_SECu32 */

static uint32_t checkintv = 16384;

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

extern unsigned zeno_delivered, zeno_discarded;

struct pong { uint32_t k; ztime_t t; };

static void shandler(rid_t rid, zpsize_t size, const void *payload, void *arg)
{
    static ztime_t tprint;
    static uint32_t lastk;
    static uint32_t oooc;
    static int lastk_init;
    uint32_t k;
    assert(size == 4);
    k = *(uint32_t *)payload;
    if (lastk_init) {
        if (k != lastk+1) {
            oooc++;
        }
        lastk = k;
    } else {
        lastk = k;
        lastk_init = 1;
    }
    if ((k % checkintv) == 0) {
        ztime_t tnow = zeno_time();
        if (ZTIME_TO_SECu32(tnow - tprint) >= 1) {
            pubidx_t *pub = arg;
            struct pong pong = { .k = k, .t = tnow };
            zeno_write(*pub, sizeof(pong), &pong);

            printf ("%4"PRIu32".%03"PRIu32" %u %u [%u,%u]\n", ZTIME_TO_SECu32(tnow), ZTIME_TO_MSECu32(tnow), k, oooc, zeno_delivered, zeno_discarded);
            tprint = tnow;
        }
    }
}

static void rhandler(rid_t rid, zpsize_t size, const void *payload, void *arg)
{
    const struct pong *pong;
    assert(size == sizeof(*pong));
    pong = payload;
    ztime_t tnow = zeno_time();
    printf ("%4"PRIu32".%03"PRIu32" pong %u %4"PRIu32".%03"PRIu32"\n", ZTIME_TO_SECu32(tnow), ZTIME_TO_MSECu32(tnow), pong->k, ZTIME_TO_SECu32(pong->t), ZTIME_TO_MSECu32(pong->t));
}

int main(int argc, char * const *argv)
{
    unsigned char ownid[16];
    zpsize_t ownidsize;
    int opt;
    int mode = 0;
    unsigned cid = 0;
    int reliable = 1;
    struct zeno_config cfg;
    
    zeno_time_init();
    ownidsize = getrandomid(ownid, sizeof(ownid));
    zeno_trace_cats = ~0u;

    cfg.scoutaddr = "239.255.0.1:7007";
    const char *mcgroups_join[10] = { "239.255.0.2:7007", "239.255.0.3:7007" };
    cfg.n_mcgroups_join = 2;
    cfg.mcgroups_join = mcgroups_join;
    const char *mconduit_dstaddrs[10] = { "239.255.0.2:7007", "239.255.0.3:7007" };
    cfg.n_mconduit_dstaddrs = 2;
    cfg.mconduit_dstaddrs = mconduit_dstaddrs;
    cfg.transport_options = NULL;

    while((opt = getopt(argc, argv, "C:c:h:psquX:S:G:M:")) != EOF) {
        switch(opt) {
            case 'h': ownidsize = getidfromarg(ownid, sizeof(ownid), optarg); break;
            case 'p': mode = 1; break;
            case 's': mode = -1; break;
            case 'q': zeno_trace_cats = ZTCAT_PEERDISC | ZTCAT_PUBSUB; break;
            case 'c': {
                unsigned long t = strtoul(optarg, NULL, 0);
                if (t >= N_OUT_CONDUITS) { fprintf(stderr, "cid %lu out of range\n", t); exit(1); }
                cid = (unsigned)t;
                break;
            }
            case 'u': reliable = 0; break;
            case 'C': checkintv = (unsigned)atoi(optarg); break;
            case 'S': cfg.scoutaddr = optarg; break;
            case 'G': case 'M': {
                const char **dst = (opt == 'G') ? mcgroups_join : mconduit_dstaddrs;
                size_t *n = (opt == 'G') ? &cfg.n_mcgroups_join : &cfg.n_mconduit_dstaddrs;
                char *addr;
                *n = 0;
                for (addr = strtok(optarg, ","); addr != NULL; addr = strtok(NULL, ",")) {
                    dst[(*n)++] = addr;
                }
                break;
            }
            case 'X': cfg.transport_options = optarg; break;
            default: fprintf(stderr, "invalid options given\n"); exit(1); break;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "extraneous parameters given\n");
        exit(1);
    }

    cfg.id = ownid;
    cfg.idlen = ownidsize;

    if (zeno_init(&cfg) < 0) {
        fprintf(stderr, "init failed\n");
        exit(1);
    }
    zeno_loop_init();
    switch (mode) {
        case 0: {
            ztime_t tstart = zeno_time();
            do {
                const struct timespec sl = { 0, 10000000 };
                zeno_loop();
                nanosleep(&sl, NULL);
            } while(ZTIME_TO_SECu32(zeno_time() - tstart) < 20);
            break;
        }
        case -1: {
            pubidx_t p = publish(2, cid, 1);
            (void)subscribe(1, 100 /* don't actually need this much ... */, cid, shandler, &p);
            while (1) {
                zeno_loop();
                zeno_wait_input(10);
            }
            break;
        }
        case 1: {
            uint32_t k = 0;
            pubidx_t p = publish(1, cid, reliable);
            (void)subscribe(2, 0, 0, rhandler, 0);
            ztime_t tprint = zeno_time();
            while (1) {
                const int blocksize = 50;
                int i;
                zeno_loop();
                /* Loop means we don't call zeno_loop for each sample, which dramatically reduces the
                   number of (non-blocking) recvfrom calls and speeds things up a fair bit */
                for (i = 0; i < blocksize; i++) {
                    if (zeno_write(p, sizeof(k), &k)) {
                        if ((k % checkintv) == 0) {
                            ztime_t tnow = zeno_time();
                            if (ZTIME_TO_SECu32(tnow - tprint) >= 1) {
                                extern unsigned zeno_synch_sent;
                                printf ("%4"PRIu32".%03"PRIu32" %u [%u]\n", ZTIME_TO_SECu32(tnow), ZTIME_TO_MSECu32(tnow), k, zeno_synch_sent);
                                tprint = tnow;
                            }
                        }
                        k++;
                    } else {
                        /* zeno_write failed => no space in transmit window => must first process incoming
                         packets or expire a lease to make further progress */
                        zeno_wait_input(10);
                        break;
                    }
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
