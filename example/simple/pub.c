#include <stdio.h>
#include <string.h>
#include "zhe-util.h"
#include "zhe-platform.h"

int main(int argc, char* argv[])
{
    struct zhe_platform * const platform = zhe(7447, NULL);
    zhe_pubidx_t p;
    p = zhe_publish(1, 0, 1);
    uint64_t delay = 1000000000;
    zhe_once(platform, delay);
    uint64_t count = 0;
    while (true) {
        printf(">> Writing count %llu\n", count);
        zhe_write(p, &count, sizeof(count), zhe_platform_time());
        count += 1;
        zhe_once(platform, delay);
    }
}
