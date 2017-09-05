#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#include "platform-udp.h"
#include "zhe-assert.h"
#include "zhe-tracing.h"
#include "zhe-config-deriv.h"
#include "zhe.h"

#define BLOCKING_SEND 0
#define SIMUL_PACKET_LOSS 1

#define MAX_SELF 16

struct udp {
    int s[2];
    int next;
    uint16_t port;
    uint16_t ucport;
    size_t nself;
    in_addr_t self[MAX_SELF];
#if SIMUL_PACKET_LOSS
    long randomthreshold;
#endif
};

static struct udp gudp;
static struct timespec toffset;

zhe_time_t zhe_platform_time(void)
{
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (zhe_time_t)((t.tv_sec - toffset.tv_sec) * (1000000000 / ZHE_TIMEBASE) + t.tv_nsec / ZHE_TIMEBASE);
}

void zhe_platform_trace(struct zhe_platform *pf, const char *fmt, ...)
{
    uint32_t t = (uint32_t)zhe_platform_time();
    va_list ap;
    va_start(ap, fmt);
    flockfile(stdout);
    printf("%4"PRIu32".%03"PRIu32" ", ZTIME_TO_SECu32(t), ZTIME_TO_MSECu32(t));
    (void)vprintf(fmt, ap);
    printf("\n");
    funlockfile(stdout);
    va_end(ap);
}

static void set_nonblock(int sock)
{
    int flags = fcntl(sock, F_GETFL, 0);
    flags |= O_NONBLOCK;
    (void)fcntl(sock, F_SETFL, flags);
}

struct zhe_platform *zhe_platform_new(uint16_t port, int drop_pct)
{
    const int one = 1;
    struct udp * const udp = &gudp;
    struct sockaddr_in addr;
    socklen_t addrlen;
    struct ifaddrs *ifa;

    (void)clock_gettime(CLOCK_MONOTONIC, &toffset);
    toffset.tv_sec -= toffset.tv_sec % 10000;

#if SIMUL_PACKET_LOSS
    udp->randomthreshold = drop_pct * 21474836;
#endif

    udp->port = htons(port);

    /* Get own IP addresses so we know what to filter out -- disabling MC loopback would help if
       we knew there was only a single proces on a node, but I actually want to run multiple for
       testing. This does the trick as long as the addresses don't change. There are (probably)
       various better ways to deal with the original problem as well. */
    udp->nself = 0;
    if (getifaddrs(&ifa) == -1) {
        perror("getifaddrs");
        return NULL;
    } else {
        for (const struct ifaddrs *c = ifa; c; c = c->ifa_next) {
            if (c->ifa_addr && c->ifa_addr->sa_family == AF_INET) {
                const struct sockaddr_in *a = (const struct sockaddr_in *)c->ifa_addr;
                struct zhe_address za = { *a };
                char str[TRANSPORT_ADDRSTRLEN];
                zhe_platform_addr2string(NULL, str, sizeof(str), &za);
                if (a->sin_addr.s_addr == htonl(INADDR_ANY) || a->sin_addr.s_addr == htonl(INADDR_NONE)) {
                    ZT(TRANSPORT, "%s: %s (not interesting)", c->ifa_name, str);
                } else if (udp->nself < MAX_SELF) {
                    ZT(TRANSPORT, "%s: %s", c->ifa_name, str);
                    udp->self[udp->nself++] = a->sin_addr.s_addr;
                } else {
                    ZT(TRANSPORT, "%s: %s (no space left)", c->ifa_name, str);
                }
            }
        }
        freeifaddrs(ifa);
    }
    if (udp->nself == 0) {
        return NULL;
    }

    udp->next = 0;
    for (size_t i = 0; i < sizeof(udp->s) / sizeof(udp->s[0]); i++) {
        if ((udp->s[i] = socket(PF_INET, SOCK_DGRAM, 0)) == -1) {
            while (i--) {
                close(udp->s[i]);
            }
            return NULL;
        }
        set_nonblock(udp->s[i]);
    }

