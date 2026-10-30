/* KolibriOS for ARM: the WCN3990 as a WPA2-PSK station (wifi.h), after
 * Linux's ath10k (snoc.c: ath10k_snoc_hif_power_up, _hif_start,
 * _rx_post, _process_rx_cb, _hif_tx_sg; core.c: ath10k_core_start; htc.c;
 * htt.c: ath10k_htt_setup; htt_tx.c: ath10k_htt_tx_64; htt_rx.c:
 * ath10k_htt_rx_ring_fill_n, ath10k_htt_rx_in_ord_ind,
 * ath10k_htt_t2h_msg_handler; wmi.c: ath10k_wmi_event_service_ready_work;
 * mac.c: ath10k_start, ath10k_add_interface, ath10k_hw_scan,
 * ath10k_vdev_start_restart, ath10k_sta_state, ath10k_bss_assoc,
 * ath10k_set_key) and mac80211's station MLME (auth, assoc) for one
 * network, with wpa.c for the 4-way handshake. */
#include "wifi.h"
#include "htc.h"
#include "wmi.h"
#include "htt.h"
#include "wlan.h"
#include "wpa.h"
#include "crypto.h"

#define VDEV 0
#define SCAN_ID 0                       /* ATH10K_SCAN_ID */

/* --- DMA buffers: 2 KiB slots in one block -------------------------------- */

#define SLOT 2048
#define NSLOTS 512
#define RX_POST_HTT 32                   /* buffers on CE1 (HTT, HTC); Linux fills every ring entry */
#define RX_POST_WMI 16                   /* CE2 (WMI, HTC control) */
#define RX_POST_OTHER 4                  /* CE5, CE8, CE9-11 */
#define HTT_RX_FILL 256                  /* RX ring buffers (Linux: 2047 for the WCN3990) */
#define MAX_TX 64                        /* pending TX_FRM ids (Linux: 1056) */
#define MAX_MGMT 8                       /* pending management frames */
#define WMI_QUEUE 32

static struct wifi_hw hw;
static struct wifi_conf conf;
static enum wifi_state st = WIFI_OFF;
static u32 st_since, st_timeout, retry_at;
static struct wifi_stats stats;
static u8 mac[6];

static u8 *pool;
static u64 pool_pa;
static u16 free_slots[NSLOTS];
static u32 nfree;

static void *slot_alloc(u64 *pa)
{
	if (!nfree) return NULL;
	u32 i = free_slots[--nfree];
	*pa = pool_pa + (u64)i * SLOT;
	return pool + (u64)i * SLOT;
}

static void slot_free(void *p)
{
	if (!p) return;
	u32 i = (u32)(((u8 *)p - pool) / SLOT);
	if (i < NSLOTS && nfree < NSLOTS) free_slots[nfree++] = (u16)i;
}

static u8 *slot_by_pa(u64 pa)
{
	if (pa < pool_pa || pa >= pool_pa + (u64)NSLOTS * SLOT || (pa - pool_pa) % SLOT) return NULL;
	return pool + (pa - pool_pa);
}

static u64 slot_pa(const void *p)
{
	return pool_pa + (u64)((const u8 *)p - pool);
}

/* --- state ------------------------------------------------------------------ */

static const char *const names[] = {
	"off", "enable", "HTC ready", "HTC connect", "service ready", "WMI ready", "HTT version", "setup", "scan",
	"join", "peer", "auth", "assoc", "4-way", "keys", "up", "failed",
};

const char *wifi_state_name(enum wifi_state s) { return s <= WIFI_FAILED ? names[s] : "?"; }
enum wifi_state wifi_state(void) { return st; }
const u8 *wifi_mac(void) { return mac; }
const struct wifi_stats *wifi_stats(void) { return &stats; }

static void go(enum wifi_state s, u32 timeout_ms)
{
	st = s;
	st_since = hw.now_ms();
	st_timeout = timeout_ms;
}

static void fail(const char *why)
{
	mlog("wifi: %s (in %s): stopped\n", why, wifi_state_name(st));
	go(WIFI_FAILED, 0);
}

/* --- HTC over the copy engines --------------------------------------------- */

static struct htc htc;
static int wmi_eid = -1, htt_eid = -1;
static u8 *wmi_q[WMI_QUEUE];
static u16 wmi_qlen[WMI_QUEUE];
static u32 wmi_qh, wmi_qn;

/* a message on endpoint eid: header and payload in a slot (the payload
 * at HTC_HDR_LEN), sent on the endpoint's pipe, the slot freed when the
 * engine is done */
static int htc_out(u8 eid, u8 *b, u16 len)
{
	int n = htc_prepare(&htc, eid, b, len);
	if (n < 0) return -1;
	if (ce_send(htc.ep[eid].ul_pipe, slot_pa(b), (u32)n, eid, b)) {
		htc.ep[eid].credits += htc.ep[eid].credit_flow ? 1 : 0;
		return -1;
	}
	return 0;
}

static u8 *msg_new(u64 *pa)
{
	u8 *b = slot_alloc(pa);
	if (b) mzero(b, 64);
	return b;
}

/* WMI commands wait for a credit (the WCN3990's one: use_fw_tx_credits
 * false); they go in order */
static void wmi_flush(void)
{
	while (wmi_qn && wmi_eid >= 0 && htc.ep[wmi_eid].credits > 0) {
		u8 *b = wmi_q[wmi_qh];
		if (htc_out((u8)wmi_eid, b, wmi_qlen[wmi_qh])) break;
		stats.wmi_cmds++;
		wmi_qh = (wmi_qh + 1) % WMI_QUEUE;
		wmi_qn--;
	}
}

/* a command built at HTC_HDR_LEN of a slot */
static void wmi_cmd(u8 *b, u32 len)
{
	if (!b) return;
	if (wmi_qn == WMI_QUEUE) {
		mlog("wifi: WMI queue full, a command dropped\n");
		slot_free(b);
		return;
	}
	u32 t = (wmi_qh + wmi_qn) % WMI_QUEUE;
	wmi_q[t] = b;
	wmi_qlen[t] = (u16)len;
	wmi_qn++;
	wmi_flush();
}

#define WMI_CMD(call) do { u64 pa_; u8 *b_ = msg_new(&pa_); if (b_) wmi_cmd(b_, call(b_ + HTC_HDR_LEN)); } while (0)

