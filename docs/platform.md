# System interface

The user of *zhe* is expected to provide a handful of function and type definitions to interface with the rest of the system.

If the implementation requires some internal data, it can be held in a **struct zhe\_platform** to which a pointers is provided in **zhe_init** and passed whenever required. If no additional data is required, it is acceptable to leave the struct undefined and pass a null pointer.

Then there is the notion of a network address, **struct zhe_address**, which must be properly defined as objects of this type are stored inside internal structures of **zhe**. However, what constitutes an address is left open, and, e.g., a minimal client that connects to its broker via a serial line need not really use addresses. In this case defining it as a single byte and never reading it is perfectly acceptable. On the other hand, on a UDP/IP network, it makes sense to define it to consist of a **struct sockaddr_in**, and the TCP/IP example is primarily about connection numbers but also allows representing IP addresses.

These two types are then used in the following required interface functions:

* int **zhe\_platform\_addr\_eq**(const struct zhe\_address \*a, const struct zhe\_address \*b)
* size\_t **zhe\_platform\_addr2string**(const struct zhe\_platform \*pf, char \* restrict str, size\_t size, const struct zhe\_address \* restrict addr)
* int **zhe\_platform\_send**(struct zhe\_platform \*pf, const void * restrict buf, size\_t size, const struct zhe\_address \* restrict dst)
* void **zhe\_platform\_housekeeping**(struct zhe\_platform \*pf, zhe\_time\_t tnow)
* void **zhe\_platform\_close\_session**(struct zhe\_platform \*pf, const struct zhe\_address \* restrict addr)

The first shall return 1 if the two addresses *a* and *b* are equal, and 0 otherwise.

The second shall convert the address in *addr* to a standard string representation no longer than **TRANSPORT\_ADDRSTRLEN**-1, and store the result into *str*.
 No more than *size* bytes shall be written to *str*, and *str* shall be null-terminated. *Size* must
 consequently be > 0. The number of characters written into *str* shall be returned. (Failure is not an
 option.)
 
The third shall send the packet in *buf* of *size* bytes to address *dst* and return the number of bytes written (which
 must be *sz* for now, sending a packet only partially is not yet supported); or 0 to indicate nothing was written, **SENDRECV\_HANGUP** to indicate the
 other side hung up on us (i.e., when using TCP), or **SENDRECV\_ERROR** for unspecified, and therefore
 fatal errors. It is assumed to be non-blocking.
 
Then, the fourth is called whenever **zhe\_housekeeping**() is called, so that some background processing is possible. This is used, for example, by the TCP/IP example code to abandon connection attempts if establishing a connection over which Zenoh messages are being received takes longer than configured. The fifth is called whenever the generic *zhe* code closes a session with a peer, which allows a connection-oriented platform implementation (such as, again, the TCP/IP one) to close the corresponding network connection.

Then, if **ENABLE\_TRACING** evaluates to true, a tracing function analogous to **fprintf** (and interpreting the format string in the same manner) must be provided:

* void **zhe\_platform\_trace**(struct zhe\_platform \*pf, const char \*fmt, ...)

Finally, the platform specification must include a definition of a macro **zhe_assert**, which is the equivalent of the standard **assert** macro. That is:

```
#include <assert.h>
#define zhe_assert(x) assert(x)
```

is an excellent choice on a hosted environment.
