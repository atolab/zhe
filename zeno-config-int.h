/* -*- mode: c; c-basic-offset: 4; fill-column: 95; -*- */
#ifndef ZENO_CONFIG_INT_H
#define ZENO_CONFIG_INT_H

#include "zeno-config.h"

/********** Arduino Hack Alert ***********/

#ifndef ARDUINO

#  include "transport-udp.h"

/* Maximum number of peers one node can have (that is, the network
   may consist of at most MAX_PEERS+1 nodes). If MAX_PEERS is 0,
   it becomes a client rather than as a peer */
#  define MAX_PEERS 12

#  define N_IN_CONDUITS 3
#  define N_OUT_CONDUITS 3
#  define HAVE_UNICAST_CONDUIT 1

/* Transmit window size, each reliable message is prefixed by its size in a single byte. */
#  define XMITW_BYTES 16384u
#  define XMITW_BYTES_UNICAST 384u

#else /* defined ARDUINO -- just to check it builds */

#  include "transport-arduino.h"

#  define MAX_PEERS 0
#  define N_IN_CONDUITS 2
#  define N_OUT_CONDUITS 1
#  define HAVE_UNICAST_CONDUIT 1
#  define XMITW_BYTES_UNICAST 384u

#endif /* defined ARDUINO */

/********** End of Arduino Hack ***********/

#if MAX_PEERS < 255
typedef uint8_t peeridx_t;
#else
#  error "MAX_PEERS is too large for 8-bit peer idx"
#endif
#define PEERIDX_INVALID ((peeridx_t)-1)

/* Setting a latency budget globally for now, though it could be done per-publisher as well.
   Packets will go out when full or when LATENCY_BUDGET milliseconds passed since we started
   filling it.  Setting it to 0 will disable packing of data messages, setting to INF only
   stops packing when the MTU is reached and generally requires explicit flushing.  Both edge
   cases eliminate the latency budget handling and state from the code, saving a whopping 4
   bytes of RAM!  */
#define LATENCY_BUDGET_INF   (4294967295u)
#define LATENCY_BUDGET       10 /* ms */

/* Send a SYNCH message set every MSYNCH_INTERVAL ms when unack'd messages are present in the
   transmit window.  Ideally would base this on measured round-trip time.  Messages with the S
   bit set also reset this. */
#define MSYNCH_INTERVAL    300 /* ms */

#define SCOUT_INTERVAL    3000 /* millis */
#define OPEN_INTERVAL     3000 /* millis */
#define OPEN_RETRIES        10 /* limited by OPENING_MIN .. _MAX */

#define PEERID_SIZE 16

typedef uint16_t seq_t;   /* type internally used for representing sequence numbers */
typedef int16_t sseq_t;   /* signed version of seq_t */
#if TRANSPORT_MTU < 254
typedef uint8_t zmsize_t; /* type used for representing the size of an XRCE message */
#else
typedef uint16_t zmsize_t;
#endif

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
#  error "Conduits are limited to 127 because the VLE encoding is short-circuited"
#endif

/* zmsize_t is the type capable of representing the maximum size of a message and may not
   be larger than the zpsize_t, the type capable of representing the maximum size of a
   payload (fragmentation in principle allows for larger payload (components) than message);
   type conversions occur under the assumption that a zpsize_t can always hold a zmsize_t. */
struct zmsize_leq_zpsize_t {
    char req[sizeof(zmsize_t) <= sizeof(zpsize_t) ? 1 : -1];
};

#define SUFFIX_WITH_SIZE1(name, size) name##size
#define SUFFIX_WITH_SIZE(name, size) SUFFIX_WITH_SIZE1(name, size)

#define INFIX_WITH_SIZE1(name, size, suf) name##size##suf
#define INFIX_WITH_SIZE(name, size, suf) INFIX_WITH_SIZE1(name, size, suf)

/* Size of sequence number in bits is "negotiated" -- that is, determined by the client, so we
   get to choose.  Sequence numbers are VLE on the wire (to allow decoding messages without
   knowing the sequence number size), but they are a multiple of 7 bits to avoid spending a
   byte of which only a few bits will be used.  Sequence numbers are internally represented as
   16-bit unsigned numbers.  */
#define SEQNUM_LEN          (7u * sizeof(seq_t))
#define SEQNUM_SHIFT        (sizeof(seq_t))
#define SEQNUM_UNIT         ((seq_t)(1 << SEQNUM_SHIFT))

#endif
