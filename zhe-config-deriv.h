#ifndef ZHE_CONFIG_DERIV_H
#define ZHE_CONFIG_DERIV_H

#include <limits.h>
#include "zhe-config-int.h"
#include "zhe-rid.h"

#define MAX_PEERS_1 (MAX_PEERS == 0 ? 1 : MAX_PEERS)

#if ! HAVE_UNICAST_CONDUIT
#  define N_OUT_MCONDUITS N_OUT_CONDUITS
#  define MAX_MULTICAST_CID (N_OUT_CONDUITS - 1)
#else
#  define UNICAST_CID (N_OUT_CONDUITS - 1)
#  define N_OUT_MCONDUITS (N_OUT_CONDUITS - 1)
#  if N_OUT_CONDUITS > 1
#    define MAX_MULTICAST_CID (N_OUT_MCONDUITS - 2)
#  endif
#endif

#if TRANSPORT_MODE != TRANSPORT_STREAM && TRANSPORT_MODE != TRANSPORT_PACKET
#  error "transport configuration did not set MODE properly"
#endif

/* There is some lower limit that really won't work anymore, but I actually know what that is, so the 16 is just a placeholder (but it is roughly correct); 16-bit unsigned indices are used to index a packet, with the maximum value used as an exceptional value, so larger than 2^16-2 is also no good; and finally, the return type of zhe_input is an int, and so the number of consumed bytes must fit in an int */
#if TRANSPORT_MTU < 16 || TRANSPORT_MTU > 65534 || TRANSPORT_MTU > INT_MAX
#  error "transport configuration did not set MTU properly"
#endif

#if MAX_PEERS > 1 && N_OUT_MCONDUITS == 0
#  error "MAX_PEERS > 1 requires presence of multicasting conduit"
#endif

#if MAX_PEERS <= 1 && ! HAVE_UNICAST_CONDUIT
#  warning "should use a unicast conduit in a client or if there can be at most one peer"
#endif

#if LEASE_DURATION <= SCOUT_INTERVAL
#  warning "scout interval should be shorter than lease duration"
#endif

#if MAX_PEERS < 255
typedef uint8_t peeridx_t;
#else
#  error "MAX_PEERS is too large for 8-bit peer idx"
#endif
#define PEERIDX_INVALID ((peeridx_t)-1)

#if TRANSPORT_MTU < 254
typedef uint8_t zhe_msgsize_t; /* type used for representing the size of an XRCE message */
#else
typedef uint16_t zhe_msgsize_t;
#endif

/* zhe_msgsize_t is the type capable of representing the maximum size of a message and may not
 be larger than the zhe_paysize_t, the type capable of representing the maximum size of a
 payload (fragmentation in principle allows for larger payload (components) than message);
 type conversions occur under the assumption that a zhe_paysize_t can always hold a zhe_msgsize_t. */
struct msgsize_leq_paysize_t {
    char req[sizeof(zhe_msgsize_t) <= sizeof(zhe_paysize_t) ? 1 : -1];
};

/* There is not a fundamental limit on the number of conduits, but there are some places
 where a conduit id is assumed to fit in a single byte in message processing, and there
 are some places where a signed integer is used to index either conduit or peer. */
#if N_OUT_CONDUITS <= 127 && N_IN_CONDUITS <= 127 && MAX_PEERS <= 127
typedef int8_t cid_t;
#  define MAX_CID_T 127
#elif N_OUT_CONDUITS <= 127 && N_IN_CONDUITS <= 127 && MAX_PEERS <= 32767
typedef int16_t cid_t;
#  define MAX_CID_T 32767
#else
#  error "Conduits are limited to 127 because the VLE encoding is short-circuited for CIDs"
#endif

/* Size of sequence number in bits is "negotiated" -- that is, determined by the client, so we
 get to choose.  Sequence numbers are VLE on the wire (to allow decoding messages without
 knowing the sequence number size), but they are a multiple of 7 bits to avoid spending a
 byte of which only a few bits will be used.  Sequence numbers are internally represented as
 16-bit unsigned numbers.  */
#if SEQNUM_LEN == 7
typedef uint8_t seq_t;    /* type internally used for representing sequence numbers */
typedef int8_t sseq_t;    /* signed version of seq_t */
#elif SEQNUM_LEN == 14
typedef uint16_t seq_t;
typedef int16_t sseq_t;
#elif SEQNUM_LEN == 28
typedef uint32_t seq_t;
typedef int32_t sseq_t;
#elif SEQNUM_LEN == 56
typedef uint64_t seq_t;
typedef int64_t sseq_t;
#else
#error "SEQNUM_LEN must be either 7, 14, 28 or 56"
#endif
#define SEQNUM_SHIFT        (sizeof(seq_t))
#define SEQNUM_UNIT         ((seq_t)(1 << SEQNUM_SHIFT))

#if ZHE_TIMEBASE != 1000000
#warning "better get the time conversions correct first ..."
#endif
#define ZTIME_TO_SECu32(zt) ((uint32_t)((zt) / (1000000000 / ZHE_TIMEBASE)))
#define ZTIME_TO_MSECu32(zt) ((uint32_t)((zt) / (1000000 / ZHE_TIMEBASE)) % 1000u)

#endif
