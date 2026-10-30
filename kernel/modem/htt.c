/* KolibriOS for ARM: HTT's messages (htt.h), as Linux's ath10k builds and
 * reads them: ath10k_htt_h2t_ver_req_msg, ath10k_htt_send_frag_desc_bank_cfg_64,
 * ath10k_htt_send_rx_ring_cfg_64 (with ath10k_rx_desc_wcn3990_get_offsets),
 * ath10k_htt_h2t_aggr_cfg_msg_v2, ath10k_htt_tx_64, ath10k_htt_rx_in_ord_ind.
 * The structures are packed and little endian (htt.h of Linux). */
#include "htt.h"

u32 htt_ver_req(u8 *o)
{
	/* htt_cmd_hdr, htt_ver_req: three bytes of padding */
	mzero(o, 4);
	o[0] = HTT_H2T_VERSION_REQ;
	return 4;
}

u32 htt_frag_bank_cfg(u8 *o, u64 bank_pa, u16 max_id)
{
	/* htt_frag_desc_bank_cfg64: info (no queue state: the firmware has
	 * no ATH10K_FW_FEATURE_PEER_FLOW_CONTROL), one bank, the descriptor's
	 * size, four bank addresses, four id ranges, the queue state (zero) */
	mzero(o, 64);
	o[0] = HTT_H2T_FRAG_DESC_BANK_CFG;
	o[1] = 0;
	o[2] = 1;
	o[3] = HTT_EXT_DESC_LEN;
	put64(o + 4, bank_pa);
	put16(o + 36, 0);
	put16(o + 38, max_id);
	/* q_state at 52: paddr 0, peers 0, tids 0, record size 1, multiplier 0 */
	o[52 + 8] = 1;                          /* HTT_TX_Q_STATE_ENTRY_SIZE */
	return 64;
}

u32 htt_rx_ring_cfg(u8 *o, u64 idx_pa, u64 ring_pa, u16 ring_len, u16 fw_idx)
{
	/* htt_rx_ring_setup_64: hdr (one ring, two reserved), then
	 * htt_rx_ring_setup_ring64 */
	mzero(o, 48);
	o[0] = HTT_H2T_RX_RING_CFG;
	o[1] = 1;
	u8 *r = o + 4;
	put64(r, idx_pa);                       /* fw_idx_shadow_reg_paddr */
	put64(r + 8, ring_pa);
	put16(r + 16, ring_len);
	put16(r + 18, HTT_RX_BUF_SIZE);
	/* HTT_RX_RING_FLAGS_*: every part of the descriptor, every kind of frame */
	put16(r + 20, 0xffff);
	put16(r + 22, fw_idx);
	/* the offsets: mac80211 header, payload, ppdu start/end, mpdu
	 * start/end, msdu start/end, attention, frag info */
	static const u16 w[10] = {
		HTT_RX_W_HDR_STATUS, HTT_RX_W_PAYLOAD, HTT_RX_W_PPDU_START, HTT_RX_W_PPDU_END, HTT_RX_W_MPDU_START,
		HTT_RX_W_MPDU_END, HTT_RX_W_MSDU_START, HTT_RX_W_MSDU_END, HTT_RX_W_ATTENTION, HTT_RX_W_FRAG_INFO,
	};
	for (int i = 0; i < 10; i++) put16(r + 24 + 2 * i, w[i]);
	return 48;
}

u32 htt_aggr_cfg(u8 *o, u8 ampdu, u8 amsdu)
{
	o[0] = HTT_H2T_AGGR_CFG;
	o[1] = ampdu;
	o[2] = amsdu;
	o[3] = 0;
	return 4;
}

void htt_tx_desc(u8 *d, u32 mode, u32 vdev, u32 tid, u16 len, u16 id, u64 frags_pa, bool no_encrypt)
{
	u8 f0 = (u8)(mode << HTT_TX_FLAGS0_PKT_TYPE_LSB);
	if (mode != HTT_TX_MODE_ETHERNET) f0 |= HTT_TX_FLAGS0_MAC_HDR_PRESENT;
	if (no_encrypt) f0 |= HTT_TX_FLAGS0_NO_ENCRYPT;
	/* no tx inspection requests: a discard status instead */
	u16 f1 = (u16)((vdev & 0x3f) | (tid & 0x1f) << HTT_TX_FLAGS1_TID_LSB | HTT_TX_FLAGS1_POSTPONED);
	d[0] = f0;
	put16(d + 1, f1);
	put16(d + 3, len);
	put16(d + 5, id);
	put64(d + 7, frags_pa);
	/* the peer id (ath10k_mac_tx_frm_has_freq: HTT 3.4 and later have
	 * the off-channel frequency in its upper half: 0 here) */
	put16(d + 15, HTT_INVALID_PEER);
	put16(d + 17, 0);
	/* d + 19..22: the first bytes (prefetch) follow in the message */
}

void htt_ext_desc(u8 *e, u64 pa, u16 len)
{
	mzero(e, HTT_EXT_DESC_LEN);
	/* frags[0] at 24: paddr_lo, paddr_hi (16 bits), len_16 */
	put32(e + 24, (u32)pa);
	put16(e + 28, (u16)(pa >> 32));
	put16(e + 30, len);
}

int htt_parse_in_ord(const u8 *p, u32 len, struct htt_in_ord *o)
{
	/* htt_rx_in_ord_ind: info, peer id, vdev, reserved, the count,
	 * then htt_rx_in_ord_msdu_desc_ext */
	if (len < 7) return -1;
	o->tid = p[0] & 0x1f;
	o->offload = p[0] & 0x20;
	o->frag = p[0] & 0x40;
	o->peer_id = get16(p + 1);
	o->vdev = p[3];
	o->count = get16(p + 5);
	o->descs = p + 7;
	return (u32)o->count * 12 <= len - 7 ? 0 : -1;
}

void htt_in_ord_buf(const struct htt_in_ord *o, u32 i, u64 *pa, u16 *len)
{
	const u8 *d = o->descs + 12 * i;
	*pa = get64(d);
	*len = get16(d + 8);
}
