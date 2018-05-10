#include <stdio.h>
#include "zhe-util.h"
#include "zhe-platform.h"

void data_handler(zhe_rid_t rid, const void *payload, zhe_paysize_t size, void *vpub)
{
    printf(">> Processing data for resource %ju\n", (uintmax_t)rid);
    char *data = (char*)payload;
    printf(">> Received: \t");
    for (int i = 0; i < size; ++i) {
        printf("%c", data[i]);
    }
    printf("\n");
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
    zhe_flush(zhe_platform_time());
    while (true) {
        zhe_dispatch(platform);
    }
}
