/* KolibriOS for ARM: the 802.11 frames the station builds and reads
 * (wlan.c; docs/pro1x-network.md, stage 5): what mac80211 does above
 * ath10k for a station, as far as needed here (IEEE 802.11-2016 9.3, 9.4):
 * beacons and probe responses read (the BSS), authentication (open
 * system), association (the rates, the RSN element), deauthentication;
 * data frames in the WCN3990's "native Wi-Fi" form (ath10k's
 * ATH10K_HW_TXRX_NATIVE_WIFI: a 3-address header without QoS control,
 * then LLC/SNAP) to and from Ethernet frames (htt_rx_h_undecap_nwifi,
 * ath10k_tx_h_nwifi). No QoS (WMM), no HT: 802.11a/b/g rates. */
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
#define WLAN_LISTEN_INTERVAL 5           /* ath10k's ATH10K_MAX_HW_LISTEN_INTERVAL */

struct wlan_bss {
	u8 bssid[6];
	u8 ssid[32], ssid_len;
	u16 freq, beacon_int, cap;
	u8 chan, dtim;
	s32 snr;
	u8 rates[16], nrates;               /* in 500 kb/s, bit 7: basic */
	u8 rsn[64], rsn_len;                /* the RSN element, whole */
	bool ht;
};

/* a beacon or probe response (the whole frame) into b; freq and snr from
 * the firmware's MGMT_RX: 0, or -1 */
int wlan_parse_bss(const u8 *f, u32 len, u16 freq, s32 snr, struct wlan_bss *b);
/* the probe request's elements the firmware adds to its own (the SSID):
 * the rates */
u32 wlan_probe_ies(u8 *o, bool band5);
/* management frames to the AP: their length */
u32 wlan_auth(u8 *o, const u8 bssid[6], const u8 own[6]);
u32 wlan_assoc_req(u8 *o, const struct wlan_bss *b, const u8 own[6], const u8 *rsn, u32 rsn_len);
/* the AP's answers (to us from bssid): 0 with the status (and the AID),
 * -1 when not the expected frame */
int wlan_auth_resp(const u8 *f, u32 len, const u8 bssid[6], const u8 own[6], u16 *status);
int wlan_assoc_resp(const u8 *f, u32 len, const u8 bssid[6], const u8 own[6], u16 *status, u16 *aid);
int wlan_deauth(const u8 *f, u32 len, const u8 bssid[6], u16 *reason);
u16 wlan_fc(const u8 *f);

/* Ethernet (dst, src, type, payload) to a native Wi-Fi data frame to the
 * AP (ToDS: addr1 the BSSID, addr2 ours, addr3 the destination), the
 * Protected bit when the keys are in: its length, or 0 */
u32 wlan_from_eth(u8 *o, u32 cap, const u8 *eth, u32 len, const u8 bssid[6], bool protect);
/* a native Wi-Fi data frame from the AP (FromDS) to Ethernet: its length,
 * or 0 (not data, not LLC/SNAP) */
u32 wlan_to_eth(u8 *o, u32 cap, const u8 *f, u32 len);

#endif
