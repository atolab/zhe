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
    /* Construct a new transport instance (may be a global one, but the caller must not assume so),
       given configuration and scout address. Return NULL on error, a non-NULL pointer acceptable to
       the other functions on success */
    struct zeno_transport * (*new)(const struct zeno_config *config, const struct zeno_address *scoutaddr);

    /* Release resources claimed by new() */
    void (*free)(struct zeno_transport * restrict tp);

    /* Return 0 if a and b are different addresses, 1 if they are the same */
    int (*addr_eq)(const struct zeno_address *a, const struct zeno_address *b);

    /* Convert the address in ADDR to a standard string representation, and write the result into str.
       No more than size bytes shall be written to str, and str shall be null-terminated. Size must
       consequently be > 0. Number of characters written into str shall be returned. Failure is not an
       option. */
    size_t (*addr2string)(char * restrict str, size_t size, const struct zeno_address * restrict addr);

    /* Convert the address in the string to an internal address representation. The string consists of
       the first sz bytes of octseq and need not be null-terminated. Any string yielded by addr2string
       must be accepted, others may be accepted. Returns 0 on error, 1 on successful conversion */
    int (*octseq2addr)(struct zeno_address * restrict addr, size_t sz, const void * restrict octseq);

    /* Sends the packet in buf of size bytes to address dst. Returns the number of bytes written (which
       must be sz for now), or 0 to indicate nothing was written, or SENDRECV_HANGUP to indicate the
       other side hung up on us (i.e., when using TCP), or SENDRECV_ERROR for unspecified, and therefore
       fatal errors. Should be non-blocking, or else not wait "long". */
    ssize_t (*send)(struct zeno_transport * restrict tp, const void * restrict buf, size_t size, const struct zeno_address * restrict dst);

    /* Attempts to read one packet (TRANSPORT_PACKET) / a number of bytes (TRANSPORT_STREAM) from the
       underlying transport and puts the result in buf. No more than size bytes may be written to buf
       (so a packet should be truncated), and the actual number of bytes read is returned. The source
       of the data shall be stored in src when data has been read and a positive number is returned,
       otherwise the contents of src shall be undefined after the call. 0 may be returned to indicate
       no data is available at this time, SENDRECV_HANGUP to indicate the other side hung up on us
       (again, e.g., TCP), and SENDRECV_ERROR for some unspecified (and assumed fatal) error. Should
       be non-blocking, or else not wait "long". */
    ssize_t (*recv)(struct zeno_transport * restrict tp, void * restrict buf, size_t size, struct zeno_address * restrict src);

    /* Wait for data to become available on the transport (which should result in a subsequent call to
       recv succeeding), but wait no longer than timeout. The return value shall be 1 in the case data
       is available (or is highly likely to be available), the return value shall be 0 in all other
       cases, including errors. Timeout is in the same units as other times and intervals, 0 is a pure
       poll, and a negative value for timeout means wait forever. */
    int (*wait)(const struct zeno_transport * restrict tp, ztimediff_t timeout);

    /* Request that data addressed by peers to the address in addr also be received on this transport
       (i.e., join a multicast group). Return value shall be 1 on success, 0 on failure. */ 
    int (*join)(const struct zeno_transport * restrict tp, const struct zeno_address * restrict addr);
} zeno_transport_ops_t;

#endif
