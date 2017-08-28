#ifndef ZHE_PLATFORM_H
#define ZHE_PLATFORM_H

#include <stddef.h>

#include "zhe-config-int.h"

/* modes for transport should not include 0 */
#define TRANSPORT_PACKET 1
#define TRANSPORT_STREAM 2

#define SENDRECV_HANGUP (-1)
#define SENDRECV_ERROR  (-2)

struct zhe_address;
struct zhe_platform;

/* Return 0 if a and b are different addresses, 1 if they are the same */
int zhe_platform_addr_eq(const struct zhe_address *a, const struct zhe_address *b);

/* Convert the address in ADDR to a standard string representation, and write the result into str.
 No more than size bytes shall be written to str, and str shall be null-terminated. Size must
 consequently be > 0. Number of characters written into str shall be returned. Failure is not an
 option. */
size_t zhe_platform_addr2string(const struct zhe_platform *pf, char * restrict str, size_t size, const struct zhe_address * restrict addr);

/* Sends the packet in buf of size bytes to address dst. Returns the number of bytes written (which
 must be sz for now), or 0 to indicate nothing was written, or SENDRECV_HANGUP to indicate the
 other side hung up on us (i.e., when using TCP), or SENDRECV_ERROR for unspecified, and therefore
 fatal errors. Should be non-blocking. */
int zhe_platform_send(struct zhe_platform *pf, const void * restrict buf, size_t size, const struct zhe_address * restrict dst);

void zhe_platform_trace(struct zhe_platform *pf, const char *fmt, ...);

#endif
