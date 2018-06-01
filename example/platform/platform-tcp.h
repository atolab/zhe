#ifndef TRANSPORT_TCP_H
#define TRANSPORT_TCP_H

#include <netinet/in.h>
#include "zhe-platform.h"

#ifndef MAX_PEERS
#  error "platform include file should come after core configuration settings have been defined"
#endif

#define TRANSPORT_MTU        100u
#define TRANSPORT_MODE       TRANSPORT_STREAM
#define TRANSPORT_ADDRSTRLEN (4 + INET_ADDRSTRLEN + 6) /* tcp/IP:PORT -- tcp/ is 4, colon is 1, PORT in [1,5] */

/* FIXME: think through whether there really need to be more conns than peers - until then, this code should help us avoid confusing the two; derived configuration definitions are not yet available, so no MAX_PEERS_1, no peeridx_t, &c. */
#define MAX_CONNECTIONS (11 * (MAX_PEERS == 0 ? 1 : MAX_PEERS) / 10)
#if MAX_CONNECTIONS < UINT8_MAX
typedef uint8_t connidx_t;
typedef uint16_t connid_t;
#elif MAX_CONNECTIONS < UINT16_MAX
typedef uint16_t connidx_t;
typedef uint32_t connid_t;
#else
#error "MAX_CONNECTIONS is too large for 16-bit connection idx"
#endif

enum zhe_address_kind {
    ZHE_AK_CONN,
    ZHE_AK_IP
};

typedef struct zhe_address {
    enum zhe_address_kind kind;
    union {
        connid_t id;
        struct {
            connidx_t idx;
            connidx_t serial;
        } s;
        struct sockaddr_in a;
    } u;
} zhe_address_t;

typedef struct zhe_recvbuf {
    void *buf;
} zhe_recvbuf_t;

zhe_time_t zhe_platform_time(void);
struct zhe_platform *zhe_platform_new(uint16_t port, const char *pingaddrs);
int zhe_platform_string2addr(const struct zhe_platform *pf, struct zhe_address * restrict addr, const char * restrict str);
int zhe_platform_join(const struct zhe_platform *pf, const struct zhe_address *addr);
int zhe_platform_wait(const struct zhe_platform *pf, zhe_timediff_t timeout);
int zhe_platform_recv(struct zhe_platform *pf, zhe_recvbuf_t *rbuf, struct zhe_address * restrict src);
int zhe_platform_advance(struct zhe_platform *pf, const struct zhe_address * restrict src, int cnt);

typedef struct zhe_platform_waitinfo {
    bool shouldwait;
    int maxfd;
    fd_set rs, ws;
} zhe_platform_waitinfo_t;

void zhe_platform_wait_prep(zhe_platform_waitinfo_t *wi, const struct zhe_platform *pf);
int zhe_platform_wait_block(zhe_platform_waitinfo_t *wi, zhe_timediff_t timeout);

#endif
