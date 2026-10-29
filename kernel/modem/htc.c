/* KolibriOS for ARM: HTC over the WCN3990's copy engines (htc.h), after
 * Linux's ath10k htc.c: ath10k_htc_prepare_tx_skb, ath10k_htc_consume_credit,
 * ath10k_htc_rx_completion_handler, ath10k_htc_process_trailer,
 * ath10k_htc_wait_target, ath10k_htc_connect_service, ath10k_htc_start. */
#include "htc.h"
#include "ce.h"

void htc_put_hdr(u8 *out, u8 eid, u16 len, u8 flags, u8 seq)
{
	out[0] = eid;
	out[1] = flags;
	put16(out + 2, len);
	out[4] = 0;
	out[5] = seq;
	put16(out + 6, 0);
}

static void ep_reset(struct htc_ep *e)
{
	mzero(e, sizeof(*e));
	e->credit_flow = true;                  /* ath10k_htc_reset_endpoint_states */
}

void htc_init(struct htc *h)
{
	mzero(h, sizeof(*h));
	for (int i = 0; i < HTC_EP_COUNT; i++) ep_reset(&h->ep[i]);
	/* the pseudo control service on endpoint 0: no credit flow */
	struct htc_ep *e = &h->ep[0];
	u32 ul = 0, dl = 0;
	ce_service_pipes(HTC_SVC_RSVD_CTRL, &ul, &dl);
	e->service = HTC_SVC_RSVD_CTRL;
	e->ul_pipe = (u8)ul;
	e->dl_pipe = (u8)dl;
	e->max_msg = HTC_MAX_CTRL_MSG_LEN;
	e->credit_flow = false;
}

int htc_prepare(struct htc *h, u8 eid, u8 *out, u16 len)
{
	if (eid >= HTC_EP_COUNT || !h->ep[eid].service) return -1;
	struct htc_ep *e = &h->ep[eid];
	u32 total = HTC_HDR_LEN + len;
	if (e->credit_flow) {
		if (!h->credit_size) return -1;
		int need = (int)((total + h->credit_size - 1) / h->credit_size);
		if (e->credits < need) return -1;
		e->credits -= need;
	}
	htc_put_hdr(out, eid, len, e->credit_flow ? HTC_FLAG_NEED_CREDIT_UPDATE : 0, e->seq++);
	return (int)total;
}

/* the trailer's records: credit reports (eid, credits, two pads) */
static int trailer(struct htc *h, const u8 *t, u32 len)
{
	while (len) {
		if (len < 4) return -1;
		u8 id = t[0], rl = t[1];
		if (rl > len - 4) return -1;
		if (id == HTC_RECORD_CREDITS)
			for (u32 i = 0; i + 4 <= rl; i += 4) {
				u8 eid = t[4 + i], credits = t[4 + i + 1];
				if (eid >= HTC_EP_COUNT) break;
				h->ep[eid].credits += credits;
			}
		t += 4 + rl;
		len -= 4 + rl;
	}
	return 0;
}

int htc_receive(struct htc *h, const u8 *msg, u32 len, struct htc_rx *out)
{
	if (len < HTC_HDR_LEN) return -1;
	u8 eid = msg[0], flags = msg[1];
	u16 payload = get16(msg + 2);
	if (eid >= HTC_EP_COUNT || !h->ep[eid].service) return -1;
	if (payload + HTC_HDR_LEN > HTC_MAX_LEN || len - HTC_HDR_LEN < payload) return -1;
	u32 tl = 0;
	if (flags & HTC_FLAG_TRAILER_PRESENT) {
		tl = msg[4];
		if (tl < 4 || tl > payload) return -1;
		if (trailer(h, msg + HTC_HDR_LEN + payload - tl, tl)) return -1;
	}
	out->eid = eid;
	out->flags = flags;
	out->payload = msg + HTC_HDR_LEN;
	out->len = (u16)(payload - tl);
	return 0;
}

u32 htc_connect_msg(u8 *out, u16 service, u8 tx_alloc)
{
	u16 flags = (u16)(tx_alloc << HTC_CONN_FLAGS_RECV_ALLOC_LSB);
	/* credit flow only for WMI's control service */
	if (service != HTC_SVC_WMI_CONTROL) flags |= HTC_CONN_FLAGS_DISABLE_CREDIT_FLOW_CTRL;
	mzero(out, 8);
	put16(out, HTC_MSG_CONNECT_SERVICE);
	put16(out + 2, service);
	put16(out + 4, flags);
	return 8;
}

u32 htc_setup_complete_msg(u8 *out)
{
	/* the message id, two pads, the flags (0: no bundles over SNOC), the
	 * bundle size, three pads */
	mzero(out, 12);
	put16(out, HTC_MSG_SETUP_COMPLETE_EX);
	return 12;
}

u8 htc_credit_alloc(const struct htc *h, u16 service)
{
	return service == HTC_SVC_WMI_CONTROL ? (u8)h->total_credits : 0;
}

int htc_ready(struct htc *h, const u8 *p, u32 len)
{
	if (len < 2 + 6 || get16(p) != HTC_MSG_READY) return -1;
	h->credit_count = get16(p + 2);
	h->credit_size = get16(p + 4);
	h->max_endpoints = p[6];
	h->total_credits = 1;
	if (!h->credit_count || !h->credit_size) return -1;
	/* the extended one, known by its size */
	if (len >= 2 + 10) {
		h->version = p[8];
		h->max_bundle = MIN(p[9], (u8)32);
	}
	h->ready = true;
	return 0;
}

int htc_connect_resp(struct htc *h, const u8 *p, u32 len, u16 service, int *eid)
{
	if (len < 2 + 6 || get16(p) != HTC_MSG_CONNECT_SERVICE_RESP) return -1;
	if (get16(p + 2) != service || p[4] != HTC_CONN_STATUS_SUCCESS) return -1;
	u8 e = p[5];
	u16 max = get16(p + 6);
	if (e >= HTC_EP_COUNT || !max || h->ep[e].service) return -1;
	u32 ul, dl;
	if (ce_service_pipes(service, &ul, &dl)) return -1;
	struct htc_ep *ep = &h->ep[e];
	ep->service = service;
	ep->max_msg = max;
	ep->ul_pipe = (u8)ul;
	ep->dl_pipe = (u8)dl;
	ep->credits = htc_credit_alloc(h, service);
	ep->credit_flow = service == HTC_SVC_WMI_CONTROL;
	*eid = e;
	return 0;
}
