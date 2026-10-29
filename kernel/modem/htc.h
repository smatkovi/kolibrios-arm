/* KolibriOS for ARM: HTC, the host-target protocol over the WCN3990's copy
 * engines (htc.c; docs/pro1x-network.md, stage 4). After Linux's ath10k
 * htc.h and htc.c: every message has an 8-byte header (the endpoint, the
 * flags, the payload's length, a sequence number or the trailer's
 * length); endpoint 0 carries the control messages (the target's READY
 * with its credits, CONNECT_SERVICE and its response, which hands out an
 * endpoint per service, SETUP_COMPLETE_EX); a trailer of records at the
 * end of a received payload reports credits given back. */
#ifndef HTC_H
#define HTC_H
#include "modem.h"

#define HTC_HDR_LEN 8
#define HTC_MAX_LEN 4096
#define HTC_MAX_CTRL_MSG_LEN 256
#define HTC_EP_COUNT 9

/* header flags: sent, received */
#define HTC_FLAG_NEED_CREDIT_UPDATE 0x01
#define HTC_FLAG_TRAILER_PRESENT 0x02

/* the control messages (enum ath10k_ath10k_htc_msg_id) */
#define HTC_MSG_READY 1
#define HTC_MSG_CONNECT_SERVICE 2
#define HTC_MSG_CONNECT_SERVICE_RESP 3
#define HTC_MSG_SETUP_COMPLETE 4
#define HTC_MSG_SETUP_COMPLETE_EX 5

/* CONNECT_SERVICE's flags */
#define HTC_CONN_FLAGS_DISABLE_CREDIT_FLOW_CTRL (1u << 3)
#define HTC_CONN_FLAGS_RECV_ALLOC_LSB 8
#define HTC_CONN_STATUS_SUCCESS 0

/* the services: group << 8 | index */
#define HTC_SVC_RSVD_CTRL 0x0001
#define HTC_SVC_WMI_CONTROL 0x0100
#define HTC_SVC_WMI_DATA_BE 0x0101
#define HTC_SVC_WMI_DATA_BK 0x0102
#define HTC_SVC_WMI_DATA_VI 0x0103
#define HTC_SVC_WMI_DATA_VO 0x0104
#define HTC_SVC_HTT_DATA_MSG 0x0300
#define HTC_SVC_HTT_DATA2_MSG 0x0301
#define HTC_SVC_HTT_DATA3_MSG 0x0302
#define HTC_SVC_HTT_LOG_MSG 0x0600
#define HTC_SVC_TEST_RAW_STREAMS 0xfe00

/* the trailer's records */
#define HTC_RECORD_CREDITS 1

/* an endpoint: its service, its pipes (ce.h), its credits */
struct htc_ep {
	u16 service;
	u8 ul_pipe, dl_pipe;
	u16 max_msg;
	u8 seq;
	bool credit_flow;
	int credits;
};

struct htc {
	u16 credit_count, credit_size; /* from READY */
	u16 total_credits;             /* the WCN3990 (use_fw_tx_credits false): 1, not READY's count */
	u8 max_endpoints, version, max_bundle;
	bool ready, setup_done;
	struct htc_ep ep[HTC_EP_COUNT];
};

/* a message's header in front of its payload (out: HTC_HDR_LEN + len) */
void htc_put_hdr(u8 *out, u8 eid, u16 len, u8 flags, u8 seq);
/* to send on an endpoint: the header (its sequence number, the credit
 * update asked for while credits flow) put in front of payload's len
 * bytes at out + HTC_HDR_LEN; a credit taken. The length, or -1 (no
 * credit, not connected) */
int htc_prepare(struct htc *h, u8 eid, u8 *out, u16 len);

/* a received message: its endpoint, its payload without the trailer;
 * the trailer's credit reports given to their endpoints. 0, or -1 */
struct htc_rx {
	u8 eid, flags;
	const u8 *payload;
	u16 len;
};
int htc_receive(struct htc *h, const u8 *msg, u32 len, struct htc_rx *out);

/* the control messages (the payload at out, the header not included):
 * the length */
u32 htc_connect_msg(u8 *out, u16 service, u8 tx_alloc);
u32 htc_setup_complete_msg(u8 *out);
/* endpoint 0's payload: READY taken (0, or -1: not READY, no credits),
 * CONNECT_SERVICE_RESP taken: the endpoint set up with the service's
 * pipes (0, or -1: refused, or no such endpoint or pipe) */
int htc_ready(struct htc *h, const u8 *p, u32 len);
int htc_connect_resp(struct htc *h, const u8 *p, u32 len, u16 service, int *eid);
/* endpoint 0 (HTC's own control service: no credits) set up, as
 * ath10k_htc_init with ATH10K_HTC_SVC_ID_RSVD_CTRL */
void htc_init(struct htc *h);
/* the credits a service gets at connect: all of them for WMI control,
 * none for the rest (ath10k_htc_get_credit_allocation) */
u8 htc_credit_alloc(const struct htc *h, u16 service);

#endif
