#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "zhe-util.h"
#include "zhe-platform.h"

int main(int argc, char *argv[])
{
    struct zhe_platform *platform;
    zhe_pubidx_t p;
    if (argc > 2) {
        fprintf(stderr, "usage: %s [BROKER-IP:PORT]\n", argv[0]);
        return 1;
    }
    platform = zhe(argc == 2 ? 0 : 7447, argc == 2 ? argv[1] : NULL);
    p = zhe_publish(1, 0, 1);
    uint64_t delay = 1000000000;
    zhe_once(platform, delay);
    uint64_t count = 0;
    while (true) {
        printf(">> Writing count %"PRIu64"\n", count);
        zhe_write(p, &count, sizeof(count), zhe_platform_time());
        count += 1;
        zhe_once(platform, delay);
    }
}
