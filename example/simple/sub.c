#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "zhe-util.h"
#include "zhe-platform.h"

void data_handler(zhe_rid_t rid, const void *payload, zhe_paysize_t size, void *vpub)
{
    if (size != 8) {
        printf("received unexpected payload (rid %ju, size %u)\n", (uintmax_t)rid, (unsigned)size);
    } else {
        /* copy to avoid issues on platforms that require aligned access */
        uint64_t count;
        memcpy(&count, payload, sizeof(count));
        printf("%"PRIu64"\n", count);
    }
}

int main(int argc, char* argv[])
{
#ifdef TCPTLS
    struct zhe_platform * const platform = zhe(0, "127.0.0.1:7447");
#else
    struct zhe_platform * const platform = zhe(7447, NULL);
#endif
    zhe_subidx_t s;
    s = zhe_subscribe(1, 0, 0, data_handler, NULL);
    while (true) {
        zhe_dispatch(platform);
    }
}
