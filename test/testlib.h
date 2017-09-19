#ifndef TESTLIB_H
#define TESTLIB_H

#include <stddef.h>
#include "zhe.h"

zhe_paysize_t getrandomid(unsigned char *ownid, size_t ownidsize);
zhe_paysize_t getidfromarg(unsigned char *ownid, size_t ownidsize, const char *in);
void cfg_handle_addrs(struct zhe_config *cfg, struct zhe_platform *platform, const char *scoutaddrstr, const char *mcgroups_join_str, const char *mconduit_dstaddrs_str);

#endif
