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

uint16_t port = 7447;
const char *scoutaddrstr = "239.255.0.1";

void init_rnd() {
#ifdef __APPLE__
    srandomdev();
#else
    srandom(time(NULL) + getpid());
#endif

}

void data_handler(zhe_rid_t rid, const void *payload, zhe_paysize_t size, void *vpub) {
    printf(">> Processing data for resource %ju\n", (uintmax_t)rid);
    uint64_t count = *(uint64_t*)payload;
    printf(">> Received count: %lld\n", count);
}

int main(int argc, char* argv[]) {
    unsigned char ownid[16];
    zhe_paysize_t ownidsize;
    struct zhe_config cfg;

    init_rnd();

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

#if ENABLE_TRACING
    zhe_trace_cats = ZTCAT_PEERDISC | ZTCAT_PUBSUB;
#endif

    ownidsize = getrandomid(ownid, sizeof(ownid));

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
    zhe_subidx_t s;
    uint64_t  period = 1000000;
    s = zhe_subscribe(1, 0, 0, data_handler, NULL);
    zhe_flush();
    zhe_loop(platform, period);

}
