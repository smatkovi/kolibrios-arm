/* KolibriOS for ARM: HTT, the WCN3990's data path over HTC (htt.c;
 * docs/pro1x-network.md, stage 6). After Linux's ath10k htt.h, htt.c,
 * htt_tx.c (the 64-bit forms: htt_tx_ops_64) and htt_rx.c, for the
 * firmware's HTT "TLV" message numbers (firmware-5.bin: HTT op version 3):
 *
 *  - host -> target: VERSION_REQ; FRAG_DESC_BANK_CFG (one bank of
 *    htt_msdu_ext_desc_64, the TX fragment lists); RX_RING_CFG (the RX
 *    ring of buffer addresses, the index the host fills it to, where the
 *    descriptors' parts are in each buffer: the WCN3990's htt_rx_desc_v2);
 *    AGGR_CFG (v2); TX_FRM (a data or management frame: its descriptor,
 *    then the frame's first bytes, through CE4 without HTC's send path)
 *  - target -> host: VERSION_CONF, PEER_MAP/UNMAP, RX_IN_ORD_PADDR_IND
 *    (filled RX buffers by address: WMI_SERVICE_RX_FULL_REORDER),
 *    TX_COMPL_IND (sent frames by id), MGMT_TX_COMPL_IND. */
#ifndef HTT_H
#define HTT_H
#include "modem.h"

/* host -> target */
#define HTT_H2T_VERSION_REQ 0
#define HTT_H2T_TX_FRM 1
#define HTT_H2T_RX_RING_CFG 2
#define HTT_H2T_AGGR_CFG 5
#define HTT_H2T_FRAG_DESC_BANK_CFG 6
/* target -> host (enum htt_tlv_t2h_msg_type) */
#define HTT_T2H_VERSION_CONF 0x0
#define HTT_T2H_RX_IND 0x1
#define HTT_T2H_PEER_MAP 0x3
#define HTT_T2H_PEER_UNMAP 0x4
#define HTT_T2H_TX_COMPL_IND 0x7
#define HTT_T2H_SEC_IND 0xb
#define HTT_T2H_MGMT_TX_COMPL_IND 0xe
#define HTT_T2H_TX_CREDIT_UPDATE_IND 0xf
#define HTT_T2H_RX_IN_ORD_PADDR_IND 0x12

/* TX_FRM's descriptor (htt_data_tx_desc_64) */
#define HTT_TX_FLAGS0_MAC_HDR_PRESENT 0x01
#define HTT_TX_FLAGS0_NO_ENCRYPT 0x04
#define HTT_TX_FLAGS0_PKT_TYPE_LSB 5     /* enum ath10k_hw_txrx_mode */
#define HTT_TX_MODE_RAW 0
#define HTT_TX_MODE_NATIVE_WIFI 1
#define HTT_TX_MODE_ETHERNET 2
#define HTT_TX_MODE_MGMT 3
#define HTT_TX_FLAGS1_TID_LSB 6
#define HTT_TX_FLAGS1_POSTPONED (1u << 11)
#define HTT_TID_NON_QOS 16               /* HTT_DATA_TX_EXT_TID_NON_QOS_MCAST_BCAST */
#define HTT_TID_MGMT 17
#define HTT_INVALID_PEER 0xffff
#define HTT_TX_DESC_LEN 23               /* flags0 1, flags1 2, len 2, id 2, frags 8, peer id 4, (prefetch) */
#define HTT_PREFETCH 46                  /* htt->prefetch_len: 802.11 + QoS + HT 36, 802.1q 4, LLC/SNAP 8, 2 */
#define HTT_EXT_DESC_LEN 72              /* htt_msdu_ext_desc_64: tso 20, ip id 2, flags 2, 6 fragments of 8 */
#define HTT_TXBUF_LEN 48                 /* ath10k_htt_txbuf_64: frags 16, HTC 8, cmd 1, descriptor 23 */

