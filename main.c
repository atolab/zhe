#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <time.h>

#include "transport-udp.h"
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
            zeno_write(*pub, sizeof(pong), &pong, tnow);

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
#ifndef ARDUINO
    uint16_t port = 7007;
#endif
    int drop_pct = 0;
    const char *scoutaddrstr = "239.255.0.1";
    char *mcgroups_join_str = "239.255.0.2,239.255.0.3";
    char *mconduit_dstaddrs_str = "239.255.0.2,239.255.0.3";
    int (*wait)(const struct zeno_transport * restrict tp, ztimediff_t timeout);
    ssize_t (*recv)(struct zeno_transport * restrict tp, void * restrict buf, size_t size, zeno_address_t * restrict src);

    srandomdev();
    zeno_time_init();
    ownidsize = getrandomid(ownid, sizeof(ownid));
    zeno_trace_cats = ~0u;

    scoutaddrstr = "239.255.0.1";
    while((opt = getopt(argc, argv, "C:c:h:psquS:G:M:X:")) != EOF) {
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
    struct zeno_transport * const transport = udp_new(port, drop_pct);

    struct zeno_address scoutaddr;
    cfg.scoutaddr = &scoutaddr;
    if (!udp_string2addr(transport, cfg.scoutaddr, scoutaddrstr)) {
        fprintf(stderr, "%s: invalid address\n", scoutaddrstr); exit(2);
    } else {
        udp_join(transport, cfg.scoutaddr);
    }

    struct zeno_address mcgroups_join[MAX_MULTICAST_GROUPS];
    cfg.n_mcgroups_join = 0;
    cfg.mcgroups_join = mcgroups_join;
    mcgroups_join_str = strdup(mcgroups_join_str);
    for (char *addrstr = strtok(mcgroups_join_str, ","); addrstr != NULL; addrstr = strtok(NULL, ",")) {
        if (cfg.n_mcgroups_join == MAX_MULTICAST_GROUPS) {
            fprintf(stderr, "too many multicast groups specified\n"); exit(2);
        } else if (!udp_string2addr(transport, &cfg.mcgroups_join[cfg.n_mcgroups_join], addrstr)) {
            fprintf(stderr, "%s: invalid address\n", addrstr); exit(2);
        } else if (!udp_join(transport, &cfg.mcgroups_join[cfg.n_mcgroups_join])) {
            fprintf(stderr, "%s: join failed\n", addrstr); exit(2);
        } else {
            cfg.n_mcgroups_join++;
        }
    }
    free(mcgroups_join_str);

    struct zeno_address mconduit_dstaddrs[N_OUT_MCONDUITS];
    cfg.n_mconduit_dstaddrs = 0;
    cfg.mconduit_dstaddrs = mconduit_dstaddrs;
    mconduit_dstaddrs_str = strdup(mconduit_dstaddrs_str);
    for (char *addrstr = strtok(mconduit_dstaddrs_str, ","); addrstr != NULL; addrstr = strtok(NULL, ",")) {
        if (cfg.n_mconduit_dstaddrs == N_OUT_MCONDUITS) {
            fprintf(stderr, "too many mconduit dstaddrs specified\n"); exit(2);
        } else if (!udp_string2addr(transport, &cfg.mconduit_dstaddrs[cfg.n_mconduit_dstaddrs], addrstr)) {
            fprintf(stderr, "%s: invalid address\n", addrstr); exit(2);
        } else {
            cfg.n_mconduit_dstaddrs++;
        }
    }
    if (cfg.n_mconduit_dstaddrs != N_OUT_MCONDUITS) {
        fprintf(stderr, "too few mconduit dstaddrs specified\n"); exit(2);
    }
    free(mconduit_dstaddrs_str);

    wait = udp_wait;
    recv = udp_recv;
#else
    struct zeno_transport * const transport = arduino_new();
    struct zeno_address scoutaddr;
    memset(&scoutaddr, 0, sizeof(scoutaddr));
    cfg.scoutaddr = &scoutaddr;
    wait = arduino_wait;
    recv = arduino_recv;
#endif

    if (zeno_init(&cfg, transport, zeno_time()) < 0) {
        fprintf(stderr, "init failed\n");
        exit(1);
    }
    zeno_start(zeno_time());

#if TRANSPORT_MODE == TRANSPORT_STREAM
#warning "input code here presupposes packets"
#endif

    switch (mode) {
        case 0: case -1: {
            ztime_t tstart = zeno_time();
            pubidx_t p;
            if (mode != 0) {
                p = publish(2, cid, 1);
                (void)subscribe(1, 100 /* don't actually need this much ... */, cid, shandler, &p);
            }
            while ((mode != 0) || ZTIME_TO_SECu32(zeno_time() - tstart) < 20) {
                ztime_t tnow;
                if (wait(transport, 10)) {
                    char inbuf[TRANSPORT_MTU];
                    zeno_address_t insrc;
                    ssize_t recvret;
                    tnow = zeno_time();
                    if ((recvret = recv(transport, inbuf, sizeof(inbuf), &insrc)) > 0) {
                        zeno_input(inbuf, (size_t)recvret, &insrc, tnow);
                    }
                } else {
                    tnow = zeno_time();
                }
                zeno_housekeeping(tnow);
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

                zeno_housekeeping(zeno_time());

                {
                    char inbuf[TRANSPORT_MTU];
                    zeno_address_t insrc;
                    ssize_t recvret;
                    while ((recvret = recv(transport, inbuf, sizeof(inbuf), &insrc)) > 0) {
                        zeno_input(inbuf, (size_t)recvret, &insrc, zeno_time());
                    }
                }

                /* Loop means we don't call zeno_housekeeping for each sample, which dramatically reduces the
                   number of (non-blocking) recvfrom calls and speeds things up a fair bit */
                for (int i = 0; i < blocksize; i++) {
                    ztime_t tnow = zeno_time();
                    if (zeno_write(p, sizeof(k), &k, tnow)) {
                        if ((k % checkintv) == 0) {
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
                        wait(transport, 10);
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
