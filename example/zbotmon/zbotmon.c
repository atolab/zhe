#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <termios.h>
#include <time.h>
#include <sys/poll.h>
#include "zhe.h"
#include "platform-serial.h"

#define RID_DISTANCE (1)
#define RID_MOTOR    (2)

struct motorstate {
    int16_t speedL;
    int16_t speedR;
};

static const uint8_t peerid[] = { 'z', 'm', 'o', 'n' };
static uint8_t inbuf[TRANSPORT_MTU];
static uint8_t inp;
static int devfd;
static struct zhe_address dummyaddr;
static struct timespec toffset;

static zhe_time_t millis(void)
{
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (zhe_time_t)((t.tv_sec - toffset.tv_sec) * (1000000000 / ZHE_TIMEBASE) + t.tv_nsec / ZHE_TIMEBASE);
}

void zhe_platform_trace(struct zhe_platform *pf, const char *fmt, ...)
{
    uint32_t t = (uint32_t)millis();
    va_list ap;
    va_start(ap, fmt);
    flockfile(stdout);
    printf("%4"PRIu32".%03"PRIu32" ", (uint32_t)(t / 1000), (uint32_t)(t % 1000));
    (void)vprintf(fmt, ap);
    printf("\n");
    funlockfile(stdout);
    va_end(ap);
}

size_t zhe_platform_addr2string(const struct zhe_platform *pf, char *str, size_t size, const zhe_address_t *addr)
{
    str[0] = 0;
    return 0;
}

int zhe_platform_string2addr(const struct zhe_platform *pf, struct zhe_address *addr, const char *str)
{
    memset(addr, 0, sizeof(*addr));
    return 1;
}

int zhe_platform_send(struct zhe_platform *pf, const void *buf, size_t size, const zhe_address_t *dst)
{
    int cnt;
    size_t pos = 0;
    while (pos < size && (cnt = write(devfd, (const char*)buf + pos, size - pos)) != size - pos) {
        if (cnt == 0) {
            fprintf(stderr, "write: EOF\n");
            exit(1);
        } else if (cnt == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("write");
            exit(1);
        }
        pos += (size_t)cnt;
        struct pollfd pfd;
        pfd.fd = devfd;
        pfd.events = POLLOUT;
        (void)poll(&pfd, 1, 1);
    }
    return (int)size;
}

int zhe_platform_addr_eq(const struct zhe_address *a, const struct zhe_address *b)
{
    return 1;
}

void zhe_platform_close_session(struct zhe_platform *pf, const struct zhe_address *addr)
{
}

void zhe_platform_housekeeping(struct zhe_platform *pf, zhe_time_t millis)
{
}

bool zhe_platform_needs_keepalive(struct zhe_platform *pf)
{
    return false;
}

static void setup(const char *devname)
{
    extern unsigned zhe_trace_cats;
    struct zhe_config config;
    zhe_trace_cats = ~0;
    (void)clock_gettime(CLOCK_MONOTONIC, &toffset);
    toffset.tv_sec -= toffset.tv_sec % 10000;
    memset(&config, 0, sizeof(config));
    config.id = peerid;
    config.idlen = sizeof(peerid);
    config.scoutaddr = &dummyaddr;
    if ((devfd = open(devname, O_RDWR | O_NOCTTY | O_NONBLOCK)) < 0) {
        perror("open device");
        exit(1);
    }
    struct termios ttyconf;
    if (tcgetattr(devfd, &ttyconf) < 0) {
        perror("tcgetattr");
        exit(1);
    }
    cfmakeraw(&ttyconf);
    if (cfsetspeed(&ttyconf, 115200) < 0) {
        perror("set baud");
        exit(1);
    }
    ttyconf.c_cflag &= ~PARENB;
    ttyconf.c_cflag &= ~CSTOPB;
    ttyconf.c_cflag &= ~CSIZE;
    ttyconf.c_cflag |= CS8;
    // no flow control
    ttyconf.c_cflag &= ~CRTSCTS;

    ttyconf.c_cflag |= CREAD | CLOCAL;  // turn on READ & ignore ctrl lines
    ttyconf.c_iflag &= ~(IXON | IXOFF | IXANY); // turn off s/w flow ctrl

    ttyconf.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // make raw
    ttyconf.c_oflag &= ~OPOST; // make raw

    // see: http://unixwiz.net/techtips/termios-vmin-vtime.html
    ttyconf.c_cc[VMIN]  = 0;
    ttyconf.c_cc[VTIME] = 20;

    if (tcsetattr(devfd, TCSANOW, &ttyconf) < 0) {
        perror("tcsetattr");
        exit(1);
    }
    zhe_init(&config, NULL, millis());
}

static void handle_input(zhe_time_t millis)
{
    int cnt;
    if (inp == sizeof(inbuf)) {
        /* this might happen if the input is corrupt or contains oversized messages,
           but not if the sender plays by the rules */
        fprintf(stderr, "handle_input: input buffer full\n");
        exit(1);
    } else if ((cnt = read(devfd, &inbuf[inp], sizeof(inbuf) - inp)) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("handle_input: read");
            exit(1);
        }
    } else if (cnt == 0) {
        fprintf(stderr, "handle_input: EOF\n");
        exit(1);
    } else {
        inp += (uint8_t)cnt;
        const int cons = zhe_input(inbuf, inp, &dummyaddr, millis);
        if (cons > 0) {
            if (cons < inp) {
                memmove(inbuf, inbuf + cons, inp - cons);
            }
            inp -= cons;
        }
    }
}

static void handle_dist(zhe_rid_t rid, const void *payload, zhe_paysize_t sz, void *arg)
{
    uint8_t v = *((const uint8_t *)payload);
    printf("%d\n", v);
}

static uint16_t swap2u(uint16_t x)
{
    return (x >> 8) | (x << 8);
}

static int16_t swap2(int16_t x)
{
    return (int16_t)swap2u((uint16_t)x);
}

static bool write_motorstate(zhe_pubidx_t pubh, int16_t speedL, int16_t speedR)
{
    static const union { uint16_t s; uint8_t b[2]; } u = { .s = 0x1234 };
    struct motorstate m = { speedL, speedR };
    if (u.b[0] == 0x12) {
        m.speedL = swap2(m.speedL);
        m.speedR = swap2(m.speedR);
    }
    return zhe_write(pubh, &m, sizeof(m), millis());
}

int main(int argc, char* argv[])
{
    zhe_subidx_t dist_sub;
    zhe_pubidx_t mstate_pub;
    struct pollfd pfd[2];
    if (argc != 2) {
        fprintf(stderr, "usage: %s SERIAL\n", argv[0]);
        return 1;
    }
    setup(argv[1]);
    dist_sub = zhe_subscribe(RID_DISTANCE, 0, 0, handle_dist, NULL);
    mstate_pub = zhe_publish(RID_MOTOR, 0, 1);
    pfd[0].fd = devfd;
    pfd[0].events = POLLIN;
    pfd[1].fd = 0;
    pfd[1].events = POLLIN;
    zhe_start(millis());
    while (true) {
        zhe_housekeeping(millis());
        if (poll(pfd, 2, 10) < 0) {
            perror("poll");
            break;
        }
        if (pfd[0].revents & POLLIN) {
            handle_input(millis());
        }
        if (pfd[1].revents & POLLIN) {
            int l, r;
            scanf("%d %d", &l, &r);
            if (write_motorstate(mstate_pub, (int16_t)l, (int16_t)r)) {
                printf("wrote %d %d\n", l, r);
            } else {
                printf("failed to write %d %d (transmit window full)\n", l, r);
            }
        }
    }
    return 0;
}
