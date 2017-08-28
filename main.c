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

struct data {
    uint32_t key;
    uint32_t seq;
};

static uint32_t checkintv = 16384;

static zhe_paysize_t getrandomid(unsigned char *ownid, size_t ownidsize)
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
    return (zhe_paysize_t)ownidsize;
}

static zhe_paysize_t getidfromarg(unsigned char *ownid, size_t ownidsize, const char *in)
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
    return (zhe_paysize_t)i;
}

extern unsigned zhe_delivered, zhe_discarded;

struct pong { uint32_t k; zhe_time_t t; };

#define MAX_KEY 9u

static void shandler(zhe_rid_t rid, zhe_paysize_t size, const void *payload, void *arg)
{
    static zhe_time_t tprint;
    static uint32_t lastseq[MAX_KEY+1];
    static uint32_t oooc;
    static uint32_t lastseq_init;
    const struct data * const d = payload;
    assert(size == sizeof(*d));
    if (lastseq_init & (1u << d->key)) {
        if (d->seq != lastseq[d->key]+1) {
            oooc++;
        }
        lastseq[d->key] = d->seq;
    } else {
        lastseq[d->key] = d->seq;
        lastseq_init |= 1u << d->key;
    }
    if ((d->seq % checkintv) == 0) {
        zhe_time_t tnow = zhe_platform_time();
        if (ZTIME_TO_SECu32(tnow - tprint) >= 1) {
            zhe_pubidx_t *pub = arg;
            struct pong pong = { .k = d->seq, .t = tnow };
            zhe_write(*pub, sizeof(pong), &pong, tnow);
            for (uint32_t k = 0; k <= MAX_KEY; k++) {
                if (lastseq_init & (1u << k)) {
                    printf ("%4"PRIu32".%03"PRIu32" [%u] %u %u [%u,%u]\n", ZTIME_TO_SECu32(tnow), ZTIME_TO_MSECu32(tnow), k, lastseq[k], oooc, zhe_delivered, zhe_discarded);
                }
            }
            tprint = tnow;
        }
    }
}

static void rhandler(zhe_rid_t rid, zhe_paysize_t size, const void *payload, void *arg)
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
#ifndef ARDUINO
    uint16_t port = 7007;
#endif
    int drop_pct = 0;
    const char *scoutaddrstr = "239.255.0.1";
    char *mcgroups_join_str = "239.255.0.2,239.255.0.3";
    char *mconduit_dstaddrs_str = "239.255.0.2,239.255.0.3";

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
    while((opt = getopt(argc, argv, "C:k:c:h:psquS:G:M:X:")) != EOF) {
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
            case 'X': drop_pct = atoi(optarg); break;
            case 'G': mcgroups_join_str = optarg; break;
            case 'M': mconduit_dstaddrs_str = optarg; break;
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

#ifndef ARDUINO

    struct zhe_platform * const platform = zhe_platform_new(port, drop_pct);

    struct zhe_address scoutaddr;
    cfg.scoutaddr = &scoutaddr;
    if (!zhe_platform_string2addr(platform, cfg.scoutaddr, scoutaddrstr)) {
        fprintf(stderr, "%s: invalid address\n", scoutaddrstr); exit(2);
    } else {
        zhe_platform_join(platform, cfg.scoutaddr);
    }

    struct zhe_address mcgroups_join[MAX_MULTICAST_GROUPS];
    cfg.n_mcgroups_join = 0;
    cfg.mcgroups_join = mcgroups_join;
    mcgroups_join_str = strdup(mcgroups_join_str);
    for (char *addrstr = strtok(mcgroups_join_str, ","); addrstr != NULL; addrstr = strtok(NULL, ",")) {
        if (cfg.n_mcgroups_join == MAX_MULTICAST_GROUPS) {
            fprintf(stderr, "too many multicast groups specified\n"); exit(2);
        } else if (!zhe_platform_string2addr(platform, &cfg.mcgroups_join[cfg.n_mcgroups_join], addrstr)) {
            fprintf(stderr, "%s: invalid address\n", addrstr); exit(2);
        } else if (!zhe_platform_join(platform, &cfg.mcgroups_join[cfg.n_mcgroups_join])) {
            fprintf(stderr, "%s: join failed\n", addrstr); exit(2);
        } else {
            cfg.n_mcgroups_join++;
        }
    }
    free(mcgroups_join_str);

    struct zhe_address mconduit_dstaddrs[N_OUT_MCONDUITS];
    cfg.n_mconduit_dstaddrs = 0;
    cfg.mconduit_dstaddrs = mconduit_dstaddrs;
    mconduit_dstaddrs_str = strdup(mconduit_dstaddrs_str);
    for (char *addrstr = strtok(mconduit_dstaddrs_str, ","); addrstr != NULL; addrstr = strtok(NULL, ",")) {
        if (cfg.n_mconduit_dstaddrs == N_OUT_MCONDUITS) {
            fprintf(stderr, "too many mconduit dstaddrs specified\n"); exit(2);
        } else if (!zhe_platform_string2addr(platform, &cfg.mconduit_dstaddrs[cfg.n_mconduit_dstaddrs], addrstr)) {
            fprintf(stderr, "%s: invalid address\n", addrstr); exit(2);
        } else {
            cfg.n_mconduit_dstaddrs++;
        }
    }
    if (cfg.n_mconduit_dstaddrs != N_OUT_MCONDUITS) {
        fprintf(stderr, "too few mconduit dstaddrs specified\n"); exit(2);
    }
    free(mconduit_dstaddrs_str);
#else
    struct zhe_platform * const platform = zhe_arduino_new();
    struct zhe_address scoutaddr;
    memset(&scoutaddr, 0, sizeof(scoutaddr));
    cfg.scoutaddr = &scoutaddr;
#endif

    if (zhe_init(&cfg, platform, zhe_platform_time()) < 0) {
        fprintf(stderr, "init failed\n");
        exit(1);
    }
    zhe_start(zhe_platform_time());

#if TRANSPORT_MODE == TRANSPORT_STREAM
#warning "input code here presupposes packets"
#endif

    switch (mode) {
        case 0: case -1: {
            zhe_time_t tstart = zhe_platform_time();
            zhe_pubidx_t p;
            if (mode != 0) {
                p = zhe_publish(2, cid, 1);
                (void)zhe_subscribe(1, 100 /* don't actually need this much ... */, cid, shandler, &p);
            }
            while ((mode != 0) || ZTIME_TO_SECu32(zhe_platform_time() - tstart) < 20) {
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
            (void)zhe_subscribe(1, 0, 0, shandler, &p2);
            (void)zhe_subscribe(2, 0, 0, rhandler, 0);
            zhe_time_t tprint = zhe_platform_time();
            while (1) {
                const int blocksize = 50;
                zhe_time_t tnow = zhe_platform_time();

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
                    if (zhe_write(p, sizeof(d), &d, tnow)) {
                        if ((d.seq % checkintv) == 0) {
                            if (ZTIME_TO_SECu32(tnow - tprint) >= 1) {
                                extern unsigned zhe_synch_sent;
                                printf ("%4"PRIu32".%03"PRIu32" %u [%u]\n", ZTIME_TO_SECu32(tnow), ZTIME_TO_MSECu32(tnow), d.seq, zhe_synch_sent);
                                tprint = tnow;
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
    return 0;
}
