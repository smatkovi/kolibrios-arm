/* KolibriOS for ARM: the 802.11 frames the station builds and reads
 * (wlan.c; docs/pro1x-network.md, stage 5): what mac80211 does above
 * ath10k for a station, as far as needed here (IEEE 802.11-2016 9.3, 9.4):
 * beacons and probe responses read (the BSS: rates, HT Capabilities and
 * Operation, the WMM element, RSN and RSNX), authentication (open system,
 * SAE's frames are sae.c's), association (mac80211's
 * ieee80211_send_assoc: the rates both have, the RSN element, HT
 * Capabilities, the WMM information element), deauthentication; data
 * frames in the WCN3990's "native Wi-Fi" form (ath10k's
 * ATH10K_HW_TXRX_NATIVE_WIFI: a 3-address header without QoS control,
 * then LLC/SNAP; the TID goes in HTT's descriptor) to and from Ethernet
 * frames (htt_rx_h_undecap_nwifi, ath10k_tx_h_nwifi). */
#ifndef WLAN_H
#define WLAN_H
#include "modem.h"

#define WLAN_HDR 24
#define WLAN_FC_BEACON 0x0080
#define WLAN_FC_PROBE_RESP 0x0050
#define WLAN_FC_AUTH 0x00b0
#define WLAN_FC_ASSOC_REQ 0x0000
#define WLAN_FC_ASSOC_RESP 0x0010
#define WLAN_FC_DEAUTH 0x00c0
#define WLAN_FC_DISASSOC 0x00a0
#define WLAN_FC_DATA 0x0008
#define WLAN_FC_TODS 0x0100
#define WLAN_FC_FROMDS 0x0200
#define WLAN_FC_PROTECTED 0x4000
#define WLAN_CAP_ESS 0x0001
#define WLAN_CAP_PRIVACY 0x0010
#define WLAN_CAP_SHORT_PREAMBLE 0x0020
#define WLAN_CAP_SHORT_SLOT 0x0400
#define WLAN_CAP_SPECTRUM_MGMT 0x0100
#define WLAN_FC_QOS 0x0080               /* the data subtypes' QoS bit */
#define WLAN_LISTEN_INTERVAL 5           /* ath10k's ATH10K_MAX_HW_LISTEN_INTERVAL */

/* HT (include/linux/ieee80211-ht.h) */
#define HT_CAP_LDPC 0x0001
#define HT_CAP_SUP_WIDTH_20_40 0x0002
#define HT_CAP_SM_PS 0x000c
#define HT_CAP_SM_PS_SHIFT 2
#define HT_CAP_GRN_FLD 0x0010
#define HT_CAP_SGI_20 0x0020
#define HT_CAP_SGI_40 0x0040
#define HT_CAP_TX_STBC 0x0080
#define HT_CAP_RX_STBC 0x0300
#define HT_CAP_RX_STBC_SHIFT 8
#define HT_CAP_MAX_AMSDU 0x0800
#define HT_CAP_DSSSCCK40 0x1000
#define HT_CAP_LSIG_TXOP_PROT 0x8000
#define HT_SM_PS_STATIC 0
#define HT_SM_PS_DYNAMIC 1
#define HT_SM_PS_DISABLED 3
#define HT_MCS_TX_DEFINED 0x01
#define HT_MCS_TX_RX_DIFF 0x02
#define HT_MCS_TX_UNEQUAL 0x10
#define HT_PARAM_SEC_OFFSET 0x03
#define HT_PARAM_SEC_ABOVE 1
#define HT_PARAM_SEC_BELOW 3
#define HT_AMPDU_64K 3                   /* IEEE80211_HT_MAX_AMPDU_64K */
#define HT_MPDU_DENSITY_8 6              /* 8 us */
/* WMM (the vendor element 00-50-f2, type 2) */
#define WMM_QOSINFO_AP_UAPSD 0x80
#define WMM_QOSINFO_STA_AC_VO 0x01
#define WMM_QOSINFO_STA_AC_VI 0x02
#define WMM_QOSINFO_STA_AC_BK 0x04
#define WMM_QOSINFO_STA_AC_BE 0x08

/* the HT capabilities as the element has them (struct ieee80211_ht_cap's
 * first 19 bytes): the info, the A-MPDU parameters, the MCS set (the RX
 * mask in 10 bytes, the highest rate, the TX parameters) */
struct wlan_ht {
	u16 cap;
	u8 ampdu;
	u8 mcs[16];
};

