#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "zhe.h"
#include "mindeps-platform.h"

/* NB this is pushing it: a hardcoded unique peer id ... */
static const uint8_t uniqueid[] = { 1 };

void background(struct zhe_platform * const platform)
{
    char inbuf[TRANSPORT_MTU];
    zhe_address_t insrc;
    int recvret;
    zhe_time_t tnow = zhe_platform_time();
    zhe_housekeeping(tnow);
    /* Note: calling "wait" just to lower CPU load a bit */
    zhe_platform_wait(platform);
    tnow = zhe_platform_time();
    if ((recvret = zhe_platform_recv(platform, inbuf, sizeof(inbuf), &insrc)) > 0) {
        zhe_input(inbuf, (size_t)recvret, &insrc, tnow);
    }
}

void data_handler(zhe_rid_t rid, const void *payload, zhe_paysize_t size, void *vpub)
{
    uint64_t count;
    if (size >= sizeof(count)) {
        memcpy(&count, payload, sizeof(count));
        printf(">> Received: %"PRIu64"\n", count);
    } else {
        /* NB can only happen when someone published something weird for this resource id */
        printf(">> Received short message\n");
    }
}

int main()
{
    struct zhe_platform * const platform = zhe_platform_new(PORT);
    struct zhe_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.id = uniqueid;
    cfg.idlen = sizeof(uniqueid);
    cfg.scoutaddr = (zhe_address_t *)&scoutaddr;
    if (!zhe_platform_join(platform, &scoutaddr)) {
        fprintf(stderr, "join scoutaddr failed\n");
        return 1;
    }
    if (zhe_init(&cfg, platform, zhe_platform_time()) < 0) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    zhe_start(zhe_platform_time());
    zhe_subscribe(1, 0, 0, data_handler, NULL);
    while (true) {
        zhe_platform_background(platform);
    }
}
