/* KolibriOS for ARM: the Pro1X's Wi-Fi (the WCN3990 behind WLFW) as one
 * driver (wifi.c; docs/pro1x-network.md, stages 4-6): what Linux's
 * ath10k_snoc, ath10k_core and ath10k_mac do with the firmware, and what
 * mac80211 and wpa_supplicant do above them for a WPA2-PSK station:
 *
 *  4: WLFW's WLAN_CFG and WLAN_MODE, the copy engines, HTC (READY, the
 *     HTT and WMI services, SETUP_COMPLETE_EX), WMI's SERVICE_READY, INIT
 *     (with the host memory the firmware asks for), READY (the MAC
 *     address), HTT's version, its TX fragment bank and RX ring; the
 *     device's parameters, the channel list;
 *  5: a station vdev, the scan (beacons and probe responses by WMI's
 *     MGMT_RX), the BSS of the configured SSID, the vdev started on its
 *     channel, its peer, authentication and association (WMI's
 *     MGMT_TX_SEND), PEER_ASSOC and VDEV_UP, the 4-way handshake (EAPOL
 *     over the data path, wpa.c), the keys installed (VDEV_INSTALL_KEY),
 *     the peer authorized;
 *  6: HTT's data path: Ethernet frames out as native Wi-Fi in TX_FRM,
 *     in from RX_IN_ORD_PADDR_IND's buffers, to the network stack.
 *
 * Plain C over the hardware's hooks (wifi_hw), the same in the kernel
 * and in tests/wifi (MODEM_HOST, against a simulated firmware and AP).
 * Everything runs from wifi_poll (interrupts or the tick). */
#ifndef WIFI_H
#define WIFI_H
#include "modem.h"
#include "ce.h"

/* the network to join: from the device test's file (wifi_conf_parse) */
struct wifi_conf {
	u8 ssid[32], ssid_len;
	char psk[65];
	u8 bssid[6];
	bool have_bssid;
	char country[3];                    /* the regulatory domain's country: "AT" unless country= says */
	bool no_ps;                         /* powersave=off (cfg80211's default: on) */
	u8 uapsd_queues;                    /* uapsd=on: all four ACs (mac80211's default: none) */
};

struct wifi_hw {
	struct ce_hw ce;                    /* the register window, memory below 2^35 (the rings, the buffers) */
	void *(*dma_alloc32)(u32 size, u64 *pa);   /* below 4 GiB: the firmware's host memory (INIT) */
	/* WLFW: WLAN_CFG, then WLAN_MODE mission; the answer by wifi_wlan_enabled */
	void (*wlan_enable)(void);
	u32 (*now_ms)(void);
	void (*random)(void *p, u32 n);
	void (*rx)(const u8 *eth, u32 len); /* an Ethernet frame for the network stack */
	void (*link)(bool up, const u8 mac[6]);
};

enum wifi_state {
	WIFI_OFF, WIFI_ENABLE, WIFI_HTC_READY, WIFI_HTC_CONNECT, WIFI_SVC_READY, WIFI_WMI_READY, WIFI_HTT_VERSION,
	WIFI_SETUP, WIFI_SCAN, WIFI_JOIN, WIFI_PEER, WIFI_AUTH, WIFI_ASSOC, WIFI_4WAY, WIFI_KEYS, WIFI_UP, WIFI_FAILED,
};

/* "ssid=NAME" and "psk=PASSPHRASE" (or 64 hex digits) lines, "bssid=
 * aa:bb:cc:dd:ee:ff", "country=XX" (ISO 3166 alpha-2, 00: none; AT by
 * default), "powersave=on|off" (on), "uapsd=on|off" (off, as mac80211's
 * IEEE80211_DEFAULT_UAPSD_QUEUES) optional, '#' comments: 0, or -1
 * (*why) */
int wifi_conf_parse(const char *text, u32 len, struct wifi_conf *c, const char **why);

/* after WLFW said the firmware is ready (wlfw.c): the bring-up starts */
void wifi_start(const struct wifi_hw *hw, const struct wifi_conf *c);
void wifi_wlan_enabled(int err);       /* WLFW's answer to WLAN_MODE */
void wifi_poll(void);                  /* the engines, then the state's timeouts */
u32 wifi_next_ms(void);                /* ms until wifi_poll has work without an interrupt (tickless) */
/* the host's side of power save: how often to look at the engines without
 * their interrupts (10 ms on the way up; up: 20 ms while frames come, 200
 * ms after a second without, 1 s after ten: the firmware and the AP keep
 * what comes meanwhile), and whether frames came in the last second (then
 * the engines' interrupts should be able to wake the CPU: no SoC sleep) */
u32 wifi_poll_ms(void);
bool wifi_busy(void);
int wifi_send(const u8 *eth, u32 len); /* an Ethernet frame out: 0, or -1 (not up, or full) */
enum wifi_state wifi_state(void);
const char *wifi_state_name(enum wifi_state s);
const u8 *wifi_mac(void);

/* what happened, for the console and the tests */
struct wifi_stats {
	u32 wmi_events, wmi_cmds, htt_msgs, scans, bss_seen, rx_frames, tx_frames, tx_done, rx_dropped, eapol;
	u32 restarts;
	u8 bssid[6];
	u16 freq, aid;
	u8 ht;                              /* the peer: 0 legacy, 20 or 40 MHz HT */
	bool qos, ps;                       /* QoS data, the firmware's power save */
};
const struct wifi_stats *wifi_stats(void);

#endif
