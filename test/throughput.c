#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <time.h>

#include "platform-udp.h"
#include "zhe.h"
#include "zhe-tracing.h"
#include "zhe-assert.h"

#include "zhe-config-deriv.h" /* for N_OUT_CONDUITS, ZTIME_TO_SECu32 */

#include "testlib.h"

struct data {
    uint32_t key;
    uint32_t seq;
};

static uint32_t checkintv = 16384;

extern unsigned zhe_delivered, zhe_discarded;

struct pong { uint32_t k; zhe_time_t t; };

#define MAX_KEY 9u

static uint32_t firstseq[MAX_KEY+1];
static uint32_t lastseq[MAX_KEY+1];
static uint32_t oooc;
static uint32_t lastseq_init;

static int all_was_well(uint32_t nsecs)
{
    zhe_time_t tnow = zhe_platform_time();
    if (lastseq_init == 0) {
        printf ("%4"PRIu32".%03"PRIu32" *** no data\n", ZTIME_TO_SECu32(tnow), ZTIME_TO_MSECu32(tnow));
        return 0;
    } else if (oooc != 0) {
        printf ("%4"PRIu32".%03"PRIu32" *** out-of-order %"PRIu32"\n", ZTIME_TO_SECu32(tnow), ZTIME_TO_MSECu32(tnow), oooc);
        return 0;
    } else {
        int ret = 1;
        for (uint32_t k = 0; k <= MAX_KEY; k++) {
            if (lastseq_init & (1u << k)) {
                if ((lastseq[k] - firstseq[k]) / 10 <= nsecs) {
                    printf ("%4"PRIu32".%03"PRIu32" *** [%"PRIu32"] only %"PRIu32" in %"PRIu32"s\n", ZTIME_TO_SECu32(tnow), ZTIME_TO_MSECu32(tnow), k, lastseq[k] - firstseq[k], nsecs);
                    ret = 0;
                }
            }
        }
        return ret;
    }
}

static void shandler(zhe_rid_t rid, const void *payload, zhe_paysize_t size, void *arg)
{
    static zhe_time_t tprint;
    const struct data * const d = payload;
    assert(size == sizeof(*d));
    if (rid == 0) {
        zhe_time_t tnow = zhe_platform_time();
        printf ("%4"PRIu32".%03"PRIu32" got a WriteData %u %u\n", ZTIME_TO_SECu32(tnow), ZTIME_TO_MSECu32(tnow), d->key, d->seq);
        return;
    }
    if (lastseq_init & (1u << d->key)) {
        if (d->seq != lastseq[d->key]+1) {
            oooc++;
        }
        lastseq[d->key] = d->seq;
    } else {
        firstseq[d->key] = lastseq[d->key] = d->seq;
        lastseq_init |= 1u << d->key;
    }
    if ((d->seq % checkintv) == 0) {
        zhe_time_t tnow = zhe_platform_time();
        if (ZTIME_TO_SECu32(tnow - tprint) >= 1) {
            zhe_pubidx_t *pub = arg;
            struct pong pong = { .k = d->seq, .t = tnow };
            zhe_write(*pub, &pong, sizeof(pong), tnow);
            for (uint32_t k = 0; k <= MAX_KEY; k++) {
                if (lastseq_init & (1u << k)) {
                    printf ("%4"PRIu32".%03"PRIu32" [%u] %u %u [%u,%u]\n", ZTIME_TO_SECu32(tnow), ZTIME_TO_MSECu32(tnow), k, lastseq[k], oooc, zhe_delivered, zhe_discarded);
                }
            }
            tprint = tnow;
        }
    }
}

static void rhandler(zhe_rid_t rid, const void *payload, zhe_paysize_t size, void *arg)
{
    const struct pong *pong;
    assert(size == sizeof(*pong));
    pong = payload;
    zhe_time_t tnow = zhe_platform_time();
    printf ("%4"PRIu32".%03"PRIu32" pong %u %4"PRIu32".%03"PRIu32"\n", ZTIME_TO_SECu32(tnow), ZTIME_TO_MSECu32(tnow), pong->k, ZTIME_TO_SECu32(pong->t), ZTIME_TO_MSECu32(pong->t));
}

