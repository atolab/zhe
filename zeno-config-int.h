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
#  define MAX_MULTICAST_GROUPS 5

/* Transmit window size, each reliable message is prefixed by its size in a single byte. */
#  define XMITW_BYTES 16384u
#  define XMITW_BYTES_UNICAST 384u

#else /* defined ARDUINO -- just to check it builds */

#  include "transport-arduino.h"

#  define MAX_PEERS 0
#  define N_IN_CONDUITS 2
#  define N_OUT_CONDUITS 1
#  define HAVE_UNICAST_CONDUIT 1
#  define MAX_MULTICAST_GROUPS 0
#  define XMITW_BYTES_UNICAST 384u

#endif /* defined ARDUINO */

/********** End of Arduino Hack ***********/

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
#define MSYNCH_INTERVAL        10 /* ms */
#define ROUNDTRIP_TIME_ESTIMATE 1 /* ms */

#define SCOUT_INTERVAL    3000 /* millis */
#define OPEN_INTERVAL     3000 /* millis */
#define OPEN_RETRIES        10 /* limited by OPENING_MIN .. _MAX */

#define PEERID_SIZE 16

typedef uint16_t seq_t;   /* type internally used for representing sequence numbers */
typedef int16_t sseq_t;   /* signed version of seq_t */

/* We're pretty dependent on making no typos in HAVE_UNICAST_CONDUIT, so it seems sensible to
   enable warnings for the use of undefined macros */
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6) || __clang__
#pragma GCC diagnostic warning "-Wundef"
#endif

#endif
