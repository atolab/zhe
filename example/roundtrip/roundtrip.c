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

#include "zhe-util.h"

struct data {
    uint64_t ts;
};

#define MAX_LAT 10000
static uint64_t lat[MAX_LAT];
static int latp = 0;

static int uint64_cmp(const void *va, const void *vb)
{
    const uint64_t *a = va;
    const uint64_t *b = vb;
    return (*a == *b) ? 0 : (*a < *b) ? -1 : 1;
}

static void latupd(uint64_t l)
{
    lat[latp++] = l;
    if (latp == 10000) {
        static int first = 1;
        uint64_t sum;
        qsort(lat, (size_t)latp, sizeof(lat[0]), uint64_cmp);
        sum = lat[0];
        for (int i = 1; i < latp; i++) {
            sum += lat[i];
        }
        if (first) {
            first = 0;
            printf("%10s %10s %10s %10s %10s\n", "min", "90%", "99%", "max", "avg");
        }
        printf("%#10g %#10g %#10g %#10g %#10g\n", lat[0] / 1000.0, lat[latp - (latp+9)/10] / 1000.0, lat[latp - (latp+99)/100] / 1000.0, lat[latp-1] / 1000.0, (double)sum / (latp * 1000.0));
        latp = 0;
    }
}

static uint64_t gethrtime(void)
{
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + (unsigned)t.tv_nsec;
}

static void pong_handler(zhe_rid_t rid, const void *payload, zhe_paysize_t size, void *vpub)
{
    const zhe_pubidx_t *pub = vpub;
    zhe_write(*pub, payload, size, zhe_platform_time());
    zhe_flush(); /* just in case we use latency budget */
}

static void ping_handler(zhe_rid_t rid, const void *payload, zhe_paysize_t size, void *vpub)
{
    uint64_t hrtnow = gethrtime();
    const zhe_pubidx_t *pub = vpub;
    const struct data *pong = payload;
    latupd(hrtnow - pong->ts);
    struct data ping = { hrtnow };
    zhe_write(*pub, &ping, sizeof(ping), zhe_platform_time());
    zhe_flush(); /* just in case we use latency budget */
}

static void loop(struct zhe_platform *platform)
{
    zhe_time_t tnow = zhe_platform_time(), tend = tnow + (1000000000 / ZHE_TIMEBASE);
    while ((zhe_timediff_t)(tnow - tend) < 0) {
        zhe_housekeeping(tnow);
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
    }
}

int main(int argc, char * const *argv)
{
    unsigned char ownid[16];
    zhe_paysize_t ownidsize;
    int opt;
    int mode = 0;
    unsigned cid = 0;
    struct zhe_config cfg;
    uint16_t port = 7447;
    const char *scoutaddrstr = "239.255.0.1";
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
    zhe_trace_cats = ZTCAT_PEERDISC | ZTCAT_PUBSUB;
#endif

    scoutaddrstr = "239.255.0.1";
    while((opt = getopt(argc, argv, "h:c:S:G:M:")) != EOF) {
        switch(opt) {
            case 'h': ownidsize = getidfromarg(ownid, sizeof(ownid), optarg); break;
            case 'c': {
                unsigned long t = strtoul(optarg, NULL, 0);
                if (t >= N_OUT_CONDUITS) { fprintf(stderr, "cid %lu out of range\n", t); exit(1); }
                cid = (unsigned)t;
                break;
            }
            case 'S': scoutaddrstr = optarg; break;
            case 'G': mcgroups_join_str = optarg; break;
            case 'M': mconduit_dstaddrs_str = optarg; break;
            default: fprintf(stderr, "invalid options given\n"); exit(1); break;
        }
    }
    if (optind + 1 < argc) {
        fprintf(stderr, "extraneous parameters given\n");
        exit(1);
    } else if (argc < optind + 1) {
        fprintf(stderr, "missing ping|pong argument\n");
        exit(1);
    } else if (strcmp(argv[optind], "ping") == 0) {
        mode = 1;
    } else if (strcmp(argv[optind], "pong") == 0) {
        mode = 0;
    } else {
        fprintf(stderr, "argument is neither ping nor pong\n");
        exit(2);
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.id = ownid;
    cfg.idlen = ownidsize;

    struct zhe_platform * const platform = zhe_platform_new(port, 0);
    cfg_handle_addrs(&cfg, platform, scoutaddrstr, mcgroups_join_str, mconduit_dstaddrs_str);
    if (zhe_init(&cfg, platform, zhe_platform_time()) < 0) {
        fprintf(stderr, "init failed\n");
        exit(1);
    }
    zhe_start(zhe_platform_time());

    zhe_pubidx_t p;
    if (mode == 0) {/* pong */
        p = zhe_publish(2, cid, 1);
        (void)zhe_subscribe(1, 100, cid, pong_handler, &p);
    } else { /* ping */
        p = zhe_publish(1, cid, 1);
        (void)zhe_subscribe(2, 100, cid, ping_handler, &p);
        /* while (!decls_done()) ... */
        loop(platform);
        /* first write can't really fail with a reasonably sized buffer */
        struct data d = { gethrtime() };
        (void)zhe_write(p, &d, sizeof(d), zhe_platform_time());
        zhe_flush();
    }
    printf("starting loop\n");
    while(1) {
        loop(platform);
    }
    return 0;
}
