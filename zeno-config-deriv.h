#ifndef ZENO_CONFIG_DERIV_H
#define ZENO_CONFIG_DERIV_H

#include "zeno-config-int.h"

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
#if TRANSPORT_MTU < 16 || TRANSPORT_MTU > 65534
#  error "transport configuration did not set MTU properly"
#endif

#if MAX_PEERS > 1 && N_OUT_MCONDUITS == 0
#  error "MAX_PEERS > 1 requires presence of multicasting conduit"
#endif

#if MAX_PEERS <= 1 && ! HAVE_UNICAST_CONDUIT
#  warning "should use a unicast conduit in a client or if there can be at most one peer"
#endif

#if MAX_PEERS < 255
typedef uint8_t peeridx_t;
#else
#  error "MAX_PEERS is too large for 8-bit peer idx"
#endif
#define PEERIDX_INVALID ((peeridx_t)-1)

#if TRANSPORT_MTU < 254
typedef uint8_t zmsize_t; /* type used for representing the size of an XRCE message */
#else
typedef uint16_t zmsize_t;
#endif

/* zmsize_t is the type capable of representing the maximum size of a message and may not
 be larger than the zpsize_t, the type capable of representing the maximum size of a
 payload (fragmentation in principle allows for larger payload (components) than message);
 type conversions occur under the assumption that a zpsize_t can always hold a zmsize_t. */
struct zmsize_leq_zpsize_t {
    char req[sizeof(zmsize_t) <= sizeof(zpsize_t) ? 1 : -1];
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
#define SEQNUM_LEN          (7u * sizeof(seq_t))
#define SEQNUM_SHIFT        (sizeof(seq_t))
#define SEQNUM_UNIT         ((seq_t)(1 << SEQNUM_SHIFT))

#endif
