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


int main(int argc, char* argv[]) {
    uint16_t port = 7447;
    struct zhe_platform * const platform = zhe(port);
    zhe_pubidx_t p;
    p = zhe_publish(1, 0, 1);
    uint64_t  delay = 1000000000;
    zhe_once(platform, delay);

    uint64_t count = 0;
    char* data = "zenoh data";

    while (true) {
        printf(">> Writing count %llu\n", count);
        zhe_write(p, data, (zhe_paysize_t)strlen(data), zhe_platform_time());
        zhe_flush(zhe_platform_time());
        count += 1;
        zhe_once(platform, delay);
    }
}
