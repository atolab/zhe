/* -*- mode: c; c-basic-offset: 4; fill-column: 95; -*- */
#ifndef ZHE_CONFIG_INT_H
#define ZHE_CONFIG_INT_H

#include "zhe-config.h"
#include "platform-arduino.h"

#define MAX_PEERS 0
#define N_IN_CONDUITS 2
#define N_OUT_CONDUITS 1
#define HAVE_UNICAST_CONDUIT 1
#define MAX_MULTICAST_GROUPS 0
#define XMITW_BYTES_UNICAST 384u
#define XMITW_SAMPLES_UNICAST 63u
#define XMITW_SAMPLE_INDEX 0

#define ENABLE_TRACING 0

/* Setting a latency budget globally for now, though it could be done per-publisher as well. Packets will go out when full or when LATENCY_BUDGET milliseconds passed since we started filling it. Setting it to 0 will disable packing of data messages, setting to INF only stops packing when the MTU is reached and generally requires explicit flushing. Both edge cases eliminate the latency budget handling and state from the code, saving a whopping 4 bytes of RAM!  */
#define LATENCY_BUDGET_INF      (4294967295u)
#define LATENCY_BUDGET         10 /* units, see ZHE_TIMEBASE */

/* Send a SYNCH message set every MSYNCH_INTERVAL ms when unack'd messages are present in the transmit window. Ideally this would be based on a measured round-trip time, but instead it is based on an estimate of the round-trip time. */
#define MSYNCH_INTERVAL        10 /* units, see ZHE_TIMEBASE */
#define ROUNDTRIP_TIME_ESTIMATE 1 /* units, see ZHE_TIMEBASE */

/* Scouts are sent periodically by a peer; by a client only when not connected to, or trying to connect to, a broker. The interval is configurable. Scouts are always multicasted (however implemented by the transport). */
#define SCOUT_INTERVAL       3000 /* units, see ZHE_TIMEBASE */

/* Once new peer/a broker has been discovered, a number of attempts at establishing a connection take place. The interval between these attempts is OPEN_INTERVAL, the number of attempts before giving up and relying on scouting again is OPEN_RETRIES. */
#define OPEN_INTERVAL        1000 /* units, see ZHE_TIMEBASE */
#define OPEN_RETRIES           10 /* limited by OPENING_MIN .. _MAX */

/* Lease duration should be greater than SCOUT_INTERVAL */
#define LEASE_DURATION      30000 /* units, see ZHE_TIMEBASE */

/* Peer IDs are non-empty byte vectors of at most PEERID_SIZE. Peers that provide a peer id that does not meet these requirements are ignored */
#define PEERID_SIZE             4

/* Sequence numbers are represented on the wire as VLE, but internally as a fixed size unsigned integer, but counting only 7 bits per byte (there is no point in having mostly unused bytes for the sequence number most of the time). So, uint16_t is a 14-bit sequence number, uint8_t a 7-bit one, uint32_t a 28-bit one and uint64_t a 56-bit one. The transmit window size and the sequence number are related: the window must be full before the window contains as many samples as half the range can represent (i.e., for a 14-bit sequence number the window must be smaller than 8192 messages, worst-case). */
#define SEQNUM_LEN              7u

/* We're pretty dependent on making no typos in HAVE_UNICAST_CONDUIT, so it seems sensible to
   enable warnings for the use of undefined macros */
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6) || __clang__
#pragma GCC diagnostic warning "-Wundef"
#endif

#endif
