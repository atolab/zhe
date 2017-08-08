/* -*- mode: c; c-basic-offset: 4; fill-column: 95; -*- */
#ifndef ZENO_MSG_H
#define ZENO_MSG_H

#define ZENO_VERSION        1

#define MSCOUT              1
#define MHELLO              2
#define MOPEN               3
#define MACCEPT             4
#define MCLOSE              5
#define MDECLARE            6
#define MSDATA              7
#define MBDATA              8 /* FIXME: NIY */
#define MWDATA              9 /* FIXME: NIY */
#define MQUERY             10 /* FIXME: NIY */
#define MPULL              11 /* FIXME: NIY */
#define MPING              12
#define MPONG              13
#define MSYNCH             14
#define MACKNACK           15
#define MKEEPALIVE         16
#define MCONDUIT_CLOSE     17 /* FIXME: NIY */
#define MFRAGMENT          18 /* FIXME: NIY */
#define MCONDUIT           19

#define MRFLAG            128
#define MSFLAG             64
#define MXFLAG             32
#define MKIND            0x1f

#define MPFLAG         MXFLAG /* RID of content present in SDATA */
#define MMFLAG         MXFLAG /* mask present in ACKNACK (i.e., a NACK) */
#define MLFLAG         MXFLAG /* seqnum length present in OPEN */
#define MZFLAG         MRFLAG /* beware of MCONDUIT flags! */

#define DRESOURCE           1
#define DPUB                2
#define DSUB                3
#define DSELECTION          4
#define DBINDID             5
#define DCOMMIT             6
#define DRESULT             7
#define DDELETERES          8
#define DKIND             0xf

#define DPFLAG           0x80

#define CREAS_INVALID_AUTH  1
#define CREAS_UNSUPP_PROTO  2
#define CREAS_OUT_OF_RES    3
#define CREAS_UNSUPP_SEQLEN 4

#define MSCOUT_BROKER       1
#define MSCOUT_DURABILITY   2
#define MSCOUT_PEER         4
#define MSCOUT_CLIENT       8

#define SUBMODE_PUSH        1
#define SUBMODE_PULL        2
#define SUBMODE_PERIODPUSH  3
#define SUBMODE_PERIODPULL  4
#define SUBMODE_PUSHPULL    5
#define SUBMODE_MAX         SUBMODE_PUSHPULL

#endif