struct wlan_bss {
	u8 bssid[6];
	u8 ssid[32], ssid_len;
	u16 freq, beacon_int, cap;
	u8 chan, dtim;
	s32 snr;
	u8 rates[16], nrates;               /* in 500 kb/s, bit 7: basic */
	u8 rsn[64], rsn_len;                /* the RSN element, whole */
	u8 rsnx[16], rsnx_len;              /* the RSN Extension element, whole (SAE's hash-to-element) */
	bool ht;                            /* HT Capabilities there */
	struct wlan_ht ht_cap;
	bool ht_op;                         /* HT Operation there */
	u8 ht_primary, ht_param;
	bool wmm;                           /* a WMM information or parameter element (mac80211's wmm_used) */
	bool uapsd;                         /* its QoS info: the AP does U-APSD (is_uapsd_supported) */
	u8 wmm_param[24], wmm_len;          /* the parameter element from the OUI (elems->wmm_param), 24 or 0 */
	bool h2e_only;                      /* the BSS membership selector 123: SAE hash-to-element only */
};

/* a beacon or probe response (the whole frame) into b; freq and snr from
 * the firmware's MGMT_RX: 0, or -1 */
int wlan_parse_bss(const u8 *f, u32 len, u16 freq, s32 snr, struct wlan_bss *b);
/* the probe request's elements the firmware adds to its own (the SSID):
 * the rates */
u32 wlan_probe_ies(u8 *o, bool band5);
/* management frames to the AP: their length */
u32 wlan_auth(u8 *o, const u8 bssid[6], const u8 own[6]);
/* an authentication frame of another algorithm (SAE: 3): the header,
 * algorithm, transaction, status, then len bytes of body */
u32 wlan_auth_alg(u8 *o, const u8 bssid[6], const u8 own[6], u16 alg, u16 seq, u16 status, const u8 *body, u32 len);
/* the association request as mac80211's: our capabilities (the BSS's
 * privacy, 2.4 GHz short slot and preamble, spectrum management with the
 * power capability (max_power dBm) and the supported channels), the rates
 * both have, the RSN element (and RSNX), HT Capabilities (ht: ours, NULL
 * without HT), the WMM information element (wmm_qos: its QoS info, -1
 * without WMM) */
struct wlan_assoc {
	const u8 *rsn, *rsnx;
	u32 rsn_len, rsnx_len;
	const struct wlan_ht *ht;
	int wmm_qos;
	s8 max_power;
	const u8 *chans;                    /* the band's channels (spectrum management) */
	u32 nchans;
};
u32 wlan_assoc_req(u8 *o, const struct wlan_bss *b, const u8 own[6], const struct wlan_assoc *a);
/* the association response's elements (after status and AID): the WMM
 * parameter element (24 bytes from the OUI) into wmm (*wmm_len 0 when
 * none), the HT Capabilities and Operation */
int wlan_assoc_resp_ies(const u8 *f, u32 len, u8 wmm[24], u8 *wmm_len, struct wlan_ht *ht, bool *have_ht);
/* the AP's answers (to us from bssid): 0 with the status (and the AID),
 * -1 when not the expected frame */
int wlan_auth_resp(const u8 *f, u32 len, const u8 bssid[6], const u8 own[6], u16 *status);
int wlan_assoc_resp(const u8 *f, u32 len, const u8 bssid[6], const u8 own[6], u16 *status, u16 *aid);
int wlan_deauth(const u8 *f, u32 len, const u8 bssid[6], u16 *reason);
u16 wlan_fc(const u8 *f);
#define WLAN_FC_ACTION 0x00d0
/* management frame protection (IEEE 802.11-2020 12.6.11): an SA Query
 * request or response (action category 8, 0 or 1, the transaction id) to
 * the AP, protected: the CCMP header with this PN and key 0, room for the
 * MIC the firmware puts there (ath10k's WMI management path adds it): its
 * length */
u32 wlan_sa_query(u8 *o, const u8 bssid[6], const u8 own[6], u8 action, const u8 id[2], u64 pn);
/* a protected unicast management frame from the AP as the firmware hands
 * it over (decrypted, the CCMP header and the MIC still there: ath10k's
 * wmi.c on PMF): the PN, the body between: 0, or -1 */
int wlan_ccmp_mgmt(const u8 *f, u32 len, u64 *pn, const u8 **body, u32 *blen);

/* Ethernet (dst, src, type, payload) to a native Wi-Fi data frame to the
 * AP (ToDS: addr1 the BSSID, addr2 ours, addr3 the destination), the
 * Protected bit when the keys are in: its length, or 0 */
u32 wlan_from_eth(u8 *o, u32 cap, const u8 *eth, u32 len, const u8 bssid[6], bool protect);
/* a native Wi-Fi data frame from the AP (FromDS) to Ethernet: its length,
 * or 0 (not data, not LLC/SNAP); a QoS header as ath10k_htt_rx_nwifi_hdrlen
 * counts it (rounded up to 4) */
u32 wlan_to_eth(u8 *o, u32 cap, const u8 *f, u32 len);

/* the 802.1d priority of an Ethernet frame (cfg80211_classify8021d without
 * a QoS map: the DSCP's top three bits, RFC 8325's exceptions; 802.21 7;
 * EAPOL is the caller's: 7) */
u8 wlan_classify(const u8 *eth, u32 len);

#endif
