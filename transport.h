#ifndef ZENO_TRANSPORT_H
#define ZENO_TRANSPORT_H

#include <stddef.h>
#include <sys/types.h>

#include "zeno-config-int.h"

/* modes for transport should not include 0 */
#define TRANSPORT_PACKET 1
#define TRANSPORT_STREAM 2

#define SENDRECV_HANGUP (-1)
#define SENDRECV_ERROR  (-2)

struct zeno_address;
struct zeno_transport_ops;

struct zeno_transport {
    const struct zeno_transport_ops *ops;
};

typedef struct zeno_transport_ops {
    /* Return 0 if a and b are different addresses, 1 if they are the same */
    int (*addr_eq)(const struct zeno_address *a, const struct zeno_address *b);

    /* Convert the address in ADDR to a standard string representation, and write the result into str.
       No more than size bytes shall be written to str, and str shall be null-terminated. Size must
       consequently be > 0. Number of characters written into str shall be returned. Failure is not an
       option. */
    size_t (*addr2string)(const struct zeno_transport *tp, char * restrict str, size_t size, const struct zeno_address * restrict addr);

    /* Sends the packet in buf of size bytes to address dst. Returns the number of bytes written (which
       must be sz for now), or 0 to indicate nothing was written, or SENDRECV_HANGUP to indicate the
       other side hung up on us (i.e., when using TCP), or SENDRECV_ERROR for unspecified, and therefore
       fatal errors. Should be non-blocking. */
    ssize_t (*send)(struct zeno_transport * restrict tp, const void * restrict buf, size_t size, const struct zeno_address * restrict dst);
} zeno_transport_ops_t;

#endif
