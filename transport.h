#ifndef ZHE_TRANSPORT_H
#define ZHE_TRANSPORT_H

#include <stddef.h>
#include <sys/types.h>

#include "zeno-config-int.h"

/* modes for transport should not include 0 */
#define TRANSPORT_PACKET 1
#define TRANSPORT_STREAM 2

#define SENDRECV_HANGUP (-1)
#define SENDRECV_ERROR  (-2)

struct zhe_address;
struct zhe_transport_ops;

struct zhe_transport {
    const struct zhe_transport_ops *ops;
};

typedef struct zhe_transport_ops {
    /* Return 0 if a and b are different addresses, 1 if they are the same */
    int (*addr_eq)(const struct zhe_address *a, const struct zhe_address *b);

    /* Convert the address in ADDR to a standard string representation, and write the result into str.
       No more than size bytes shall be written to str, and str shall be null-terminated. Size must
       consequently be > 0. Number of characters written into str shall be returned. Failure is not an
       option. */
    size_t (*addr2string)(const struct zhe_transport *tp, char * restrict str, size_t size, const struct zhe_address * restrict addr);

    /* Sends the packet in buf of size bytes to address dst. Returns the number of bytes written (which
       must be sz for now), or 0 to indicate nothing was written, or SENDRECV_HANGUP to indicate the
       other side hung up on us (i.e., when using TCP), or SENDRECV_ERROR for unspecified, and therefore
       fatal errors. Should be non-blocking. */
    ssize_t (*send)(struct zhe_transport * restrict tp, const void * restrict buf, size_t size, const struct zhe_address * restrict dst);
} zhe_transport_ops_t;

#endif
