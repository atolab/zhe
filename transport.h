#ifndef ZENO_TRANSPORT_H
#define ZENO_TRANSPORT_H

#include <stddef.h>
#include <sys/types.h>

/* modes for transport should not include 0 */
#define TRANSPORT_PACKET 1
#define TRANSPORT_STREAM 2

#define SENDRECV_HANGUP (-1)
#define SENDRECV_ERROR  (-2)

struct zeno_transport;
struct zeno_config;
struct zeno_address;

typedef struct zeno_transport_ops {
    struct zeno_transport * (*new)(const struct zeno_config *config, const struct zeno_address *scoutaddr);
    void (*free)(struct zeno_transport * restrict tp);
    int (*addr_eq)(const struct zeno_address *a, const struct zeno_address *b);
    size_t (*addr2string)(char * restrict str, size_t size, const struct zeno_address * restrict addr);
    int (*octseq2addr)(struct zeno_address * restrict addr, size_t sz, const void * restrict octseq);
    ssize_t (*send)(struct zeno_transport * restrict tp, const void * restrict buf, size_t size, const struct zeno_address * restrict dst);
    ssize_t (*recv)(struct zeno_transport * restrict tp, void * restrict buf, size_t size, struct zeno_address * restrict src);
    int (*wait)(const struct zeno_transport * restrict tp, ztimediff_t timeout);
    int (*join)(const struct zeno_transport * restrict tp, const struct zeno_address * restrict addr);
} zeno_transport_ops_t;

#endif