    /* UC socket gets bound to random port number, INADDR_ANY -- the recipients will find the
       the source address in the incoming packets & use that to reply */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(udp->s[0], (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind[0]");
        goto err;
    }
    addrlen = sizeof(addr);
    (void)getsockname(udp->s[0], (struct sockaddr *)&addr, &addrlen);
    udp->ucport = addr.sin_port;

    /* MC sockets needs reuse options set, and is bound to the MC address we use at a "well-known" 
       port number */
    if (setsockopt(udp->s[1], SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one)) == -1) {
        perror("SO_REUSEADDR");
        goto err;
    }
#ifdef SO_REUSEPORT
    if (setsockopt(udp->s[1], SOL_SOCKET, SO_REUSEPORT, (char *)&one, sizeof(one)) == -1) {
        perror("SO_REUSEPORT");
        goto err;
    }
#endif
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = udp->port;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(udp->s[1], (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind[1]");
        goto err;
    }
    return (struct zhe_platform *)udp;

err:
    for (size_t i = 0; i < sizeof(udp->s) / sizeof(udp->s[0]); i++) {
        close(udp->s[i]);
    }
    return NULL;
}

static char *uint16_to_string(char * restrict str, uint16_t val)
{
    if (val == 0) {
        *str++ = '0';
        *str = 0;
        return str;
    } else {
        char *end;
        if (val >= 10000)     { str += 5; }
        else if (val >= 1000) { str += 4; }
        else if (val >= 100)  { str += 3; }
        else if (val >= 10)   { str += 2; }
        else                  { str += 1; }
        end = str;
        *str-- = 0;
        while (val != 0) {
            *str-- = '0' + (val % 10);
            val /= 10;
        }
        return end;
    }
}

static size_t addr2string1(char * restrict str, const zhe_address_t * restrict addr)
{
    char *p;
    memcpy(str, "udp/", 4);
    (void)inet_ntop(AF_INET, &addr->a.sin_addr.s_addr, str + 4, INET_ADDRSTRLEN);
    p = str + strlen(str);
    *p++ = ':';
    p = uint16_to_string(p, ntohs(addr->a.sin_port));
    return (size_t)(p - str);
}

size_t zhe_platform_addr2string(const struct zhe_platform *pf, char * restrict str, size_t size, const zhe_address_t * restrict addr)
{
    zhe_assert(size > 0);
    if (size >= TRANSPORT_ADDRSTRLEN) {
        return addr2string1(str, addr);
    } else {
        char tmp[TRANSPORT_ADDRSTRLEN];
        size_t n = addr2string1(tmp, addr);
        if (n >= size) {
            n = size - 1;
        }
        memcpy(str, tmp, n);
        str[n] = 0;
        return n;
    }
}

int zhe_platform_string2addr(const struct zhe_platform *pf, struct zhe_address * restrict addr, const char * restrict str)
{
    struct udp *udp = (struct udp *)pf;
    char *portstr, *portend;
    unsigned long port;
    memset(addr, 0, sizeof(*addr));
    if (strncmp(str, "udp/", 4) == 0) {
        str += 4;
    }
    if ((portstr = strchr(str, ':')) != NULL) {
        *portstr++ = 0;
    }
    addr->a.sin_family = AF_INET;
    if (inet_pton(AF_INET, str, &addr->a.sin_addr.s_addr) != 1) {
        return 0;
    }
    if (portstr != NULL) {
        port = strtoul(portstr, &portend, 10);
        if (*portstr == 0 || *portend != 0 || port > 65535) {
            return 0;
        }
        addr->a.sin_port = htons((uint16_t)port);
    } else {
        addr->a.sin_port = udp->port;
    }
    return 1;
}

int zhe_platform_join(const struct zhe_platform *pf, const struct zhe_address *addr)
{
    struct udp *udp = (struct udp *)pf;
    struct ip_mreq mreq;
    mreq.imr_multiaddr = addr->a.sin_addr;
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(udp->s[1], IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mreq, sizeof(mreq)) == -1) {
        perror("IP_ADD_MEMBERSHIP");
        return 0;
    }
    return 1;
}

#if BLOCKING_SEND
static void wait_send(int sock)
{
    fd_set ws;
    struct timeval tv;
    FD_ZERO(&ws);
    FD_SET(sock, &ws);
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    (void)select(sock+1, NULL, &ws, NULL, &tv);
}
#endif

