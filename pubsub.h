#ifndef PUBSUB_H
#define PUBSUB_H

#define WC_DRESULT_SIZE     8 /* worst-case result size: header, commitid, status, rid */
#define WC_DCOMMIT_SIZE     2 /* commit: header, commitid */
#define WC_DPUB_SIZE        6 /* pub: header, rid (not using properties) */
#define WC_DSUB_SIZE        7 /* sub: header, rid, mode (neither properties nor periodic modes) */

void decl_note_error(uint8_t bitmask, rid_t rid);
int handle_msdata_deliver(rid_t prid, zpsize_t paysz, const void *pay);

void rsub_register(peeridx_t peeridx, rid_t rid, uint8_t submode);
uint8_t rsub_precommit(peeridx_t peeridx, rid_t *err_rid);
void rsub_commit(peeridx_t peeridx);
void rsub_precommit_curpkt_abort(peeridx_t peeridx);
void rsub_clear(peeridx_t peeridx);
void rsub_precommit_curpkt_done(peeridx_t peeridx);

void send_declares(void);

void reset_pubs_to_declare(void);
void reset_subs_to_declare(void);

#endif
