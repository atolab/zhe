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


void data_handler(zhe_rid_t rid, const void *payload, zhe_paysize_t size, void *vpub) {
    printf(">> Processing data for resource %ju\n", (uintmax_t)rid);
    char *data = (char*)payload;
    //count = *(uint64_t*)payload;
    printf(">> Received: \t");
    for (int i = 0; i < size; ++i) {
        printf("%c", data[i]);
    }
    printf("\n");
}

int main(int argc, char* argv[]) {
    uint16_t port = 7447;
    struct zhe_platform * const platform = zhe(port);
    zhe_subidx_t s;
    s = zhe_subscribe(1, 0, 0, data_handler, NULL);
    zhe_flush(zhe_platform_time());
    while (true) {
        zhe_dispatch(platform);
    }
}