static void htt_out(u32 (*build)(u8 *o))
{
	u64 pa;
	u8 *b = msg_new(&pa);
	if (!b || htc_out((u8)htt_eid, b, (u16)build(b + HTC_HDR_LEN))) slot_free(b);
}

/* --- the RX buffers on the engines ------------------------------------------ */

static const u8 rx_post_n[CE_COUNT] = { 0, RX_POST_HTT, RX_POST_WMI, 0, 0, RX_POST_OTHER, 0, 0,
					RX_POST_OTHER, RX_POST_OTHER, RX_POST_OTHER, RX_POST_OTHER };

static void rx_post(u32 ce)
{
	while (ce_dst_free(ce) && ce_dst_free(ce) > (u32)(ce_attr_wlan[ce].dst_n - rx_post_n[ce])) {
		u64 pa;
		u8 *b = slot_alloc(&pa);
		if (!b) return;
		if (ce_rx_post(ce, pa, b)) {
			slot_free(b);
			return;
		}
	}
}

/* --- HTT: the RX ring, TX frames -------------------------------------------- */

static u64 *rx_ring;
static u64 rx_ring_pa, rx_idx_pa;
static volatile u32 *rx_idx;
static u32 rx_fill;
static u8 *txbuf, *txext;
static u64 txbuf_pa, txext_pa;
static u8 *tx_frame[MAX_TX];
static u8 tx_kind[MAX_TX];              /* 1 data, 2 EAPOL message 4 */
static u8 htt_major, htt_minor;
static u16 peer_id = 0xffff;

static void rx_ring_fill(void)
{
	u32 idx = *rx_idx & (HTT_RX_RING_SIZE - 1);
	while (rx_fill < HTT_RX_FILL) {
		u64 pa;
		u8 *b = slot_alloc(&pa);
		if (!b) break;
		/* the attention word cleared (ath10k_htt_rx_ring_fill_n) */
		put32(b + HTT_RX_OFF_ATTENTION, 0);
		rx_ring[idx] = pa;
		idx = (idx + 1) & (HTT_RX_RING_SIZE - 1);
		rx_fill++;
	}
	sm_barrier();
	*rx_idx = idx;
}

static int tx_id_alloc(void)
{
	for (int i = 0; i < MAX_TX; i++)
		if (!tx_frame[i]) return i;
	return -1;
}

/* TX_FRM: the descriptor (with HTC's header in front, in txbuf) and the
 * frame's first bytes, gathered on CE4 (transfer id 0, no context: done
 * by TX_COMPL_IND, as Linux) */
static int htt_tx(u8 *frame, u32 len, u32 mode, u32 tid, u8 kind)
{
	int id = tx_id_alloc();
	if (id < 0 || htt_eid < 0) return -1;
	u64 fpa = slot_pa(frame), frags = fpa;
	if (mode != HTT_TX_MODE_MGMT) {
		htt_ext_desc(txext + HTT_EXT_DESC_LEN * id, fpa, (u16)len);
		frags = txext_pa + (u64)HTT_EXT_DESC_LEN * id;
	}
	u32 prefetch = (MIN(len, (u32)HTT_PREFETCH) + 3) & ~3u;
	u8 *t = txbuf + HTT_TXBUF_LEN * id + 16;            /* after frags[2] */
	htc_put_hdr(t, (u8)htt_eid, (u16)(1 + HTT_TX_DESC_LEN + prefetch), 0, 0);
	t[HTC_HDR_LEN] = HTT_H2T_TX_FRM;
	htt_tx_desc(t + HTC_HDR_LEN + 1, mode, VDEV, tid, (u16)len, (u16)id, frags, false);
	u32 pipe = htc.ep[htt_eid].ul_pipe;
	if (ce_send_sg(pipe, txbuf_pa + (u64)HTT_TXBUF_LEN * id + 16, HTC_HDR_LEN + 1 + HTT_TX_DESC_LEN, 0, true, NULL))
		return -1;
	if (ce_send_sg(pipe, fpa, prefetch, 0, false, NULL)) {
		ce_send_revert(pipe);
		return -1;
	}
	tx_frame[id] = frame;
	tx_kind[id] = kind;
	stats.tx_frames++;
	return id;
}

/* --- management frames -------------------------------------------------------- */

static bool mgmt_wmi;                   /* WMI_SERVICE_MGMT_TX_WMI: MGMT_TX_SEND, else HTT */
static u8 *mgmt_frame[MAX_MGMT];

static int mgmt_tx(u8 *f, u32 len)
{
	if (mgmt_wmi) {
		int id = -1;
		for (int i = 0; i < MAX_MGMT && id < 0; i++)
			if (!mgmt_frame[i]) id = i;
		u64 pa;
		u8 *b = id >= 0 ? msg_new(&pa) : NULL;
		if (!b) return -1;
		mgmt_frame[id] = f;
		wmi_cmd(b, wmi_mgmt_tx(b + HTC_HDR_LEN, VDEV, (u32)id, slot_pa(f), f, len));
		return 0;
	}
	return htt_tx(f, len, HTT_TX_MODE_MGMT, HTT_TID_MGMT, 1) < 0 ? -1 : 0;
}

static void mgmt_done(u32 id, u32 status, void *arg)
{
	(void)status;
	(void)arg;
	if (id < MAX_MGMT && mgmt_frame[id]) {
		slot_free(mgmt_frame[id]);
		mgmt_frame[id] = NULL;
	}
}

/* --- the network --------------------------------------------------------------- */

static struct wlan_bss bss, best;
static bool have_best;
static u8 pmk[WPA_PMK_LEN];
static struct wpa_sm wpa;
static u8 own_ie[WPA_MAX_IE];
static u32 own_ie_len;
static bool keys_in;
static u32 tries;
static u8 eapol_pending[512];           /* an EAPOL frame before the association's answer */
static u32 eapol_pending_len;
static void eapol_in(const u8 *p, u32 len);

/* the channels: 802.11b/g 1-13 and 5 GHz 36-64, 100-140 at EU limits (the
 * regulatory database's "EU": 20 dBm in 2.4 GHz, 23 dBm 5150-5350 MHz, 30
 * dBm 5470-5725, DFS from 5250), passive where DFS */
static struct wmi_chan chans[13 + 8 + 11];
static u16 freqs[sizeof(chans) / sizeof(chans[0])];
static u32 nchan;

