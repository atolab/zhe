#ifndef ARDUINO

#include <stdio.h>

#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#include "zeno-tracing.h"
#include "transport-udp.h"

#define MAX_SELF 16

struct udp {
    int s[2];
    int next;
    /* FIXME: there are better ways to do this: */
    uint16_t ucport;
    size_t nself;
    in_addr_t self[MAX_SELF];
};

static struct udp gudp;

static size_t udp_addr2string(char * restrict str, size_t size, const zeno_address_t * restrict addr);

static void set_nonblock(int sock)
{
    int flags = fcntl(sock, F_GETFL, 0);
    flags |= O_NONBLOCK;
    (void)fcntl(sock, F_SETFL, flags);
}

static struct zeno_transport *udp_new(const struct zeno_config *config, zeno_address_t *scoutaddr)
{
    static const char *mcaddr_str = "239.255.0.2";
    static const unsigned short mcport = 10350;
    const int one = 1;
    struct udp * const udp = &gudp;
    struct sockaddr_in addr;
    socklen_t addrlen;
    struct ip_mreq mreq;
    struct ifaddrs *ifa;

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
                struct zeno_address za = { *a };
                char str[TRANSPORT_ADDRSTRLEN];
                udp_addr2string(str, sizeof(str), &za);
                if (a->sin_addr.s_addr == htonl(INADDR_ANY) || a->sin_addr.s_addr == htonl(INADDR_NONE)) {
                    ZT(TRANSPORT, ("%s: %s (not interesting)", c->ifa_name, str));
                } else if (udp->nself < MAX_SELF) {
                    ZT(TRANSPORT, ("%s: %s", c->ifa_name, str));
                    udp->self[udp->nself++] = a->sin_addr.s_addr;
                } else {
                    ZT(TRANSPORT, ("%s: %s (no space left)", c->ifa_name, str));
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
        goto err;
    }
    addrlen = sizeof(addr);
    (void)getsockname(udp->s[0], (struct sockaddr *)&addr, &addrlen);
    udp->ucport = addr.sin_port;

    /* MC sockets needs reuse options set, and is bound to the MC address we use at a "well-known" 
       port number */
    if (setsockopt(udp->s[1], SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one)) == -1) {
        goto err;
    }
#ifdef SO_REUSEPORT
    if (setsockopt(udp->s[1], SOL_SOCKET, SO_REUSEPORT, (char *)&one, sizeof(one)) == -1) {
        goto err;
    }
#endif
    addr.sin_port = htons(mcport);
    (void)inet_pton(AF_INET, mcaddr_str, &addr.sin_addr.s_addr);
    if (bind(udp->s[1], (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        goto err;
    }
    mreq.imr_multiaddr = addr.sin_addr;
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(udp->s[1], IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mreq, sizeof(mreq)) == -1) {
        goto err;
    }

    /* Scout address is simply the multicast address */
    scoutaddr->a = addr;
    return (struct zeno_transport *) udp;

err:
    for (size_t i = 0; i < sizeof(udp->s) / sizeof(udp->s[0]); i++) {
        close(udp->s[i]);
    }
    return NULL;
}

static void udp_free(struct zeno_transport * restrict tp)
{
    struct udp *udp = (struct udp *)tp;
    close(udp->s[0]);
    close(udp->s[1]);
}

static char *udp_uint16_to_string(char * restrict str, uint16_t val)
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

static size_t udp_addr2string1(char * restrict str, const zeno_address_t * restrict addr)
{
    char *p;
    (void)inet_ntop(AF_INET, &addr->a.sin_addr.s_addr, str, INET_ADDRSTRLEN);
    p = str + strlen(str);
    *p++ = ':';
    p = udp_uint16_to_string(p, ntohs(addr->a.sin_port));
    return (size_t)(p - str);
}

static size_t udp_addr2string(char * restrict str, size_t size, const zeno_address_t * restrict addr)
{
    char tmp[TRANSPORT_ADDRSTRLEN];
    assert(size > 0);
    if (size >= sizeof(tmp)) {
        return udp_addr2string1(str, addr);
    } else {
        size_t n = udp_addr2string1(tmp, addr);
        if (n >= size) {
            n = size - 1;
        }
        memcpy(str, tmp, n);
        str[n] = 0;
        return n;
    }
}

static ssize_t udp_send(struct zeno_transport * restrict tp, const void * restrict buf, size_t size, const zeno_address_t * restrict dst)
{
    struct udp *udp = (struct udp *)tp;
    ssize_t ret;
    assert(size <= TRANSPORT_MTU);
    ret = sendto(udp->s[0], buf, size, 0, (const struct sockaddr *)&dst->a, sizeof(dst->a));
    if (ret > 0) {
        { char tmp[TRANSPORT_ADDRSTRLEN]; udp_addr2string(tmp, sizeof(tmp), dst); ZT(TRANSPORT, ("send %zu to %s", size, tmp)); }
        return ret;
    } else if (ret == -1 && errno == EAGAIN) {
        { char tmp[TRANSPORT_ADDRSTRLEN]; udp_addr2string(tmp, sizeof(tmp), dst); ZT(TRANSPORT, ("send(EAGAIN) %zu to %s", size, tmp)); }
        return 0;
    } else {
        return SENDRECV_ERROR;
    }
}

static ssize_t udp_recv1(struct udp * restrict udp, void * restrict buf, size_t size, zeno_address_t * restrict src)
{
    socklen_t srclen = sizeof(src->a);
    ssize_t ret;
    ret = recvfrom(udp->s[udp->next], buf, size, 0, (struct sockaddr *)&src->a, &srclen);
    if (ret > 0) {
        { char tmp[TRANSPORT_ADDRSTRLEN]; udp_addr2string(tmp, sizeof(tmp), src); ZT(TRANSPORT, ("recv[%d] %zu from %s", udp->next, ret, tmp)); }
        udp->next = 1 - udp->next;
        return ret;
    } else if (ret == -1 && errno == EAGAIN) {
        ret = recvfrom(udp->s[1 - udp->next], buf, size, 0, (struct sockaddr *)&src->a, &srclen);
        if (ret > 0) {
            { char tmp[TRANSPORT_ADDRSTRLEN]; udp_addr2string(tmp, sizeof(tmp), src); ZT(TRANSPORT, ("recv[%d] %zu from %s", 1 - udp->next, ret, tmp)); }
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

static int is_from_me(const struct udp * restrict udp, const zeno_address_t * restrict src)
{
    for (size_t i = 0; i < udp->nself; i++) {
        if (src->a.sin_addr.s_addr == udp->self[i] && src->a.sin_port == udp->ucport) {
            return 1;
        }
    }
    return 0;
}

static ssize_t udp_recv(struct zeno_transport * restrict tp, void * restrict buf, size_t size, zeno_address_t * restrict src)
{
    struct udp *udp = (struct udp *)tp;
    ssize_t ret = udp_recv1(udp, buf, size, src);
    if (ret <= 0 || !is_from_me(udp, src)) {
        return ret;
    } else {
        return 0;
    }
}

static int udp_addr_eq(const struct zeno_address *a, const struct zeno_address *b)
{
    return a->a.sin_addr.s_addr == b->a.sin_addr.s_addr && a->a.sin_port == b->a.sin_port;
}

zeno_transport_ops_t transport_udp = {
    .new = udp_new,
    .free = udp_free,
    .addr2string = udp_addr2string,
    .addr_eq = udp_addr_eq,
    .send = udp_send,
    .recv = udp_recv
};

#endif