/* the RX ring (HTT_RX_RING_SIZE, HTT_RX_BUF_SIZE) and the WCN3990's RX
 * descriptor in front of every buffer's frame (htt_rx_desc_v2, 360 bytes;
 * offsets in 4-byte words as ath10k_rx_desc_wcn3990_get_offsets gives
 * them, computed from Linux's rx_desc.h) */
#define HTT_RX_RING_SIZE 2048
#define HTT_RX_BUF_SIZE 2048
#define HTT_RX_DESC_LEN 360
#define HTT_RX_OFF_ATTENTION 4          /* rx_attention.flags */
#define HTT_RX_OFF_MPDU_INFO0 16        /* rx_mpdu_start.info0 */
#define HTT_RX_OFF_MSDU_INFO1 36        /* rx_msdu_start (at 28): info0, flow_id_crc, info1 */
#define HTT_RX_OFF_MSDU_END_INFO0 64    /* rx_msdu_end (at 48): checksums 4, key id 1, filter 1, wapi_pn 10, info0 */
#define HTT_RX_OFF_HDR_STATUS 296       /* the 802.11 header as received (64 bytes) */
/* the RX_RING_CFG offsets, in words */
#define HTT_RX_W_ATTENTION 1
#define HTT_RX_W_FRAG_INFO 2
#define HTT_RX_W_MPDU_START 4
#define HTT_RX_W_MSDU_START 7
#define HTT_RX_W_MSDU_END 12
#define HTT_RX_W_MPDU_END 26
#define HTT_RX_W_PPDU_START 27
#define HTT_RX_W_PPDU_END 37
#define HTT_RX_W_HDR_STATUS 74
#define HTT_RX_W_PAYLOAD 90
#define HTT_RX_ATTN_MSDU_DONE (1u << 31)
#define HTT_RX_ATTN_FCS_ERR (1u << 30)
#define HTT_RX_ATTN_DECRYPT_ERR (1u << 29)
#define HTT_RX_ATTN_TKIP_MIC_ERR (1u << 28)
#define HTT_RX_ATTN_MGMT_TYPE (1u << 8)
#define HTT_RX_ATTN_CTRL_TYPE (1u << 9)
#define HTT_RX_MSDU_END_FIRST (1u << 14)
#define HTT_RX_MSDU_END_LAST (1u << 15)
#define HTT_RX_DECAP_SHIFT 8             /* RX_MSDU_START_INFO1_DECAP_FORMAT: bits 8-9 */
#define HTT_RX_DECAP_NATIVE_WIFI 1
#define HTT_RX_MPDU_ENCRYPTED (1u << 13)

/* message builders (the HTT payload after HTC's header): their length */
u32 htt_ver_req(u8 *o);
u32 htt_frag_bank_cfg(u8 *o, u64 bank_pa, u16 max_id);
u32 htt_rx_ring_cfg(u8 *o, u64 idx_pa, u64 ring_pa, u16 ring_len, u16 fw_idx);
u32 htt_aggr_cfg(u8 *o, u8 ampdu, u8 amsdu);
/* TX_FRM's descriptor at d (HTT_TX_DESC_LEN; the message type before
 * it is the caller's): the frame's length, its id, the fragment list's or
 * (management) the frame's address, the mode, the vdev, the TID */
void htt_tx_desc(u8 *d, u32 mode, u32 vdev, u32 tid, u16 len, u16 id, u64 frags_pa, bool no_encrypt);
/* a fragment list (htt_msdu_ext_desc_64) of one fragment */
void htt_ext_desc(u8 *e, u64 pa, u16 len);

/* RX_IN_ORD_PADDR_IND (the message after its type byte): the count, the
 * vdev, the peer; then its buffers */
struct htt_in_ord {
	u16 peer_id, count;
	u8 vdev, tid;
	bool offload, frag;
	const u8 *descs;                     /* count x 12: address 8, length 2, fw_desc, reserved */
};
int htt_parse_in_ord(const u8 *p, u32 len, struct htt_in_ord *o);
/* the n-th buffer of it */
void htt_in_ord_buf(const struct htt_in_ord *o, u32 i, u64 *pa, u16 *len);

#endif