static void chans_init(void)
{
	nchan = 0;
	for (u32 c = 1; c <= 13; c++)
		chans[nchan++] = (struct wmi_chan){ (u16)(2407 + 5 * c), MODE_11G, false, false, 40, 40 };
	for (u32 c = 36; c <= 64; c += 4)
		chans[nchan++] = (struct wmi_chan){ (u16)(5000 + 5 * c), MODE_11A, false, c >= 52, 46, 46 };
	for (u32 c = 100; c <= 140; c += 4)
		chans[nchan++] = (struct wmi_chan){ (u16)(5000 + 5 * c), MODE_11A, false, true, 60, 60 };
	for (u32 i = 0; i < nchan; i++) freqs[i] = chans[i].freq;
}

static u16 chan_freq(u32 ch)
{
	if (ch >= 1 && ch <= 13) return (u16)(2407 + 5 * ch);
	if (ch == 14) return 2484;
	if (ch >= 36 && ch <= 177) return (u16)(5000 + 5 * ch);
	return 0;
}

static void print_mac(const char *what, const u8 *m)
{
	mlog("%s%02x:%02x:%02x:%02x:%02x:%02x", what, m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void scan_start(void)
{
	have_best = false;
	stats.scans++;
	u64 pa;
	u8 *b = msg_new(&pa);
	if (!b) return;
	u8 ie[32];
	u32 il = wlan_probe_ies(ie, false);
	wmi_cmd(b, wmi_start_scan(b + HTC_HDR_LEN, VDEV, SCAN_ID, freqs, nchan, conf.ssid, conf.ssid_len, ie, il));
	mlog("wifi: scanning %u channels for \"%s\"\n", nchan, (const char *)conf.ssid);
	go(WIFI_SCAN, 25000);
}

static void join(void)
{
	bss = best;
	mcopy(stats.bssid, bss.bssid, 6);
	stats.freq = bss.freq;
	print_mac("wifi: joining ", bss.bssid);
	mlog(" on %u MHz (channel %u, SNR %d, beacon %u TU, DTIM %u)\n", bss.freq, bss.chan, bss.snr, bss.beacon_int,
	     bss.dtim);
	struct wmi_chan ch = { bss.freq, bss.freq < 5000 ? MODE_11G : MODE_11A, false, false, 40, 40 };
	for (u32 i = 0; i < nchan; i++)
		if (chans[i].freq == bss.freq) ch = chans[i];
	/* the BSS's mode: 11b when it has no OFDM rate (ath10k_peer_assoc_h_phymode) */
	if (bss.freq < 5000) {
		bool ofdm = false;
		for (u32 i = 0; i < bss.nrates; i++) {
			u8 r = bss.rates[i] & 0x7f;
			if (r != 2 && r != 4 && r != 11 && r != 22) ofdm = true;
		}
		ch.mode = ofdm ? MODE_11G : MODE_11B;
	}
	u64 pa;
	u8 *b = msg_new(&pa);
	if (b) wmi_cmd(b, wmi_vdev_start(b + HTC_HDR_LEN, VDEV, bss.beacon_int, bss.dtim, &ch));
	go(WIFI_JOIN, 3000);                    /* ath10k_vdev_setup_sync: 3 s (ATH10K_VDEV_SETUP_TIMEOUT_HZ) */
}

static void send_auth(void)
{
	eapol_pending_len = 0;
	u64 pa;
	u8 *f = slot_alloc(&pa);
	if (!f || mgmt_tx(f, wlan_auth(f, bss.bssid, mac))) {
		slot_free(f);
		fail("no room for the authentication");
		return;
	}
	go(WIFI_AUTH, 1000);                    /* mac80211's IEEE80211_AUTH_TIMEOUT: 200 ms, 3 tries */
}

static void send_assoc(void)
{
	u64 pa;
	u8 *f = slot_alloc(&pa);
	if (!f) return;
	u32 n = wlan_assoc_req(f, &bss, mac, own_ie, own_ie_len);
	if (mgmt_tx(f, n)) {
		slot_free(f);
		fail("no room for the association");
		return;
	}
	go(WIFI_ASSOC, 1000);
}

/* the rates for PEER_ASSOC: ath10k_mac_bitrate_to_rate's form (500 kb/s, CCK
 * with bit 7) from the BSS's 500 kb/s ones */
static u32 peer_rates(u8 *o)
{
	u32 n = 0;
	for (u32 i = 0; i < bss.nrates; i++) {
		u8 r = bss.rates[i] & 0x7f;
		bool cck = r == 2 || r == 4 || r == 11 || r == 22;
		o[n++] = (u8)(r | (cck ? 0x80 : 0));
	}
	return n;
}

static void associated(u16 aid, u16 cap)
{
	stats.aid = aid;
	mlog("wifi: associated, AID %u\n", aid);
	u8 rates[16];
	u32 nr = peer_rates(rates);
	u32 phymode = bss.freq >= 5000 ? MODE_11A : MODE_11G;
	if (bss.freq < 5000) {
		bool ofdm = false;
		for (u32 i = 0; i < nr; i++)
			if (!(rates[i] & 0x80)) ofdm = true;
		if (!ofdm) phymode = MODE_11B;
	}
	u32 flags = WMI_TLV_PEER_AUTH | (bss.rsn_len ? WMI_TLV_PEER_NEED_PTK_4_WAY : 0);
	u64 pa;
	u8 *b = msg_new(&pa);
	if (b) wmi_cmd(b, wmi_peer_assoc(b + HTC_HDR_LEN, VDEV, bss.bssid, aid, flags, cap, 1, phymode, rates, nr));
	b = msg_new(&pa);
	if (b) wmi_cmd(b, wmi_vdev_up(b + HTC_HDR_LEN, VDEV, aid, bss.bssid));
	wpa_init(&wpa, pmk, mac, bss.bssid, own_ie, own_ie_len, bss.rsn, bss.rsn_len, hw.random);
	go(WIFI_4WAY, 10000);                   /* the AP sends message 1 right away */
	if (eapol_pending_len) {
		u32 n = eapol_pending_len;
		eapol_pending_len = 0;
		eapol_in(eapol_pending, n);
	}
}

/* an EAPOL frame out (Ethernet: to the AP, ours, 0x888e) */
static int eapol_out(const u8 *p, u32 len, u8 kind)
{
	u64 pa;
	u8 *f = slot_alloc(&pa);
	if (!f) return -1;
	u8 eth[14 + 256];
	if (len > 256) {
		slot_free(f);
		return -1;
	}
	mcopy(eth, bss.bssid, 6);
	mcopy(eth + 6, mac, 6);
	eth[12] = ETH_P_EAPOL >> 8, eth[13] = ETH_P_EAPOL & 0xff;
	mcopy(eth + 14, p, len);
	u32 n = wlan_from_eth(f, SLOT, eth, 14 + len, bss.bssid, keys_in);
	if (!n || htt_tx(f, n, HTT_TX_MODE_NATIVE_WIFI, HTT_TID_NON_QOS, kind) < 0) {
		slot_free(f);
		return -1;
	}
	return 0;
}

static int install_step;                /* 0 PTK next, 1 GTK next, 2 authorize */

static void install_next(void)
{
	u64 pa;
	u8 *b;
	if (install_step == 0) {
		b = msg_new(&pa);
		if (b) wmi_cmd(b, wmi_install_key(b + HTC_HDR_LEN, VDEV, bss.bssid, 0, WMI_KEY_PAIRWISE, WMI_TLV_CIPHER_AES_CCM,
						  wpa_tk(&wpa), WPA_TK_LEN));
		mlog("wifi: the pairwise key installed\n");
	} else if (install_step == 1 && wpa.gtk_idx < 0) {
		install_step = 2;
		install_next();
		return;
	} else if (install_step == 1) {
		b = msg_new(&pa);
		if (b) wmi_cmd(b, wmi_install_key(b + HTC_HDR_LEN, VDEV, bss.bssid, (u32)wpa.gtk_idx, WMI_KEY_GROUP,
						  WMI_TLV_CIPHER_AES_CCM, wpa.gtk, wpa.gtk_len));
		mlog("wifi: the group key %d installed\n", wpa.gtk_idx);
	} else {
		b = msg_new(&pa);
		if (b) wmi_cmd(b, wmi_peer_set_param(b + HTC_HDR_LEN, VDEV, bss.bssid, WMI_TLV_PEER_AUTHORIZE, 1));
		keys_in = true;
		tries = 0;
		mlog("wifi: up: \"%s\" ", (const char *)conf.ssid);
		print_mac("(", bss.bssid);
		mlog("), WPA2-PSK CCMP\n");
		go(WIFI_UP, 0);
		if (hw.link) hw.link(true, mac);
	}
	install_step++;
}

static void eapol_in(const u8 *p, u32 len)
{
	stats.eapol++;
	u8 out[WPA_EAPOL_HDR + WPA_MAX_IE];
	u32 n;
	int r = wpa_rx(&wpa, p, len, out, sizeof(out), &n);
	if (r < 0) {
		mlog("wifi: EAPOL dropped: %s\n", wpa.why);
		return;
	}
	if (n && eapol_out(out, n, (r & WPA_DO_PTK) ? 2 : 1)) mlog("wifi: EAPOL reply not sent\n");
	if (r & WPA_DO_PTK) {
		/* the keys after message 4 is out (its TX completion): the
		 * firmware would encrypt it otherwise */
		mlog("wifi: 4-way handshake: message 4 out\n");
		install_step = 0;
		go(WIFI_KEYS, 3000);
	} else if (r & WPA_DO_GTK) {
		/* a new group key (rekeying) */
		u64 pa;
		u8 *b = msg_new(&pa);
		if (b) wmi_cmd(b, wmi_install_key(b + HTC_HDR_LEN, VDEV, bss.bssid, (u32)wpa.gtk_idx, WMI_KEY_GROUP,
						  WMI_TLV_CIPHER_AES_CCM, wpa.gtk, wpa.gtk_len));
		mlog("wifi: a new group key %d\n", wpa.gtk_idx);
	} else if (n) {
		mlog("wifi: 4-way handshake: message 2 out\n");
	}
}

/* lost: back to scanning after the vdev is down (mac80211's connection loss) */
static void lost(const char *why)
{
	mlog("wifi: %s: disconnected\n", why);
	if (st == WIFI_UP && hw.link) hw.link(false, mac);
	keys_in = false;
	/* ath10k_bss_disassoc (vdev down), ath10k_sta_state (the peer),
	 * ath10k_vdev_stop */
	u64 pa;
	u8 *b = msg_new(&pa);
	if (b) wmi_cmd(b, wmi_vdev_down(b + HTC_HDR_LEN, VDEV));
	b = msg_new(&pa);
	if (b) wmi_cmd(b, wmi_peer_delete(b + HTC_HDR_LEN, VDEV, bss.bssid));
	b = msg_new(&pa);
	if (b) wmi_cmd(b, wmi_vdev_stop(b + HTC_HDR_LEN, VDEV));
	stats.restarts++;
	retry_at = hw.now_ms() + 2000;
	go(WIFI_SETUP, 0);
}

/* --- WMI events --------------------------------------------------------------- */

static struct wmi_svc_ready svc;

static void service_ready(const u8 *p, u32 len)
{
	int r = wmi_service_ready(p, len, &svc);
	if (r) {
		fail(r == -2 ? "SERVICE_READY: another WMI ABI" : "SERVICE_READY not understood");
		return;
	}
	mgmt_wmi = wmi_service(&svc, WMI_TLV_SERVICE_MGMT_TX_WMI);
	mlog("wifi: SERVICE_READY: firmware build %x, %u RF chains, %u memory requests, 2.4 GHz %u-%u, 5 GHz %u-%u "
	     "MHz, regulatory domain %x; RX full reorder %s, management frames by %s\n", svc.fw_build, svc.num_rf_chains,
	     svc.nreqs, svc.low_2ghz_chan, svc.high_2ghz_chan, svc.low_5ghz_chan, svc.high_5ghz_chan, svc.eeprom_rd,
	     wmi_service(&svc, WMI_TLV_SERVICE_RX_FULL_REORDER) ? "yes" : "no", mgmt_wmi ? "WMI" : "HTT");
	if (!wmi_service(&svc, WMI_TLV_SERVICE_RX_FULL_REORDER)) {
		fail("no RX full reorder (RX_IN_ORD_PADDR_IND): the ring-order RX path is not done");
		return;
	}
	/* the host memory it wants, below 4 GiB (no
	 * WMI_SERVICE_SUPPORT_EXTEND_ADDRESS taken): sized as
	 * ath10k_wmi_event_service_ready_work does for TLV (33 peers, 4 vdevs) */
	struct wmi_mem_chunk ch[WMI_MAX_MEM_REQS];
	for (int i = 0; i < svc.nreqs; i++) {
		const struct wmi_mem_req *q = &svc.req[i];
		u32 units = q->num_units;
		if (q->num_unit_info & 6) units = 33 + 1;          /* NUM_UNITS_IS_NUM_(ACTIVE_)PEERS */
		else if (q->num_unit_info & 1) units = 4 + 1;      /* NUM_UNITS_IS_NUM_VDEVS */
		u32 size = units * ((q->unit_size + 3) & ~3u);
		void *v = hw.dma_alloc32 ? hw.dma_alloc32(size, &ch[i].pa) : NULL;
		if (!v || ch[i].pa + size > 0x100000000ull) {
			fail("no host memory below 4 GiB for the firmware");
			return;
		}
		ch[i].req_id = q->req_id;
		ch[i].size = size;
		mlog("wifi: host memory %u: %u KiB at %x%08x\n", q->req_id, size >> 10, (u32)(ch[i].pa >> 32), (u32)ch[i].pa);
	}
	struct wmi_res_cfg cfg;
	wmi_init_defaults(&cfg, true, wmi_service(&svc, WMI_TLV_SERVICE_WOW) ? 22 : 0, false);
	u64 pa;
	u8 *b = msg_new(&pa);
	if (!b) return;
	int n = wmi_init_cmd(b + HTC_HDR_LEN, SLOT - HTC_HDR_LEN, &cfg, ch, svc.nreqs);
	if (n < 0) {
		slot_free(b);
		fail("INIT does not fit");
		return;
	}
	wmi_cmd(b, (u32)n);
	go(WIFI_WMI_READY, 5000);
}

static void htt_setup(void);

static void wmi_event(const u8 *p, u32 len)
{
	if (len < WMI_HDR_LEN) return;
	stats.wmi_events++;
	u32 id = wmi_id(p);
	p += WMI_HDR_LEN;
	len -= WMI_HDR_LEN;
	switch (id) {
	case WMI_TLV_SERVICE_READY_EVENTID:
		if (st == WIFI_SVC_READY) service_ready(p, len);
		return;
	case WMI_TLV_READY_EVENTID: {
		struct wmi_ready r;
		if (st != WIFI_WMI_READY) return;
		if (wmi_ready(p, len, &r) || r.status) {
			fail("READY not understood or not ready");
			return;
		}
		mcopy(mac, r.mac, 6);
		print_mac("wifi: READY: the MAC address ", mac);
		mlog(", ABI %u\n", r.abi_version);
		htt_setup();
		return;
	}
	case WMI_TLV_SCAN_EVENTID: {
		struct wmi_scan_ev e;
		if (wmi_scan_event(p, len, &e) || st != WIFI_SCAN) return;
		if (e.type == WMI_SCAN_EVENT_START_FAILED) {
			mlog("wifi: the scan did not start (reason %u)\n", e.reason);
			retry_at = hw.now_ms() + 5000;
			go(WIFI_SETUP, 0);
		} else if (e.type == WMI_SCAN_EVENT_COMPLETED) {
			if (have_best) {
				join();
			} else {
				mlog("wifi: \"%s\" not found (%u networks seen)\n", (const char *)conf.ssid, stats.bss_seen);
				retry_at = hw.now_ms() + (tries++ < 5 ? 5000 : 30000);
				go(WIFI_SETUP, 0);
			}
		}
		return;
	}
	case WMI_TLV_MGMT_RX_EVENTID: {
		struct wmi_mgmt_rx e;
		if (wmi_mgmt_rx(p, len, &e) || e.status) return;
		u16 fc = wlan_fc(e.frame) & 0x00fc, status, aid;
		if (fc == WLAN_FC_BEACON || fc == WLAN_FC_PROBE_RESP) {
			struct wlan_bss b;
			if (wlan_parse_bss(e.frame, e.len, chan_freq(e.channel), (s32)e.snr, &b)) return;
			if (!b.freq) b.freq = chan_freq(b.chan);
			stats.bss_seen++;
			if (st != WIFI_SCAN) return;
			bool match = b.ssid_len == conf.ssid_len && !crypto_memcmp(b.ssid, conf.ssid, conf.ssid_len);
			if (conf.have_bssid) match = match && !crypto_memcmp(b.bssid, conf.bssid, 6);
			if (!match) return;
			const char *why = NULL;
			if (!b.rsn_len) why = "an open network (only WPA2-PSK here)";
			else if (wpa_check_ap_ie(b.rsn, b.rsn_len, &why)) {}
			if (why) {
				print_mac("wifi: ", b.bssid);
				mlog(" skipped: %s\n", why);
				return;
			}
			if (!have_best || b.snr > best.snr) best = b;
			have_best = true;
		} else if (fc == WLAN_FC_AUTH && st == WIFI_AUTH) {
			if (wlan_auth_resp(e.frame, e.len, bss.bssid, mac, &status)) return;
			if (status) {
				mlog("wifi: authentication refused (status %u)\n", status);
				lost("authentication");
				return;
			}
			mlog("wifi: authenticated (open system)\n");
			send_assoc();
		} else if (fc == WLAN_FC_ASSOC_RESP && st == WIFI_ASSOC) {
			if (wlan_assoc_resp(e.frame, e.len, bss.bssid, mac, &status, &aid)) return;
			if (status) {
				mlog("wifi: association refused (status %u)\n", status);
				lost("association");
				return;
			}
			associated(aid, get16(e.frame + WLAN_HDR));
		} else if ((fc == WLAN_FC_DEAUTH || fc == WLAN_FC_DISASSOC) && st >= WIFI_AUTH && st <= WIFI_UP) {
			u16 reason;
			if (wlan_deauth(e.frame, e.len, bss.bssid, &reason)) return;
			mlog("wifi: %s by the AP (reason %u)\n", fc == WLAN_FC_DEAUTH ? "deauthenticated" : "disassociated",
			     reason);
			lost("the AP");
		}
		return;
	}
	case WMI_TLV_VDEV_START_RESP_EVENTID: {
		struct wmi_vdev_start_resp e;
		if (wmi_vdev_start_resp(p, len, &e) || st != WIFI_JOIN) return;
		if (e.status) {
			mlog("wifi: vdev start refused (%u)\n", e.status);
			lost("vdev start");
			return;
		}
		/* the AP's peer (ath10k_sta_state: NOTEXIST -> NONE), then its
		 * HTT PEER_MAP (ath10k_wait_for_peer_created) */
		u64 pa;
		u8 *b = msg_new(&pa);
		if (b) wmi_cmd(b, wmi_peer_create(b + HTC_HDR_LEN, VDEV, bss.bssid, WMI_PEER_TYPE_DEFAULT));
		go(WIFI_PEER, 3000);
		return;
	}
	case WMI_TLV_MGMT_TX_COMPLETION_EVENTID:
	case WMI_TLV_MGMT_TX_BUNDLE_COMPLETION_EVENTID:
		wmi_mgmt_tx_compl(p, len, id == WMI_TLV_MGMT_TX_BUNDLE_COMPLETION_EVENTID, mgmt_done, NULL);
		return;
	case WMI_TLV_PEER_STA_KICKOUT_EVENTID:
		if (st >= WIFI_ASSOC && st <= WIFI_UP) lost("the AP is gone (kickout)");
		return;
	case WMI_TLV_ROAM_EVENTID:
		/* WMI_ROAM_REASON_BEACON_MISS (1) and the others: the AP lost */
		if (st == WIFI_UP) lost("beacons missed (roam event)");
		return;
	default:
		return;
	}
}

/* --- HTT messages --------------------------------------------------------------- */

static u32 b_ver(u8 *o) { return htt_ver_req(o); }
static u32 b_frag(u8 *o) { return htt_frag_bank_cfg(o, txext_pa, MAX_TX - 1); }
static u32 b_ring(u8 *o) { return htt_rx_ring_cfg(o, rx_idx_pa, rx_ring_pa, HTT_RX_RING_SIZE, (u16)*rx_idx); }
static u32 b_aggr(u8 *o) { return htt_aggr_cfg(o, 64, 3); }   /* ATH10K_HTT_MAX_NUM_AMPDU/AMSDU_DEFAULT */

static void htt_setup(void)
{
	/* ath10k_core_start after the unified READY: the RX ring filled,
	 * then ath10k_htt_setup: the version first */
	rx_ring_fill();
	htt_out(b_ver);
	go(WIFI_HTT_VERSION, 3000);
}

static void setup(void)
{
	/* ath10k_htt_setup's rest */
	htt_out(b_frag);
	htt_out(b_ring);
	htt_out(b_aggr);
	/* ath10k_start's device parameters */
	static const u32 params[][2] = {
		{ WMI_TLV_PDEV_PARAM_PMF_QOS, 1 }, { WMI_TLV_PDEV_PARAM_DYNAMIC_BW, 1 },
		{ WMI_TLV_PDEV_PARAM_IDLE_PS_CONFIG, 1 }, { WMI_TLV_PDEV_PARAM_ARP_AC_OVERRIDE, 0 },
		{ WMI_TLV_PDEV_PARAM_ANI_ENABLE, 1 },
	};
	for (u32 i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
		u64 pa;
		u8 *b = msg_new(&pa);
		if (b) wmi_cmd(b, wmi_pdev_set_param(b + HTC_HDR_LEN, params[i][0], params[i][1]));
	}
	/* ath10k_regd_update: the channel list (pdev_set_regdomain needs
	 * ath's regulatory tables for the EEPROM's domain: not sent, the
	 * firmware keeps its own) */
	chans_init();
	u64 pa;
	u8 *b = msg_new(&pa);
	if (b) wmi_cmd(b, wmi_scan_chan_list(b + HTC_HDR_LEN, chans, nchan));
	/* ath10k_add_interface for a station: the vdev, the power save
	 * parameters; power save off (ath10k_mac_vif_setup_ps, ps disabled) */
	b = msg_new(&pa);
	if (b) wmi_cmd(b, wmi_vdev_create(b + HTC_HDR_LEN, VDEV, WMI_VDEV_TYPE_STA, WMI_VDEV_SUBTYPE_NONE, mac));
	b = msg_new(&pa);
	if (b) wmi_cmd(b, wmi_sta_ps_param(b + HTC_HDR_LEN, VDEV, WMI_STA_PS_PARAM_RX_WAKE_POLICY, WMI_STA_PS_RX_WAKE_POLICY_WAKE));
	b = msg_new(&pa);
	if (b) wmi_cmd(b, wmi_sta_ps_mode(b + HTC_HDR_LEN, VDEV, WMI_STA_PS_MODE_DISABLED));
	retry_at = hw.now_ms() + 100;
	go(WIFI_SETUP, 0);
}

/* an RX buffer (in-order indication): the descriptor checked, the frame
 * (native Wi-Fi) to Ethernet, EAPOL to the supplicant, the rest up */
static void rx_msdu(u8 *b, u32 len)
{
	u32 attn = get32(b + HTT_RX_OFF_ATTENTION);
	u32 info1 = get32(b + HTT_RX_OFF_MSDU_INFO1);
	if (!(attn & HTT_RX_ATTN_MSDU_DONE) || (attn & (HTT_RX_ATTN_FCS_ERR | HTT_RX_ATTN_DECRYPT_ERR |
	    HTT_RX_ATTN_TKIP_MIC_ERR | HTT_RX_ATTN_MGMT_TYPE | HTT_RX_ATTN_CTRL_TYPE)) ||
	    (info1 >> HTT_RX_DECAP_SHIFT & 3) != HTT_RX_DECAP_NATIVE_WIFI || len > HTT_RX_BUF_SIZE - HTT_RX_DESC_LEN) {
		stats.rx_dropped++;
		return;
	}
	u8 eth[1600];
	u32 n = wlan_to_eth(eth, sizeof(eth), b + HTT_RX_DESC_LEN, len);
	if (!n) {
		stats.rx_dropped++;
		return;
	}
	stats.rx_frames++;
	if (get16(eth + 12) == ((ETH_P_EAPOL & 0xff) << 8 | ETH_P_EAPOL >> 8)) {
		if (st >= WIFI_4WAY) {
			eapol_in(eth + 14, n - 14);
		} else if ((st == WIFI_ASSOC || st == WIFI_AUTH) && n - 14 <= sizeof(eapol_pending)) {
			/* message 1 before the association's answer (they come
			 * on different engines): kept for later, as
			 * wpa_supplicant's pending_eapol_rx */
			mcopy(eapol_pending, eth + 14, n - 14);
			eapol_pending_len = n - 14;
		}
		return;
	}
	if (st == WIFI_UP && hw.rx) hw.rx(eth, n);
}

static void tx_done(u16 id)
{
	if (id >= MAX_TX || !tx_frame[id]) return;
	stats.tx_done++;
	slot_free(tx_frame[id]);
	tx_frame[id] = NULL;
	if (tx_kind[id] == 2 && st == WIFI_KEYS && install_step == 0) install_next();
}

static void htt_msg(const u8 *p, u32 len)
{
	if (!len) return;
	stats.htt_msgs++;
	switch (p[0]) {
	case HTT_T2H_VERSION_CONF:
		if (len < 3 || st != WIFI_HTT_VERSION) return;
		htt_minor = p[1];
		htt_major = p[2];
		mlog("wifi: HTT %u.%u\n", htt_major, htt_minor);
		if (htt_major != 2 && htt_major != 3) {
			fail("HTT version not 2 or 3 (ath10k_htt_verify_version)");
			return;
		}
		setup();
		return;
	case HTT_T2H_PEER_MAP:
		if (len < 10) return;
		if (!crypto_memcmp(p + 4, bss.bssid, 6)) {
			peer_id = get16(p + 2);
			if (st == WIFI_PEER) {
				mlog("wifi: the AP's peer, id %u\n", peer_id);
				send_auth();
			}
		}
		return;
	case HTT_T2H_RX_IN_ORD_PADDR_IND: {
		struct htt_in_ord o;
		if (htt_parse_in_ord(p + 1, len - 1, &o)) return;
		for (u32 i = 0; i < o.count; i++) {
			u64 pa;
			u16 l;
			htt_in_ord_buf(&o, i, &pa, &l);
			u8 *b = slot_by_pa(pa);
			if (!b) {
				mlog("wifi: an RX buffer not ours (%x%08x)\n", (u32)(pa >> 32), (u32)pa);
				continue;
			}
			rx_fill--;
			if (!o.offload) rx_msdu(b, l);
			slot_free(b);
		}
		rx_ring_fill();
		return;
	}
	case HTT_T2H_TX_COMPL_IND: {
		/* htt_data_tx_completion: flags, the count, flags2, the ids */
		if (len < 4) return;
		u32 n = p[2];
		if (4 + 2 * n > len) return;
		for (u32 i = 0; i < n; i++) tx_done(get16(p + 4 + 2 * i));
		return;
	}
	case HTT_T2H_SEC_IND:
		/* the key's in (ath10k_htt_t2h_msg_handler: install_key_done) */
		if (st == WIFI_KEYS) install_next();
		return;
	default:
		return;
	}
}

/* --- HTC's control messages ---------------------------------------------------- */

static int connecting;                  /* 0: HTT, 1: WMI (ath10k_core_start's order) */

static void htc_connect(u16 service)
{
	u64 pa;
	u8 *b = msg_new(&pa);
	if (!b) return;
	u32 l = htc_connect_msg(b + HTC_HDR_LEN, service, htc_credit_alloc(&htc, service));
	if (htc_out(0, b, (u16)l)) slot_free(b);
}

static void htc_ctrl(const u8 *p, u32 len)
{
	if (len < 2) return;
	u16 id = get16(p);
	if (id == HTC_MSG_READY && st == WIFI_HTC_READY) {
		if (htc_ready(&htc, p, len)) {
			fail("HTC READY not understood");
			return;
		}
		mlog("wifi: HTC ready: %u credits of %u bytes (one used), %u endpoints\n", htc.credit_count,
		     htc.credit_size, htc.max_endpoints);
		connecting = 0;
		htc_connect(HTC_SVC_HTT_DATA_MSG);
		go(WIFI_HTC_CONNECT, 3000);
	} else if (id == HTC_MSG_CONNECT_SERVICE_RESP && st == WIFI_HTC_CONNECT) {
		u16 service = connecting ? HTC_SVC_WMI_CONTROL : HTC_SVC_HTT_DATA_MSG;
		int eid;
		if (htc_connect_resp(&htc, p, len, service, &eid)) {
			fail(connecting ? "WMI's service refused" : "HTT's service refused");
			return;
		}
		if (!connecting) {
			htt_eid = eid;
			connecting = 1;
			htc_connect(HTC_SVC_WMI_CONTROL);
			return;
		}
		wmi_eid = eid;
		mlog("wifi: HTC: HTT on endpoint %d (CE%u/CE%u), WMI on %d (CE%u/CE%u)\n", htt_eid,
		     htc.ep[htt_eid].ul_pipe, htc.ep[htt_eid].dl_pipe, wmi_eid, htc.ep[wmi_eid].ul_pipe,
		     htc.ep[wmi_eid].dl_pipe);
		/* ath10k_htc_start */
		u64 pa;
		u8 *b = msg_new(&pa);
		if (b && htc_out(0, b, (u16)htc_setup_complete_msg(b + HTC_HDR_LEN))) slot_free(b);
		go(WIFI_SVC_READY, 5000);
	}
}

/* --- the engines ------------------------------------------------------------------ */

static void rx_ce(u32 ce)
{
	void *ctx;
	u32 n;
	/* ath10k_ce_per_engine_service: the status cleared first, then the
	 * completions taken (a level interrupt drops; one that comes in
	 * between is seen at the next look) */
	ce_irq_ack(ce);
	while (!ce_recv(ce, &ctx, &n)) {
		struct htc_rx m;
		if (!htc_receive(&htc, ctx, n, &m)) {
			if (m.eid == 0) htc_ctrl(m.payload, m.len);
			else if ((int)m.eid == wmi_eid) wmi_event(m.payload, m.len);
			else if ((int)m.eid == htt_eid) htt_msg(m.payload, m.len);
		}
		/* the buffer back (ath10k_snoc_rx_post_pipe) */
		if (ce_rx_post(ce, slot_pa(ctx), ctx)) slot_free(ctx);
	}
}

static void tx_ce(u32 ce)
{
	void *ctx;
	ce_irq_ack(ce);
	while (!ce_send_done(ce, &ctx))
		if (ctx) slot_free(ctx);
}

static bool polling;

void wifi_poll(void)
{
	if (st == WIFI_OFF || st == WIFI_ENABLE || polling) return;
	polling = true;
	if (st != WIFI_FAILED) {
		for (u32 ce = 0; ce < CE_COUNT; ce++)
			if (ce_attr_wlan[ce].dst_n) {
				rx_ce(ce);
				rx_post(ce);
			}
		tx_ce(0);
		tx_ce(3);
		tx_ce(4);                       /* CE4 polled (CE_ATTR_DIS_INTR) */
		wmi_flush();
	}
	u32 now = hw.now_ms();
	if (st_timeout && now - st_since > st_timeout) {
		switch (st) {
		case WIFI_AUTH: case WIFI_ASSOC: case WIFI_JOIN: case WIFI_PEER: case WIFI_4WAY: case WIFI_KEYS:
			lost("no answer in time");
			break;
		case WIFI_SCAN:
			mlog("wifi: the scan did not end\n");
			retry_at = now + 5000;
			go(WIFI_SETUP, 0);
			break;
		default:
			fail("no answer in time");
		}
	}
	if (st == WIFI_SETUP && !wmi_qn && (s32)(now - retry_at) >= 0) scan_start();
	polling = false;
}

u32 wifi_next_ms(void)
{
	if (st == WIFI_OFF || st == WIFI_FAILED || st == WIFI_ENABLE) return 0xffffffffu;
	u32 now = hw.now_ms(), n = 0xffffffffu;
	if (st_timeout) {
		u32 end = st_since + st_timeout;
		n = (s32)(end - now) > 0 ? end - now : 0;
	}
	if (st == WIFI_SETUP) n = MIN(n, (s32)(retry_at - now) > 0 ? retry_at - now : 0);
	/* without the engines' interrupts the caller polls at its own rate */
	return n;
}

int wifi_send(const u8 *eth, u32 len)
{
	if (st != WIFI_UP || len < 14) return -1;
	u64 pa;
	u8 *f = slot_alloc(&pa);
	if (!f) return -1;
	u32 n = wlan_from_eth(f, SLOT, eth, len, bss.bssid, keys_in);
	if (!n || htt_tx(f, n, HTT_TX_MODE_NATIVE_WIFI, HTT_TID_NON_QOS, 1) < 0) {
		slot_free(f);
		return -1;
	}
	return 0;
}

/* --- the start ------------------------------------------------------------------- */

void wifi_start(const struct wifi_hw *h, const struct wifi_conf *c)
{
	if (st != WIFI_OFF && st != WIFI_FAILED) return;
	hw = *h;
	conf = *c;
	if (wpa_pmk(conf.psk, conf.ssid, conf.ssid_len, pmk)) {
		mlog("wifi: the PSK is not 8-63 characters or 64 hex digits\n");
		go(WIFI_FAILED, 0);
		return;
	}
	own_ie_len = wpa_own_ie(own_ie);
	if (!pool) {
		pool = hw.ce.dma_alloc(NSLOTS * SLOT, &pool_pa);
		rx_ring = hw.ce.dma_alloc(HTT_RX_RING_SIZE * 8, &rx_ring_pa);
		rx_idx = hw.ce.dma_alloc(4, &rx_idx_pa);
		txbuf = hw.ce.dma_alloc(MAX_TX * HTT_TXBUF_LEN, &txbuf_pa);
		txext = hw.ce.dma_alloc(MAX_TX * HTT_EXT_DESC_LEN, &txext_pa);
		if (!pool || !rx_ring || !rx_idx || !txbuf || !txext) {
			mlog("wifi: no DMA memory\n");
			go(WIFI_FAILED, 0);
			return;
		}
		for (u32 i = 0; i < NSLOTS; i++) free_slots[i] = (u16)(NSLOTS - 1 - i);
		nfree = NSLOTS;
	}
	mlog("wifi: starting: WLAN_CFG, WLAN_MODE (mission)\n");
	go(WIFI_ENABLE, 0);
	hw.wlan_enable();
}

void wifi_wlan_enabled(int err)
{
	if (st != WIFI_ENABLE) return;
	if (err) {
		fail("WLFW refused WLAN_CFG or WLAN_MODE");
		return;
	}
	/* ath10k_snoc_hif_power_up: the engines after WLAN_MODE; then
	 * ath10k_core_start: HTC, the buffers posted (hif_start), READY */
	if (ce_init(&hw.ce)) {
		fail("no memory for the copy engines");
		return;
	}
	htc_init(&htc);
	wmi_eid = htt_eid = -1;
	for (u32 ce = 0; ce < CE_COUNT; ce++)
		if (ce_attr_wlan[ce].dst_n) rx_post(ce);
	mlog("wifi: copy engines up, waiting for HTC READY\n");
	go(WIFI_HTC_READY, 3000);
}

/* --- the configuration ------------------------------------------------------------ */

static int hexd(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int wifi_conf_parse(const char *t, u32 len, struct wifi_conf *c, const char **why)
{
	mzero(c, sizeof(*c));
	bool have_ssid = false, have_psk = false;
	for (u32 i = 0; i < len;) {
		u32 e = i;
		while (e < len && t[e] != '\n' && t[e]) e++;
		u32 s = i, end = e;
		while (s < end && (t[s] == ' ' || t[s] == '\t')) s++;
		while (end > s && (t[end - 1] == '\r' || t[end - 1] == ' ' || t[end - 1] == '\t')) end--;
		i = e + 1;
		if (s == end || t[s] == '#') continue;
		u32 eq = s;
		while (eq < end && t[eq] != '=') eq++;
		if (eq == end) continue;
		const char *k = t + s, *v = t + eq + 1;
		u32 kl = eq - s, vl = end - eq - 1;
		if (kl == 4 && !crypto_memcmp(k, "ssid", 4)) {
			if (!vl || vl > 32) { *why = "the SSID is 1-32 bytes"; return -1; }
			mcopy(c->ssid, v, vl);
			c->ssid_len = (u8)vl;
			have_ssid = true;
		} else if (kl == 3 && !crypto_memcmp(k, "psk", 3)) {
			if (vl > 64) { *why = "the PSK is too long"; return -1; }
			mcopy(c->psk, v, vl);
			c->psk[vl] = 0;
			have_psk = true;
		} else if (kl == 5 && !crypto_memcmp(k, "bssid", 5)) {
			if (vl != 17) { *why = "bssid=aa:bb:cc:dd:ee:ff"; return -1; }
			for (int j = 0; j < 6; j++) {
				int h = hexd(v[3 * j]), l = hexd(v[3 * j + 1]);
				if (h < 0 || l < 0) { *why = "bssid=aa:bb:cc:dd:ee:ff"; return -1; }
				c->bssid[j] = (u8)(h << 4 | l);
			}
			c->have_bssid = true;
		}
	}
	if (!have_ssid || !have_psk) {
		*why = "ssid= and psk= are needed";
		return -1;
	}
	*why = NULL;
	return 0;
}
