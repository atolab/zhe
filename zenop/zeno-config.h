/* -*- mode: c; c-basic-offset: 4; fill-column: 95; -*- */
#ifndef ZENO_CONFIG_H
#define ZENO_CONFIG_H

#include <stdint.h>

/* Size of unsigned integer used to represent a resource/selection
   id internally (externally it is always variable-length encoded) */
#define RID_T_SIZE 32

/* Maximum number of peers one node can have (that is, the network
   may consist of at most MAX_PEERS+1 nodes). If MAX_PEERS is 0,
   it becomes a client rather than as a peer */
#define MAX_PEERS 12
#if MAX_PEERS < 256
typedef uint8_t peeridx_t;
#else
#error "MAX_PEERS is too large for 8-bit peer idx"
#endif

#define N_IN_CONDUITS 3
#define N_OUT_CONDUITS 3

#define ZENO_MAKE_UINT_T1(size) uint##size##_t
#define ZENO_MAKE_UINT_T(size) ZENO_MAKE_UINT_T1(size)

typedef ZENO_MAKE_UINT_T(RID_T_SIZE) rid_t;
typedef uint32_t ztime_t;
typedef uint16_t zpsize_t; /* type used for representing payload sizes (including the length of sequences) */

///
#include <sys/socket.h>
typedef struct sockaddr_storage zeno_address_t;
///

#endif