int main(int argc, char * const *argv)
{
    unsigned char ownid[16];
    zhe_paysize_t ownidsize;
    int opt;
    int mode = 0;
    unsigned cid = 0;
    int reliable = 1;
    uint32_t key = 0;
    struct zhe_config cfg;
    uint16_t port = 7447;
    int drop_pct = 0;
    int check_likely_success = 0;
    const char *scoutaddrstr = "239.255.0.1";
    bool sub_to_wildcard = false;
    zhe_time_t duration = (zhe_time_t)~0;
#if N_OUT_MCONDUITS == 0
    char *mcgroups_join_str = "";
    char *mconduit_dstaddrs_str = "";
#elif N_OUT_MCONDUITS == 1
    char *mcgroups_join_str = "239.255.0.2"; /* in addition to scout */
    char *mconduit_dstaddrs_str = "239.255.0.2";
#elif N_OUT_MCONDUITS == 2
    char *mcgroups_join_str = "239.255.0.2,239.255.0.3"; /* in addition to scout */
    char *mconduit_dstaddrs_str = "239.255.0.2,239.255.0.3";
#elif N_OUT_MCONDUITS == 3
    char *mcgroups_join_str = "239.255.0.2,239.255.0.3,239.255.0.4"; /* in addition to scout */
    char *mconduit_dstaddrs_str = "239.255.0.2,239.255.0.3";
#endif

#ifdef __APPLE__
    srandomdev();
#else
    srandom(time(NULL) + getpid());
#endif
    ownidsize = getrandomid(ownid, sizeof(ownid));
#if ENABLE_TRACING
    zhe_trace_cats = ~0u;
#endif

    scoutaddrstr = "239.255.0.1";
    while((opt = getopt(argc, argv, "D:C:k:c:h:psquS:G:M:X:xw")) != EOF) {
        switch(opt) {
            case 'h': ownidsize = getidfromarg(ownid, sizeof(ownid), optarg); break;
            case 'k':
                if ((key = (uint32_t)atoi(optarg)) > MAX_KEY) {
                    fprintf(stderr, "key %"PRIu32" out of range\n", key); exit(1);
                }
                break;
            case 'p': mode = 1; break;
            case 's': mode = -1; break;
            case 'q':
#if ENABLE_TRACING
                zhe_trace_cats = ZTCAT_PEERDISC | ZTCAT_PUBSUB;
#endif
                break;
            case 'c': {
                unsigned long t = strtoul(optarg, NULL, 0);
                if (t >= N_OUT_CONDUITS) { fprintf(stderr, "cid %lu out of range\n", t); exit(1); }
                cid = (unsigned)t;
                break;
            }
            case 'u': reliable = 0; break;
            case 'C': checkintv = (unsigned)atoi(optarg); break;
            case 'S': scoutaddrstr = optarg; break;
            case 'x': check_likely_success = 1; break;
            case 'X': drop_pct = atoi(optarg); break;
            case 'G': mcgroups_join_str = optarg; break;
            case 'M': mconduit_dstaddrs_str = optarg; break;
            case 'D': duration = (zhe_time_t)atoi(optarg); break;
            case 'w': sub_to_wildcard = true; break;
            default: fprintf(stderr, "invalid options given\n"); exit(1); break;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "extraneous parameters given\n");
        exit(1);
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.id = ownid;
    cfg.idlen = ownidsize;

    struct zhe_platform * const platform = zhe_platform_new(port, drop_pct);
    cfg_handle_addrs(&cfg, platform, scoutaddrstr, mcgroups_join_str, mconduit_dstaddrs_str);
    if (zhe_init(&cfg, platform, zhe_platform_time()) < 0) {
        fprintf(stderr, "init failed\n");
        exit(1);
    }
    zhe_time_t tstart = zhe_platform_time();
    zhe_start(tstart);

    zhe_declare_resource(1, "/t/data");
    zhe_declare_resource(2, "/t/pong");
    if (mode == 1) {
        zhe_declare_resource(3, "/t/test");
    }
    zhe_declare_resource(4, "**/data");
    switch (mode) {
        case 0: case -1: {
            zhe_pubidx_t p;
            if (mode != 0) {
                p = zhe_publish(2, cid, 1);
                (void)zhe_subscribe(1, 100 /* don't actually need this much ... */, cid, shandler, &p);
            }
            while (ZTIME_TO_SECu32(zhe_platform_time() - tstart) <= duration) {
                zhe_time_t tnow;
                if (zhe_platform_wait(platform, 10)) {
                    char inbuf[TRANSPORT_MTU];
                    zhe_address_t insrc;
                    int recvret;
                    tnow = zhe_platform_time();
                    if ((recvret = zhe_platform_recv(platform, inbuf, sizeof(inbuf), &insrc)) > 0) {
                        zhe_input(inbuf, (size_t)recvret, &insrc, tnow);
                    }
                } else {
                    tnow = zhe_platform_time();
                }
                zhe_housekeeping(tnow);
            }
            break;
        }
        case 1: {
            struct data d = { .key = key, .seq = 0 };
            zhe_pubidx_t p = zhe_publish(1, cid, reliable);
            zhe_pubidx_t p2 = zhe_publish(2, cid, 1);
            (void)zhe_subscribe(sub_to_wildcard ? 4 : 1, 0, 0, shandler, &p2);
            (void)zhe_subscribe(2, 0, 0, rhandler, 0);
            zhe_time_t tprint = tstart;
            for (zhe_time_t tnow = tstart; ZTIME_TO_SECu32(tnow - tstart) <= duration; tnow = zhe_platform_time()) {
                const int blocksize = 50;
                zhe_housekeeping(tnow);

                {
                    char inbuf[TRANSPORT_MTU];
                    zhe_address_t insrc;
                    int recvret;
                    while ((recvret = zhe_platform_recv(platform, inbuf, sizeof(inbuf), &insrc)) > 0) {
                        zhe_input(inbuf, (size_t)recvret, &insrc, tnow);
                    }
                }

                /* Loop means we don't call zhe_housekeeping for each sample, which dramatically reduces the
                   number of (non-blocking) recvfrom calls and speeds things up a fair bit */
                for (int i = 0; i < blocksize; i++) {
                    if (zhe_write(p, &d, sizeof(d), tnow)) {
                        if ((d.seq % checkintv) == 0) {
                            if (ZTIME_TO_SECu32(tnow - tprint) >= 1) {
                                extern unsigned zhe_synch_sent;
                                printf ("%4"PRIu32".%03"PRIu32" %u [%u]\n", ZTIME_TO_SECu32(tnow), ZTIME_TO_MSECu32(tnow), d.seq, zhe_synch_sent);
                                tprint = tnow;

                                struct data d1 = { .key = key, .seq = UINT32_MAX };
                                //zhe_write_uri("/t/data", &d1, sizeof(d1), tnow);
                                zhe_write_uri("**?ta", &d1, sizeof(d1), tnow);
                            }
                        }
                        d.seq++;
                    } else {
                        /* zhe_write failed => no space in transmit window => must first process incoming
                         packets or expire a lease to make further progress */
                        zhe_platform_wait(platform, 10);
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
    if (!check_likely_success) {
        return 0;
    } else {
        return !all_was_well(ZTIME_TO_SECu32(zhe_platform_time() - tstart));
    }
}
