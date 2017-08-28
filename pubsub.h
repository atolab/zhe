#ifndef PUBSUB_H
#define PUBSUB_H

#define WC_DRESULT_SIZE     8 /* worst-case result size: header, commitid, status, rid */
#define WC_DCOMMIT_SIZE     2 /* commit: header, commitid */
#define WC_DPUB_SIZE        6 /* pub: header, rid (not using properties) */
#define WC_DSUB_SIZE        7 /* sub: header, rid, mode (neither properties nor periodic modes) */

void zhe_decl_note_error(uint8_t bitmask, zhe_rid_t rid);
int zhe_handle_msdata_deliver(zhe_rid_t prid, zhe_paysize_t paysz, const void *pay);

void zhe_rsub_register(peeridx_t peeridx, zhe_rid_t rid, uint8_t submode);
uint8_t zhe_rsub_precommit(peeridx_t peeridx, zhe_rid_t *err_rid);
void zhe_rsub_commit(peeridx_t peeridx);
void zhe_rsub_precommit_curpkt_abort(peeridx_t peeridx);
void zhe_rsub_clear(peeridx_t peeridx);
void zhe_rsub_precommit_curpkt_done(peeridx_t peeridx);

void zhe_send_declares(zhe_time_t tnow);

void zhe_reset_pubs_to_declare(void);
void zhe_reset_subs_to_declare(void);

#endif