int zhe_platform_send(struct zhe_platform *pf, const void * restrict buf, size_t size, const zhe_address_t * restrict dst)
{
    struct udp *udp = (struct udp *)pf;
    ssize_t ret;
    zhe_assert(size <= TRANSPORT_MTU);
#if SIMUL_PACKET_LOSS
    if (udp->randomthreshold && random() < udp->randomthreshold) {
        return (int)size;
    }
#endif
#if BLOCKING_SEND
    wait_send(udp->s[0]);
#endif
    ret = sendto(udp->s[0], buf, size, 0, (const struct sockaddr *)&dst->a, sizeof(dst->a));
    if (ret > 0) {
#if ENABLE_TRACING
        if (ZTT(TRANSPORT)) {
            char tmp[TRANSPORT_ADDRSTRLEN];
            zhe_platform_addr2string(pf, tmp, sizeof(tmp), dst);
            ZT(TRANSPORT, "send %zu to %s", ret, tmp);
        }
#endif
        return (int)ret;
    } else if (ret == -1 && (errno == EAGAIN || errno == ENOBUFS || errno == EHOSTDOWN || errno == EHOSTUNREACH)) {
        return 0;
    } else {
        return SENDRECV_ERROR;
    }
}

static ssize_t recv1(struct udp *udp, void * restrict buf, size_t size, zhe_address_t * restrict src)
{
    socklen_t srclen = sizeof(src->a);
    ssize_t ret;
    ret = recvfrom(udp->s[udp->next], buf, size, 0, (struct sockaddr *)&src->a, &srclen);
    if (ret > 0) {
        udp->next = 1 - udp->next;
        return ret;
    } else if (ret == -1 && errno == EAGAIN) {
        ret = recvfrom(udp->s[1 - udp->next], buf, size, 0, (struct sockaddr *)&src->a, &srclen);
        if (ret > 0) {
            return ret;
        } else if (ret == -1 && errno == EAGAIN) {
            return 0;
        } else {
            return SENDRECV_ERROR;
        }
    } else {
        return SENDRECV_ERROR;
    }
}

static int is_from_me(const struct udp *udp, const zhe_address_t * restrict src)
{
    for (size_t i = 0; i < udp->nself; i++) {
        if (src->a.sin_addr.s_addr == udp->self[i] && src->a.sin_port == udp->ucport) {
            return 1;
        }
    }
    return 0;
}

int zhe_platform_recv(struct zhe_platform *pf, void * restrict buf, size_t size, zhe_address_t * restrict src)
{
    struct udp *udp = (struct udp *)pf;
    ssize_t ret = recv1(udp, buf, size, src);
    if (ret <= 0 || !is_from_me(udp, src)) {
#if ENABLE_TRACING
        if (ZTT(TRANSPORT) && ret > 0) {
            char tmp[TRANSPORT_ADDRSTRLEN];
            zhe_platform_addr2string(pf, tmp, sizeof(tmp), src);
            ZT(TRANSPORT, "recv[%d] %zu from %s", 1 - udp->next, ret, tmp);
        }
#endif
        assert(ret < INT_MAX);
        return (int)ret;
    } else {
#if ENABLE_TRACING
        if (ZTT(TRANSPORT) && is_from_me(udp, src)) {
            char tmp[TRANSPORT_ADDRSTRLEN];
            zhe_platform_addr2string(pf, tmp, sizeof(tmp), src);
            ZT(TRANSPORT, "recv[%d] %zu from %s (self)", 1 - udp->next, ret, tmp);
        }
#endif
        return 0;
    }
}

int zhe_platform_addr_eq(const struct zhe_address *a, const struct zhe_address *b)
{
    return a->a.sin_addr.s_addr == b->a.sin_addr.s_addr && a->a.sin_port == b->a.sin_port;
}

int zhe_platform_wait(const struct zhe_platform *pf, zhe_timediff_t timeout)
{
    struct udp * const udp = (struct udp *)pf;
    const int k = (udp->s[0] > udp->s[1]) ? udp->s[0] : udp->s[1];
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(udp->s[0], &rs);
    FD_SET(udp->s[1], &rs);
    if (timeout < 0) {
        return select(k+1, &rs, NULL, NULL, NULL) > 0;
    } else {
        struct timeval tv;
        tv.tv_sec = ZTIME_TO_SECu32(timeout);
        tv.tv_usec = 1000 * ZTIME_TO_MSECu32(timeout);
        return select(k+1, &rs, NULL, NULL, &tv) > 0;
    }
}
