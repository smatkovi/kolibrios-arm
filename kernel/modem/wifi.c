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
#include "regd.h"
#include "sae.h"

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
#define WMI_QUEUE 64                      /* (mac80211's conf_tx at an association: 20 commands and more) */

static struct wifi_hw hw;
static struct wifi_conf conf;
static enum wifi_state st = WIFI_OFF;
static u32 st_since, st_timeout, retry_at;
static struct wifi_stats stats;
static u8 mac[6];
static u32 last_traffic;                /* ms: the last data frame in or out */

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
static u8 pmk[WPA_PMK_LEN];             /* the association's: PBKDF2's or SAE's */
static u8 psk_pmk[WPA_PMK_LEN];         /* PBKDF2's (the passphrase and the SSID) */
static struct wpa_choice choice, best_choice;
/* SAE (sae.c): its exchange, PT kept for the SSID and password */
static struct sae sae;
static u8 sae_pt_val[64];
static bool sae_pt_ok, sae_confirmed;
static u32 sae_tries;
/* management frame protection: our PN for protected management frames
 * (mac80211 makes their IV: IEEE80211_KEY_FLAG_GENERATE_IV_MGMT), the
 * AP's last; an SA Query (wpa_supplicant's sme_sa_query: 201 ms between
 * tries, 1000 ms at most), the association's comeback time */
static u64 mgmt_tx_pn, mgmt_rx_pn;
static bool saq_on;
static u8 saq_id[2];
static u32 saq_start, saq_next, comeback_at;
static struct wpa_sm wpa;
static u8 own_ie[WPA_MAX_IE];
static u32 own_ie_len;
static u8 own_rsnx[8];                  /* the RSN Extension element (SAE hash-to-element) */
static u32 own_rsnx_len;
static bool pmf_on;                     /* management frame protection on this association */
static bool keys_in;
static u32 tries;
static u8 eapol_pending[512];           /* an EAPOL frame before the association's answer */
static u32 eapol_pending_len;
static void eapol_in(const u8 *p, u32 len);
static void lost(const char *why);
static void sae_rx(const u8 *f, u32 len);
static void pmf_action(const u8 *f, u32 len);
static int pmf_disconnect(const u8 *f, u32 len, u16 *reason);
static u32 assoc_comeback(const u8 *f, u32 len);

/* the regulatory domain (regd.c): the EEPROM's code from SERVICE_READY,
 * the configured country; the channel list from it */
static struct regd regd;
static struct wmi_chan chans[REGD_MAX_CHANS];
static u16 freqs[REGD_MAX_CHANS];
static u32 nchan;

/* ath10k_update_channel_list: the enabled channels, passive on NO_IR and
 * radar, the power in 0.5 dBm */
static void chans_init(void)
{
	nchan = 0;
	for (u32 i = 0; i < regd.nchan; i++) {
		const struct regd_chan *c = &regd.ch[i];
		if (c->flags & REGD_DISABLED) continue;
		struct wmi_chan *w = &chans[nchan];
		mzero(w, sizeof(*w));
		w->freq = w->center1 = c->freq;
		w->mode = c->freq < 5000 ? MODE_11G : MODE_11A;
		w->list = true;
		w->ibss = !(c->flags & REGD_NO_IR);
		w->ht40plus = !(c->flags & REGD_NO_HT40PLUS);
		w->dfs = c->flags & REGD_RADAR;
		w->passive = (c->flags & REGD_NO_IR) || w->dfs;
		w->max_power = (u8)(c->max_power * 2);
		w->reg_power = (u8)(c->max_reg_power * 2);
		freqs[nchan++] = c->freq;
	}
}

static void regd_log(void)
{
	u32 passive = 0, radar = 0;
	for (u32 i = 0; i < nchan; i++) {
		passive += chans[i].passive;
		radar += chans[i].dfs;
	}
	mlog("wifi: regulatory domain: EEPROM %x (%s, ath's \"%s\"), domain %x, CTL %x/%x (2/5 GHz); country %s%s: "
	     "%u channels, %u passive, %u radar\n", regd.eeprom, regd.world ? "world" : "a country", regd.alpha2,
	     regd.pair.reg_domain, regd.pair.ctl_2g, regd.pair.ctl_5g, regd.user[0] ? regd.user : "none",
	     regd.user[0] && !regd.user_found ? " (not in the regulatory database: ignored)" : "", nchan, passive, radar);
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

/* --- HT, WMM and power save: what mac80211 decides, what ath10k sends ------ */

static struct wmi_svc_ready svc;
static struct wlan_ht own_ht;           /* ath10k_get_ht_cap (the band's ht_cap) */
static bool own_ht_ok;

/* the association's link (mac80211's conn settings and the AP's station) */
static struct {
	bool ht, bw40, wmm;                 /* HT on the channel, 40 MHz, WMM in the BSS (wmm_used) */
	bool qos;                           /* the AP's station is a QoS one (sta->wme: WMM in the association's answer) */
	bool uapsd;                         /* U-APSD asked for (assoc_data->uapsd) */
	u8 mode;                            /* VDEV_START's (chan_to_phymode) */
	u16 center;                         /* the channel's center (band_center_freq1) */
	bool peer_ht;                       /* the AP's HT capabilities, ours intersected (ieee80211_ht_cap_ie_to_sta_ht_cap) */
	struct wlan_ht peer;
	u8 acm;                             /* the priorities the AP wants admission for (sdata->wmm_acm) */
} lk;

static bool vdev_started, ps_on;        /* arvif->is_started, vif->cfg.ps */
static u32 uapsd_bits;                  /* arvif->u.sta.uapsd */
static struct wmi_wmm_ac wmm_ac[4];     /* arvif->wmm_params: BE, BK, VI, VO */

#define WMI_SEND(call) do { u64 pa_; u8 *b_ = msg_new(&pa_); if (b_) wmi_cmd(b_, (call)); } while (0)
#define CMD(b_) ((b_) + HTC_HDR_LEN)
#define SEND(fn, ...) do { u64 pa_; u8 *b_ = msg_new(&pa_); if (b_) wmi_cmd(b_, fn(CMD(b_), __VA_ARGS__)); } while (0)

/* ath10k_get_ht_cap from SERVICE_READY (ht_cap_info, the RF chains as
 * the chain masks, capped at the WCN3990's max_spatial_stream 4, the VHT
 * capabilities' MPDU length) */
static void own_ht_init(void)
{
	mzero(&own_ht, sizeof(own_ht));
	own_ht_ok = svc.ht_cap & WMI_HT_CAP_ENABLED;
	if (!own_ht_ok) return;
	u32 chains = MIN(svc.num_rf_chains, 4u), mask = (1u << chains) - 1;
	u16 cap = HT_CAP_SUP_WIDTH_20_40 | HT_CAP_DSSSCCK40 | HT_SM_PS_DISABLED << HT_CAP_SM_PS_SHIFT;
	if (svc.ht_cap & WMI_HT_CAP_HT20_SGI) cap |= HT_CAP_SGI_20;
	if (svc.ht_cap & WMI_HT_CAP_HT40_SGI) cap |= HT_CAP_SGI_40;
	if (svc.ht_cap & WMI_HT_CAP_DYNAMIC_SMPS) cap |= HT_SM_PS_DYNAMIC << HT_CAP_SM_PS_SHIFT;
	if ((svc.ht_cap & WMI_HT_CAP_TX_STBC) && mask > 1) cap |= HT_CAP_TX_STBC;
	if (svc.ht_cap & WMI_HT_CAP_RX_STBC)
		cap |= (u16)(((svc.ht_cap & WMI_HT_CAP_RX_STBC) >> WMI_HT_CAP_RX_STBC_MASK_SHIFT << HT_CAP_RX_STBC_SHIFT) & HT_CAP_RX_STBC);
	if ((svc.ht_cap & WMI_HT_CAP_LDPC) || ((svc.ht_cap & WMI_HT_CAP_RX_LDPC) && (svc.ht_cap & WMI_HT_CAP_TX_LDPC)))
		cap |= HT_CAP_LDPC;
	if (svc.ht_cap & WMI_HT_CAP_L_SIG_TXOP_PROT) cap |= HT_CAP_LSIG_TXOP_PROT;
	if (svc.vht_cap & WMI_VHT_CAP_MAX_MPDU_LEN_MASK) cap |= HT_CAP_MAX_AMSDU;
	own_ht.cap = cap;
	own_ht.ampdu = HT_AMPDU_64K | HT_MPDU_DENSITY_8 << 2;
	for (u32 i = 0; i < chains; i++)
		if (mask & (1u << i)) own_ht.mcs[i] = 0xff;
	own_ht.mcs[12] = HT_MCS_TX_DEFINED;     /* tx_params: after the RX mask (10) and the highest rate (2) */
}

/* ieee80211_ht_cap_ie_to_sta_ht_cap: the AP's capabilities as far as we
 * can use them */
static void peer_ht(const struct wlan_ht *ap)
{
	struct wlan_ht h;
	mzero(&h, sizeof(h));
	h.cap = ap->cap & (u16)(own_ht.cap | ~(HT_CAP_LDPC | HT_CAP_SUP_WIDTH_20_40 | HT_CAP_GRN_FLD | HT_CAP_SGI_20 |
					      HT_CAP_SGI_40 | HT_CAP_DSSSCCK40));
	if (!(own_ht.cap & HT_CAP_TX_STBC)) h.cap &= ~HT_CAP_RX_STBC;
	if (!(own_ht.cap & HT_CAP_RX_STBC)) h.cap &= ~HT_CAP_TX_STBC;
	h.ampdu = ap->ampdu;
	h.mcs[12] = ap->mcs[12];
	u8 tx = own_ht.mcs[12];
	u32 streams = (tx & HT_MCS_TX_RX_DIFF) ? ((tx & 0x0c) >> 2) + 1 : 4;
	for (u32 i = 0; i < streams; i++) h.mcs[i] = own_ht.mcs[i] & ap->mcs[i];
	if (tx & HT_MCS_TX_UNEQUAL)
		for (u32 i = 4; i < 10; i++) h.mcs[i] = own_ht.mcs[i] & ap->mcs[i];
	if (own_ht.mcs[4] & ap->mcs[4] & 1) h.mcs[4] |= 1;          /* MCS 32 */
	h.mcs[10] = ap->mcs[10], h.mcs[11] = ap->mcs[11];           /* rx_highest */
	lk.peer = h;
	lk.peer_ht = true;
}

/* ath10k_conf_tx (VDEV_SET_WMM_PARAMS with all four, txop in us) and
 * ath10k_conf_tx_uapsd (the AC's U-APSD bits, the wake policies, the
 * PS-Poll count, the auto trigger off), for mac80211's AC (VO 0, VI 1, BE
 * 2, BK 3) */
static void conf_tx(u32 ac, u32 cw_min, u32 cw_max, u32 aifs, u32 txop, bool uapsd)
{
	static const u8 tlv_idx[4] = { 3, 2, 0, 1 };
	static const u32 bits[4] = { 0xc0, 0x30, 0x0c, 0x03 };     /* WMI_STA_PS_UAPSD_AC3..AC0 delivery and trigger */
	static const u32 prio[4] = { 7, 5, 2, 0 }, acc[4] = { 3, 2, 1, 0 };
	struct wmi_wmm_ac *p = &wmm_ac[tlv_idx[ac]];
	p->cwmin = cw_min;
	p->cwmax = cw_max;
	p->aifs = aifs;
	p->txop = txop * 32;
	p->acm = 0;                             /* (ath10k_conf_tx does not pass it) */
	SEND(wmi_vdev_wmm, VDEV, wmm_ac);
	if (uapsd) uapsd_bits |= bits[ac];
	else uapsd_bits &= ~bits[ac];
	SEND(wmi_sta_ps_param, VDEV, WMI_STA_PS_PARAM_UAPSD, uapsd_bits);
	SEND(wmi_sta_ps_param, VDEV, WMI_STA_PS_PARAM_RX_WAKE_POLICY,
	     uapsd_bits ? WMI_STA_PS_RX_WAKE_POLICY_POLL_UAPSD : WMI_STA_PS_RX_WAKE_POLICY_WAKE);
	SEND(wmi_sta_ps_param, VDEV, WMI_STA_PS_PARAM_TX_WAKE_THRESHOLD,
	     uapsd_bits ? WMI_STA_PS_TX_WAKE_THRESHOLD_NEVER : WMI_STA_PS_TX_WAKE_THRESHOLD_ALWAYS);
	SEND(wmi_sta_ps_param, VDEV, WMI_STA_PS_PARAM_PSPOLL_COUNT,
	     uapsd_bits ? WMI_STA_PS_PSPOLL_COUNT_UAPSD : WMI_STA_PS_PSPOLL_COUNT_NO_MAX);
	if (wmi_service(&svc, WMI_TLV_SERVICE_STA_UAPSD_BASIC_AUTO_TRIG) || wmi_service(&svc, WMI_TLV_SERVICE_STA_UAPSD_VAR_AUTO_TRIG))
		SEND(wmi_uapsd_auto_trig, VDEV, vdev_started ? bss.bssid : (const u8 *)"\0\0\0\0\0\0", acc[ac], prio[ac]);
}

/* ieee80211_set_wmm_default: 802.11-2007's table 7-37 (with QoS) or the
 * old DCF values for every AC */
static void wmm_default(bool qos, bool use_11b)
{
	u32 cwmin = use_11b ? 31 : 15, cwmax = 1023;
	for (u32 ac = 0; ac < 4; ac++) {
		u32 mn = cwmin, mx = cwmax, txop = 0, aifs = 2;
		if (qos) {
			if (ac == 3) aifs = 7;                          /* BK */
			else if (ac == 2) aifs = 3;                     /* BE */
			else if (ac == 1) mx = cwmin, mn = (cwmin + 1) / 2 - 1, txop = use_11b ? 6016 / 32 : 3008 / 32;
			else mx = (cwmin + 1) / 2 - 1, mn = (cwmin + 1) / 4 - 1, txop = use_11b ? 3264 / 32 : 1504 / 32;
		}
		conf_tx(ac, mn, mx, aifs, txop, false);
	}
}

/* _ieee80211_sta_wmm_params: the AP's parameter element (24 bytes from
 * the OUI): each AC's AIFSN (at least 2), CWmin/max, TXOP, ACM; false
 * when it is not usable (then the defaults with QoS) */
static bool wmm_params(const u8 *w, u32 len)
{
	static const u8 aci_to_ac[4] = { 2, 3, 1, 0 };             /* BE, BK, VI, VO */
	static const u8 acm_bits[4] = { 0x09, 0x06, 0x30, 0xc0 };   /* BE/EE, BK/-, CL/VI, VO/NC */
	static const u8 qbit[4] = { WMM_QOSINFO_STA_AC_BE, WMM_QOSINFO_STA_AC_BK, WMM_QOSINFO_STA_AC_VI, WMM_QOSINFO_STA_AC_VO };
	u32 p[4][5];
	if (len < 8 || w[5] != 1) return false;
	mzero(p, sizeof(p));
	u8 acm = 0;
	for (u32 off = 8; off + 4 <= len; off += 4) {
		const u8 *r = w + off;
		u32 aci = r[0] >> 5 & 3, ac = aci_to_ac[aci];
		if (r[0] >> 4 & 1) acm |= acm_bits[aci];
		p[ac][0] = r[0] & 15 ? r[0] & 15 : 2;
		if (p[ac][0] < 2) p[ac][0] = 2;
		p[ac][1] = (1u << (r[1] >> 4)) - 1;                 /* cw_max */
		p[ac][2] = (1u << (r[1] & 15)) - 1;                 /* cw_min */
		p[ac][3] = get16(r + 2);
		p[ac][4] = lk.uapsd && (conf.uapsd_queues & qbit[aci]);
		if (!p[ac][2] || p[ac][2] > p[ac][1]) return false;
	}
	for (u32 ac = 0; ac < 4; ac++)
		if (!p[ac][2]) return false;
	lk.acm = acm;
	for (u32 ac = 0; ac < 4; ac++) conf_tx(ac, p[ac][2], p[ac][1], p[ac][0], p[ac][3], p[ac][4]);
	return true;
}

/* the TID of a frame to the AP (mac80211's ieee80211_select_queue and
 * ieee80211_downgrade_queue, ath10k_htt_tx_get_tid): the 802.1d priority
 * with QoS, then lower while the AP wants admission for it; without QoS
 * HTT's non-QoS TID */
static u32 tx_tid(const u8 *eth, u32 len, bool eapol)
{
	if (!lk.qos) return HTT_TID_NON_QOS;
	u32 prio = eapol ? 7 : wlan_classify(eth, len);
	while (lk.acm & (1u << prio)) {
		if (prio >= 6) prio = 5;
		else if (prio >= 4) prio = 3;
		else if (prio == 0 || prio == 3) prio = 2;
		else break;
	}
	return prio;
}

/* ath10k_mac_vif_setup_ps: forced on while the vdev is not started; with
 * power save the inactivity time (mac80211's dynamic_ps_timeout is 0 for
 * SUPPORTS_DYNAMIC_PS hardware: the beacon interval in ms) */
static void setup_ps(void)
{
	u32 mode = WMI_STA_PS_MODE_DISABLED;
	if (!vdev_started) {
		mode = WMI_STA_PS_MODE_ENABLED;
	} else if (ps_on) {
		mode = WMI_STA_PS_MODE_ENABLED;
		u32 t = bss.beacon_int * 1024 / 1000;
		SEND(wmi_sta_ps_param, VDEV, WMI_STA_PS_PARAM_INACTIVITY_TIME, t ? t : 100);
	}
	SEND(wmi_sta_ps_mode, VDEV, mode);
	stats.ps = mode == WMI_STA_PS_MODE_ENABLED;
}

static void join(void)
{
	bss = best;
	mcopy(stats.bssid, bss.bssid, 6);
	stats.freq = bss.freq;
	print_mac("wifi: joining ", bss.bssid);
	mlog(" on %u MHz (channel %u, SNR %d, beacon %u TU, DTIM %u)\n", bss.freq, bss.chan, bss.snr, bss.beacon_int,
	     bss.dtim);
	/* mac80211's ieee80211_determine_our_sta_mode and _ap_chan: HT only
	 * with WMM, with both HT elements and the HT Operation's primary
	 * channel ours; 40 MHz by its secondary channel's offset where the
	 * regulatory domain has HT40 that way (cfg80211_chandef_usable),
	 * else 20 MHz */
	/* the RSN choice (wpa.c): our elements, the PMK for the PSKs */
	choice = best_choice;
	own_ie_len = wpa_own_ie(own_ie, &choice);
	own_rsnx_len = wpa_own_rsnx(own_rsnx, &choice);
	if (choice.akm != WPA_AKM_SAE) mcopy(pmk, psk_pmk, WPA_PMK_LEN);
	pmf_on = choice.mfp;
	mgmt_tx_pn = mgmt_rx_pn = 0;
	saq_on = false;
	comeback_at = 0;
	mlog("wifi: %s%s%s\n", wpa_akm_name(choice.akm), choice.akm == WPA_AKM_SAE ? (choice.h2e ? " (hash-to-element)" :
	     " (hunting and pecking)") : "", choice.mfp ? ", management frame protection" : "");
	const struct regd_chan *rc = regd_find(&regd, bss.freq);
	mzero(&lk, sizeof(lk));
	lk.wmm = bss.wmm;
	lk.uapsd = bss.wmm && bss.uapsd;
	lk.center = bss.freq;
	lk.ht = own_ht_ok && bss.wmm && bss.ht && bss.ht_op && chan_freq(bss.ht_primary) == bss.freq;
	u8 sec = bss.ht_param & HT_PARAM_SEC_OFFSET;
	if (lk.ht && rc && sec == HT_PARAM_SEC_ABOVE && !(rc->flags & REGD_NO_HT40PLUS)) lk.bw40 = true, lk.center = bss.freq + 10;
	if (lk.ht && rc && sec == HT_PARAM_SEC_BELOW && !(rc->flags & REGD_NO_HT40MINUS)) lk.bw40 = true, lk.center = bss.freq - 10;
	/* ath10k_vdev_start_restart for a station: the mode
	 * (chan_to_phymode), the frequencies, the channel's power; no flags */
	if (bss.freq < 5000) lk.mode = lk.ht ? (lk.bw40 ? MODE_11NG_HT40 : MODE_11NG_HT20) :
				 (rc && (rc->flags & REGD_NO_OFDM)) ? MODE_11B : MODE_11G;
	else lk.mode = lk.ht ? (lk.bw40 ? MODE_11NA_HT40 : MODE_11NA_HT20) : MODE_11A;
	struct wmi_chan ch;
	mzero(&ch, sizeof(ch));
	ch.freq = bss.freq;
	ch.center1 = lk.center;
	ch.mode = lk.mode;
	ch.max_power = rc ? (u8)(rc->max_power * 2) : 40;
	ch.reg_power = rc ? (u8)(rc->max_reg_power * 2) : 40;
	mlog("wifi: %s, %s%s\n", lk.ht ? (lk.bw40 ? "HT40" : "HT20") : "no HT", lk.wmm ? "WMM" : "no WMM",
	     lk.uapsd ? (conf.uapsd_queues ? ", U-APSD" : " (U-APSD offered, not asked for)") : "");
	SEND(wmi_vdev_start, VDEV, bss.beacon_int, bss.dtim, &ch);
	go(WIFI_JOIN, 3000);                    /* ath10k_vdev_setup_sync: 3 s (ATH10K_VDEV_SETUP_TIMEOUT_HZ) */
}

static void sae_send_commit(void)
{
	u64 pa;
	u8 *f = slot_alloc(&pa);
	u8 body[SAE_COMMIT_LEN + 300];
	u32 n = sae_write_commit(&sae, body);
	if (!f || mgmt_tx(f, wlan_auth_alg(f, bss.bssid, mac, 3, 1, sae.h2e ? SAE_STATUS_H2E : 0, body, n))) {
		slot_free(f);
		fail("no room for SAE's commit");
		return;
	}
	sae_confirmed = false;
	go(WIFI_AUTH, 1000);
}

/* wpa_supplicant's sme_auth_build_sae_commit: the PWE by hash-to-element
 * (PT once per network) or hunting and pecking, a new commit */
static void sae_start(void)
{
	mzero(&sae, sizeof(sae));
	mcopy(sae.own, mac, 6);
	mcopy(sae.peer, bss.bssid, 6);
	sae.random = hw.random;
	u32 pl = mstrlen(conf.psk);
	int r;
	if (choice.h2e) {
		if (!sae_pt_ok) sae_pt_ok = !sae_pt(sae_pt_val, conf.ssid, conf.ssid_len, (const u8 *)conf.psk, pl, NULL);
		r = sae_pt_ok ? sae_pwe_from_pt(&sae, sae_pt_val) : -1;
	} else {
		r = sae_pwe_hnp(&sae, (const u8 *)conf.psk, pl);
	}
	if (r || sae_commit(&sae, NULL, NULL)) {
		lost("SAE: no password element");
		return;
	}
	sae_tries = 0;
	mlog("wifi: SAE commit out (group 19, %s)\n", sae.h2e ? "hash-to-element" : "hunting and pecking");
	sae_send_commit();
}

static void send_auth(void)
{
	eapol_pending_len = 0;
	if (choice.akm == WPA_AKM_SAE) {
		sae_start();
		return;
	}
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
	/* ieee80211_send_assoc: our HT capabilities as ieee80211_add_ht_ie
	 * changes them (no 40 MHz where the regulatory domain has no HT40 in
	 * the AP's direction, SM power save off), the WMM information
	 * element when the BSS has WMM (its QoS info: the U-APSD queues and
	 * the service period when U-APSD is asked for, else 0) */
	const struct regd_chan *rc = regd_find(&regd, bss.freq);
	struct wlan_ht h = own_ht;
	u8 sec = bss.ht_param & HT_PARAM_SEC_OFFSET;
	if (rc && ((sec == HT_PARAM_SEC_ABOVE && (rc->flags & REGD_NO_HT40PLUS)) ||
		   (sec == HT_PARAM_SEC_BELOW && (rc->flags & REGD_NO_HT40MINUS))))
		h.cap &= ~(HT_CAP_SUP_WIDTH_20_40 | HT_CAP_SGI_40);
	h.cap = (u16)((h.cap & ~HT_CAP_SM_PS) | HT_SM_PS_DISABLED << HT_CAP_SM_PS_SHIFT);
	u8 chs[REGD_MAX_CHANS];
	u32 nch = 0;
	for (u32 i = 0; i < nchan; i++)
		if ((chans[i].freq < 5000) == (bss.freq < 5000)) chs[nch++] = (u8)regd_find(&regd, chans[i].freq)->chan;
	struct wlan_assoc a = {
		.rsn = own_ie, .rsn_len = own_ie_len, .rsnx = own_rsnx, .rsnx_len = own_rsnx_len,
		.ht = lk.ht ? &h : NULL, .wmm_qos = lk.wmm ? (lk.uapsd ? conf.uapsd_queues : 0) : -1,
		.max_power = rc ? rc->max_power : 20, .chans = chs, .nchans = nch,
	};
	u32 n = wlan_assoc_req(f, &bss, mac, &a);
	if (mgmt_tx(f, n)) {
		slot_free(f);
		fail("no room for the association");
		return;
	}
	go(WIFI_ASSOC, 1000);
}

/* the rates for PEER_ASSOC: the band's rates the BSS has, in
 * ath10k_mac_bitrate_to_rate's form (500 kb/s, CCK with bit 7) */
static u32 peer_rates(u8 *o)
{
	static const u8 r24[] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 }, r5[] = { 12, 18, 24, 36, 48, 72, 96, 108 };
	const u8 *band = bss.freq < 5000 ? r24 : r5;
	u32 nb = bss.freq < 5000 ? sizeof(r24) : sizeof(r5), n = 0;
	for (u32 i = 0; i < nb; i++)
		for (u32 j = 0; j < bss.nrates; j++)
			if ((bss.rates[j] & 0x7f) == band[i]) {
				bool cck = band[i] == 2 || band[i] == 4 || band[i] == 11 || band[i] == 22;
				o[n++] = (u8)(band[i] | (cck ? 0x80 : 0));
				break;
			}
	return n;
}

/* ath10k_parse_mpdudensity: microseconds */
static u32 mpdu_density(u32 d)
{
	static const u8 us[8] = { 0, 1, 1, 1, 2, 4, 8, 16 };
	return us[d & 7];
}

static void associated(u16 aid, u16 cap, const u8 *f, u32 len)
{
	stats.aid = aid;
	mlog("wifi: associated, AID %u\n", aid);
	/* ieee80211_assoc_config_link: the AP is a QoS station with a WMM
	 * parameter element in its answer; its HT capabilities (the answer's,
	 * else the beacon's) intersected with ours; the WMM parameters
	 * (conf_tx for each AC) or the defaults with QoS */
	u8 wmm[24], wmm_len;
	struct wlan_ht aph;
	bool aph_ok;
	wlan_assoc_resp_ies(f, len, wmm, &wmm_len, &aph, &aph_ok);
	lk.qos = lk.wmm && wmm_len == 24;
	if (lk.ht) peer_ht(aph_ok ? &aph : &bss.ht_cap);
	bool use_11b = bss.freq < 5000 && lk.mode == MODE_11B;
	if (!lk.qos || !wmm_params(wmm, wmm_len)) wmm_default(true, use_11b);
	/* ath10k_peer_assoc_prepare */
	struct wmi_peer_assoc_arg a;
	mzero(&a, sizeof(a));
	mcopy(a.mac, bss.bssid, 6);
	a.vdev = VDEV;
	a.aid = aid;
	a.flags = WMI_TLV_PEER_AUTH | (bss.rsn_len ? WMI_TLV_PEER_NEED_PTK_4_WAY : 0) | (lk.qos ? WMI_TLV_PEER_QOS : 0);
	/* ath10k_peer_assoc_h_crypto: the PMF flag only with a firmware of
	 * ATH10K_FW_FEATURE_MFP_SUPPORT (not the Pro1X's: pmf=fw to try) */
	if (pmf_on && conf.pmf_fw) a.flags |= WMI_TLV_PEER_PMF;
	a.caps = cap;
	a.listen = 1;                           /* ath10k_peer_assoc_h_listen_intval: 1 for a station */
	a.nss = 1;
	a.nrates = peer_rates(a.rates);
	bool bw40 = false;
	if (lk.ht && lk.peer_ht) {
		/* ath10k_peer_assoc_h_ht */
		const struct wlan_ht *h = &lk.peer;
		bw40 = lk.bw40 && (h->cap & HT_CAP_SUP_WIDTH_20_40);
		a.flags |= WMI_TLV_PEER_HT;
		a.max_mpdu = (1u << (13 + (h->ampdu & 3))) - 1;
		a.mpdu_density = mpdu_density(h->ampdu >> 2 & 7);
		a.ht_caps = h->cap;
		a.rate_caps |= WMI_RC_HT_FLAG;
		if (h->cap & HT_CAP_LDPC) a.flags |= WMI_TLV_PEER_LDPC;
		if (bw40) a.flags |= WMI_TLV_PEER_40MHZ, a.rate_caps |= WMI_RC_CW40_FLAG;
		if (h->cap & (HT_CAP_SGI_20 | HT_CAP_SGI_40)) a.rate_caps |= WMI_RC_SGI_FLAG;
		if (h->cap & HT_CAP_TX_STBC) a.rate_caps |= WMI_RC_TX_STBC_FLAG, a.flags |= WMI_TLV_PEER_STBC;
		if (h->cap & HT_CAP_RX_STBC) {
			a.rate_caps |= ((u32)(h->cap & HT_CAP_RX_STBC) >> HT_CAP_RX_STBC_SHIFT) << WMI_RC_RX_STBC_FLAG_S;
			a.flags |= WMI_TLV_PEER_STBC;
		}
		if (h->mcs[1] && h->mcs[2]) a.rate_caps |= WMI_RC_TS_FLAG;
		else if (h->mcs[1]) a.rate_caps |= WMI_RC_DS_FLAG;
		u32 max_nss = 0, rx_nss = 0;
		for (u32 i = 0; i < 4; i++) rx_nss += h->mcs[i] != 0;    /* ieee80211_sta_init_nss */
		for (u32 i = 0; i < 77 && a.n_ht < sizeof(a.ht_rates); i++)
			if (h->mcs[i / 8] & (1u << (i % 8))) max_nss = i / 8 + 1, a.ht_rates[a.n_ht++] = (u8)i;
		if (!a.n_ht) {
			for (u32 i = 0; i < 8; i++) a.ht_rates[i] = (u8)i;
			a.n_ht = 8;
		} else {
			a.nss = MIN(rx_nss ? rx_nss : 1u, max_nss);
		}
		a.phymode = bss.freq < 5000 ? (bw40 ? MODE_11NG_HT40 : MODE_11NG_HT20) : (bw40 ? MODE_11NA_HT40 : MODE_11NA_HT20);
	} else if (bss.freq >= 5000) {
		a.phymode = MODE_11A;
	} else {
		bool ofdm = false;
		for (u32 i = 0; i < a.nrates; i++)
			if (!(a.rates[i] & 0x80)) ofdm = true;
		a.phymode = ofdm ? MODE_11G : MODE_11B;
	}
	SEND(wmi_peer_assoc, &a);
	/* ath10k_bss_assoc's rest: the peer's SM power save, the peer
	 * statistics, VDEV_UP, the dummy peer parameter (a power save state
	 * machine's workaround) */
	if (lk.ht && lk.peer_ht) {
		static const u32 smps_map[4] = { WMI_PEER_SMPS_STATIC, WMI_PEER_SMPS_DYNAMIC, WMI_PEER_SMPS_PS_NONE, WMI_PEER_SMPS_PS_NONE };
		SEND(wmi_peer_set_param, VDEV, bss.bssid, WMI_TLV_PEER_SMPS_STATE, smps_map[(lk.peer.cap & HT_CAP_SM_PS) >> HT_CAP_SM_PS_SHIFT]);
	}
	SEND(wmi_pdev_set_param, WMI_TLV_PDEV_PARAM_PEER_STATS_INFO_ENABLE, 1);
	SEND(wmi_vdev_up, VDEV, aid, bss.bssid);
	SEND(wmi_peer_set_param, VDEV, bss.bssid, WMI_TLV_PEER_DUMMY_VAR, 1);
	mlog("wifi: peer %s%s, %s, %u stream%s, %u HT rates; QoS %s\n", lk.ht && lk.peer_ht ? (bw40 ? "HT40" : "HT20") : "legacy",
	     (a.rate_caps & WMI_RC_SGI_FLAG) ? " short GI" : "", a.phymode == MODE_11B ? "11b" : a.phymode == MODE_11G ? "11g" :
	     a.phymode == MODE_11A ? "11a" : "11n", a.nss, a.nss == 1 ? "" : "s", a.n_ht, lk.qos ? "on" : "off");
	stats.ht = lk.ht && lk.peer_ht ? (bw40 ? 40 : 20) : 0;
	stats.qos = lk.qos;
	wpa_init(&wpa, pmk, &choice, mac, bss.bssid, own_ie, own_ie_len, bss.rsn, bss.rsn_len, bss.rsnx, bss.rsnx_len, hw.random);
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
	if (!n || htt_tx(f, n, HTT_TX_MODE_NATIVE_WIFI, tx_tid(eth, 14 + len, true), kind) < 0) {
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
		mlog("), %s CCMP%s\n", wpa_akm_name(choice.akm), pmf_on ? ", PMF" : "");
		go(WIFI_UP, 0);
		/* ieee80211_recalc_ps_vif: power save once the port is
		 * authorized (cfg80211's default: CONFIG_CFG80211_DEFAULT_PS) */
		if (!conf.no_ps) {
			ps_on = true;
			setup_ps();
			mlog("wifi: power save on (inactivity %u ms, %s)\n", bss.beacon_int * 1024 / 1000,
			     uapsd_bits ? "U-APSD" : "PS-Poll/null data");
		}
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
	if (r & WPA_DO_IGTK) mlog("wifi: the IGTK %d kept (BIP in software, as ath10k)\n", wpa.igtk_idx);
}

/* lost: back to scanning after the vdev is down (mac80211's connection loss) */
static void lost(const char *why)
{
	mlog("wifi: %s: disconnected\n", why);
	if (st == WIFI_UP && hw.link) hw.link(false, mac);
	keys_in = false;
	/* mac80211's disassociation: power save off while the vdev is still
	 * started, then ath10k_bss_disassoc and the channel context's end */
	if (ps_on) {
		ps_on = false;
		setup_ps();
	}
	/* ath10k_bss_disassoc (vdev down), ath10k_sta_state (the peer),
	 * ath10k_vdev_stop */
	u64 pa;
	u8 *b = msg_new(&pa);
	if (b) wmi_cmd(b, wmi_vdev_down(b + HTC_HDR_LEN, VDEV));
	b = msg_new(&pa);
	if (b) wmi_cmd(b, wmi_peer_delete(b + HTC_HDR_LEN, VDEV, bss.bssid));
	b = msg_new(&pa);
	if (b) wmi_cmd(b, wmi_vdev_stop(b + HTC_HDR_LEN, VDEV));
	vdev_started = false;
	stats.restarts++;
	retry_at = hw.now_ms() + 2000;
	go(WIFI_SETUP, 0);
}

/* --- SAE's frames (wpa_supplicant's sme_sae_auth) ------------------------------- */

static void sae_rx(const u8 *f, u32 len)
{
	if (len < WLAN_HDR + 6 || crypto_memcmp(f + 10, bss.bssid, 6) || crypto_memcmp(f + 4, mac, 6)) return;
	u16 alg = get16(f + WLAN_HDR), seq = get16(f + WLAN_HDR + 2), status = get16(f + WLAN_HDR + 4);
	const u8 *b = f + WLAN_HDR + 6;
	u32 bl = len - WLAN_HDR - 6;
	if (alg != 3) return;
	if (seq == 1 && status == SAE_STATUS_ANTI_CLOGGING && !sae_confirmed) {
		/* the AP wants its token back with our commit */
		if (sae_take_token(&sae, b, bl) || ++sae_tries > 3) {
			lost("SAE: an anti-clogging request not understood");
			return;
		}
		mlog("wifi: SAE: the AP asks for its anti-clogging token: commit again\n");
		sae_send_commit();
		return;
	}
	if (seq == 1 && status == SAE_STATUS_GROUP_UNSUPPORTED) {
		lost("SAE: the AP has no group 19");
		return;
	}
	if (seq == 1) {
		if (sae_confirmed) return;      /* (waiting for its confirm) */
		if (status != (sae.h2e ? SAE_STATUS_H2E : 0)) {
			mlog("wifi: SAE: the AP's commit with status %u\n", status);
			lost("SAE");
			return;
		}
		u16 r = sae_parse_commit(&sae, b, bl);
		if (r == SAE_SILENT) return;
		if (r || sae_process(&sae)) {
			mlog("wifi: SAE: the AP's commit refused (%u)\n", r);
			lost("SAE");
			return;
		}
		u8 body[SAE_CONFIRM_LEN];
		u32 n = sae_write_confirm(&sae, body);
		u64 pa;
		u8 *o = slot_alloc(&pa);
		if (!o || mgmt_tx(o, wlan_auth_alg(o, bss.bssid, mac, 3, 2, 0, body, n))) {
			slot_free(o);
			fail("no room for SAE's confirm");
			return;
		}
		sae_confirmed = true;
		mlog("wifi: SAE confirm out\n");
		go(WIFI_AUTH, 1000);
		return;
	}
	if (seq == 2) {
		if (!sae_confirmed) return;
		if (status || sae_check_confirm(&sae, b, bl)) {
			mlog("wifi: SAE: the AP's confirm is wrong (the password?)\n");
			lost("SAE");
			return;
		}
		mcopy(pmk, sae.pmk, WPA_PMK_LEN);
		mlog("wifi: authenticated (SAE)\n");
		send_assoc();
	}
}

/* --- management frame protection (mac80211's ieee80211_drop_unencrypted_mgmt,
 * ieee80211_process_sa_query_req; wpa_supplicant's sme_event_unprot_disconnect,
 * sme_sa_query_*) ----------------------------------------------------------------- */

static void send_sa_query(u8 action, const u8 id[2])
{
	u64 pa;
	u8 *f = slot_alloc(&pa);
	if (!f) return;
	if (mgmt_tx(f, wlan_sa_query(f, bss.bssid, mac, action, id, ++mgmt_tx_pn))) slot_free(f);
}

static void saq_begin(void)
{
	if (saq_on) return;
	hw.random(saq_id, 2);
	saq_on = true;
	saq_start = hw.now_ms();
	saq_next = saq_start + 201;
	mlog("wifi: an unprotected disconnection: an SA Query to the AP\n");
	send_sa_query(0, saq_id);
}

/* a deauthentication or disassociation while PMF is on: 0 when it counts
 * (the reason in *reason), 1 when it is dropped */
static int pmf_disconnect(const u8 *f, u32 len, u16 *reason)
{
	if (f[4] & 1) {
		/* group addressed: BIP with the IGTK */
		if (wpa_bip_check(&wpa, f, len)) {
			mlog("wifi: a group disconnection without a valid BIP MIC: dropped\n");
			return 1;
		}
		*reason = get16(f + WLAN_HDR);
		return 0;
	}
	u64 pn;
	const u8 *b;
	u32 bl;
	if (wlan_ccmp_mgmt(f, len, &pn, &b, &bl)) {
		saq_begin();
		return 1;
	}
	if (pn <= mgmt_rx_pn || bl < 2) return 1;      /* a replay */
	mgmt_rx_pn = pn;
	*reason = get16(b);
	return 0;
}

static void pmf_action(const u8 *f, u32 len)
{
	u64 pn;
	const u8 *b;
	u32 bl;
	if (!pmf_on || !keys_in || (f[4] & 1) || crypto_memcmp(f + 10, bss.bssid, 6)) return;
	/* robust: only protected ones, the PN above the last */
	if (wlan_ccmp_mgmt(f, len, &pn, &b, &bl) || pn <= mgmt_rx_pn || bl < 4 || b[0] != 8) return;
	mgmt_rx_pn = pn;
	if (b[1] == 0) {
		send_sa_query(1, b + 2);        /* the AP asks: we are here */
		stats.saq_answered++;
	} else if (b[1] == 1 && saq_on && !crypto_memcmp(b + 2, saq_id, 2)) {
		saq_on = false;
		mlog("wifi: SA Query answered: still associated\n");
	}
}

/* the association's answer: the Timeout Interval's association comeback
 * time in TUs (element 56, type 3) */
static u32 assoc_comeback(const u8 *f, u32 len)
{
	for (const u8 *p = f + WLAN_HDR + 6, *end = f + len; end - p >= 2 && p + 2 + p[1] <= end; p += 2 + p[1])
		if (p[0] == 56 && p[1] == 5 && p[2] == 3) return get32(p + 3);
	return 0;
}

/* --- WMI events --------------------------------------------------------------- */

static void service_ready(const u8 *p, u32 len)
{
	int r = wmi_service_ready(p, len, &svc);
	if (r) {
		fail(r == -2 ? "SERVICE_READY: another WMI ABI" : "SERVICE_READY not understood");
		return;
	}
	mgmt_wmi = wmi_service(&svc, WMI_TLV_SERVICE_MGMT_TX_WMI);
	own_ht_init();
	mlog("wifi: SERVICE_READY: firmware build %x, %u RF chains, %u memory requests, 2.4 GHz %u-%u, 5 GHz %u-%u "
	     "MHz, regulatory domain %x; RX full reorder %s, management frames by %s\n", svc.fw_build, svc.num_rf_chains,
	     svc.nreqs, svc.low_2ghz_chan, svc.high_2ghz_chan, svc.low_5ghz_chan, svc.high_5ghz_chan, svc.eeprom_rd,
	     wmi_service(&svc, WMI_TLV_SERVICE_RX_FULL_REORDER) ? "yes" : "no", mgmt_wmi ? "WMI" : "HTT");
	if (!wmi_service(&svc, WMI_TLV_SERVICE_RX_FULL_REORDER)) {
		fail("no RX full reorder (RX_IN_ORD_PADDR_IND): the ring-order RX path is not done");
		return;
	}
	/* ath10k_mac_register's ath_regd_init (ath10k_mac_init_rd: the
	 * EEPROM's code, 16 bits): Linux registers no wiphy without it */
	const char *rwhy;
	if (regd_init(&regd, svc.eeprom_rd, conf.country, (u16)svc.low_5ghz_chan, (u16)svc.high_5ghz_chan, &rwhy)) {
		mlog("wifi: regulatory domain %x: %s\n", svc.eeprom_rd & 0xffff, rwhy);
		fail("no regulatory domain");
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
		/* ath10k_mac_register: "invalid MAC address; choosing random"
		 * (Linux's log on the phone: its firmware reports none),
		 * eth_random_addr: unicast, locally administered */
		bool zero = true;
		for (int i = 0; i < 6; i++) zero &= mac[i] == 0;
		if (zero || (mac[0] & 1)) {
			hw.random(mac, 6);
			mac[0] = (u8)((mac[0] & 0xfe) | 0x02);
			print_mac("wifi: invalid MAC address; choosing random ", mac);
			mlog("\n");
		}
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
		/* dropped as ath10k_wmi_event_mgmt_rx drops them: CRC, decrypt,
		 * key cache miss (not EXT_INFO, 0x40: extension data, nor MIC) */
		if (wmi_mgmt_rx(p, len, &e) || (e.status & (0x01 | 0x08 | 0x20))) return;
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
			struct wpa_ap ap;
			struct wpa_choice c;
			if (!b.rsn_len) why = "an open network (only WPA2/WPA3 personal here)";
			else if (!wpa_parse_ap(b.rsn, b.rsn_len, b.rsnx, b.rsnx_len, &ap, &why)) {
				/* no SAE with a raw PSK (64 hex digits: no password);
				 * pmf=off: as ieee80211w=0 */
				if (mstrlen(conf.psk) == 64) ap.akms &= ~(1u << WPA_AKM_SAE);
				if (conf.no_pmf) ap.caps &= (u16)~WPA_CAP_MFPC;
				wpa_choose(&ap, b.h2e_only, &c, &why);
			}
			if (why) {
				print_mac("wifi: ", b.bssid);
				mlog(" skipped: %s\n", why);
				return;
			}
			if (!have_best || b.snr > best.snr) best = b, best_choice = c;
			have_best = true;
		} else if (fc == WLAN_FC_AUTH && st == WIFI_AUTH && choice.akm == WPA_AKM_SAE) {
			sae_rx(e.frame, e.len);
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
			if (status == 30) {
				/* rejected for now (an SA Query to our old
				 * association runs): again after the Timeout
				 * Interval's comeback time (mac80211) */
				u32 tu = assoc_comeback(e.frame, e.len);
				u32 ms = tu * 1024 / 1000 < 100 ? 100 : tu * 1024 / 1000;
				mlog("wifi: association refused for now (PMF), again in %u ms\n", ms);
				comeback_at = hw.now_ms() + ms;
				go(WIFI_ASSOC, ms + 1000);
				return;
			}
			if (status) {
				mlog("wifi: association refused (status %u)\n", status);
				lost("association");
				return;
			}
			associated(aid, get16(e.frame + WLAN_HDR), e.frame, e.len);
		} else if (fc == WLAN_FC_ACTION && st == WIFI_UP) {
			pmf_action(e.frame, e.len);
		} else if ((fc == WLAN_FC_DEAUTH || fc == WLAN_FC_DISASSOC) && st >= WIFI_AUTH && st <= WIFI_UP) {
			u16 reason;
			if (wlan_deauth(e.frame, e.len, bss.bssid, &reason)) return;
			if (pmf_on && keys_in && pmf_disconnect(e.frame, e.len, &reason)) return;
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
		/* ath10k_mac_op_assign_vif_chanctx: started, the power save
		 * mode; then the AP's peer (ath10k_sta_state: NOTEXIST -> NONE)
		 * and its HTT PEER_MAP (ath10k_wait_for_peer_created) */
		vdev_started = true;
		setup_ps();
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
	/* ath10k_regd_update: the channel list, then the domain pair
	 * (WMI_UNINIT_DFS_DOMAIN: no CONFIG_ATH10K_DFS_CERTIFIED) */
	chans_init();
	regd_log();
	u64 pa;
	u8 *b = msg_new(&pa);
	if (b) wmi_cmd(b, wmi_scan_chan_list(b + HTC_HDR_LEN, chans, nchan));
	b = msg_new(&pa);
	if (b) wmi_cmd(b, wmi_pdev_set_rd(b + HTC_HDR_LEN, regd.pair.reg_domain, regd.pair.reg_domain, regd.pair.reg_domain,
					  regd.pair.ctl_2g, regd.pair.ctl_5g, 0));
	/* ath10k_add_interface for a station: the vdev, the power save
	 * parameters (no PS mode yet: that comes with the vdev's start) */
	b = msg_new(&pa);
	if (b) wmi_cmd(b, wmi_vdev_create(b + HTC_HDR_LEN, VDEV, WMI_VDEV_TYPE_STA, WMI_VDEV_SUBTYPE_NONE, mac));
	SEND(wmi_sta_ps_param, VDEV, WMI_STA_PS_PARAM_RX_WAKE_POLICY, WMI_STA_PS_RX_WAKE_POLICY_WAKE);
	SEND(wmi_sta_ps_param, VDEV, WMI_STA_PS_PARAM_TX_WAKE_THRESHOLD, WMI_STA_PS_TX_WAKE_THRESHOLD_ALWAYS);
	SEND(wmi_sta_ps_param, VDEV, WMI_STA_PS_PARAM_PSPOLL_COUNT, WMI_STA_PS_PSPOLL_COUNT_NO_MAX);
	/* ieee80211_do_open: the default queue parameters (no QoS for a
	 * station interface) */
	uapsd_bits = 0;
	vdev_started = ps_on = false;
	wmm_default(false, false);
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
	last_traffic = hw.now_ms();
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
	if (st == WIFI_ASSOC && comeback_at && (s32)(now - comeback_at) >= 0) {
		comeback_at = 0;
		send_assoc();
	}
	if (st == WIFI_UP && saq_on) {
		if (now - saq_start >= 1000) {
			saq_on = false;
			lost("SA Query: no answer (the AP lost us)");
		} else if ((s32)(now - saq_next) >= 0) {
			saq_next = now + 201;
			send_sa_query(0, saq_id);
		}
	}
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
	if (comeback_at) n = MIN(n, (s32)(comeback_at - now) > 0 ? comeback_at - now : 0);
	if (saq_on) n = MIN(n, (s32)(saq_next - now) > 0 ? saq_next - now : 0);
	/* without the engines' interrupts the caller polls at its own rate */
	return n;
}

u32 wifi_poll_ms(void)
{
	if (st != WIFI_UP) return 10;
	u32 idle = hw.now_ms() - last_traffic;
	return idle < 1000 ? 20 : idle < 10000 ? 200 : 1000;
}

bool wifi_busy(void)
{
	return st != WIFI_UP || hw.now_ms() - last_traffic < 1000;
}

int wifi_send(const u8 *eth, u32 len)
{
	if (st != WIFI_UP || len < 14) return -1;
	last_traffic = hw.now_ms();
	u64 pa;
	u8 *f = slot_alloc(&pa);
	if (!f) return -1;
	u32 n = wlan_from_eth(f, SLOT, eth, len, bss.bssid, keys_in);
	if (!n || htt_tx(f, n, HTT_TX_MODE_NATIVE_WIFI, tx_tid(eth, len, false), 1) < 0) {
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
	if (wpa_pmk(conf.psk, conf.ssid, conf.ssid_len, psk_pmk)) {
		mlog("wifi: the PSK is not 8-63 characters or 64 hex digits\n");
		go(WIFI_FAILED, 0);
		return;
	}
	sae_pt_ok = false;
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
	c->country[0] = 'A', c->country[1] = 'T';
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
		} else if (kl == 7 && !crypto_memcmp(k, "country", 7)) {
			bool ok = vl == 2 && ((v[0] >= 'A' && v[0] <= 'Z' && v[1] >= 'A' && v[1] <= 'Z') || (v[0] == '0' && v[1] == '0'));
			if (!ok) { *why = "country=XX: two capital letters (ISO 3166), or 00"; return -1; }
			c->country[0] = v[0], c->country[1] = v[1], c->country[2] = 0;
		} else if ((kl == 9 && !crypto_memcmp(k, "powersave", 9)) || (kl == 5 && !crypto_memcmp(k, "uapsd", 5))) {
			bool on = vl == 2 && !crypto_memcmp(v, "on", 2), off = vl == 3 && !crypto_memcmp(v, "off", 3);
			if (!on && !off) { *why = kl == 5 ? "uapsd=on or off" : "powersave=on or off"; return -1; }
			if (kl == 9) c->no_ps = off;
			else c->uapsd_queues = on ? WMM_QOSINFO_STA_AC_VO | WMM_QOSINFO_STA_AC_VI | WMM_QOSINFO_STA_AC_BK | WMM_QOSINFO_STA_AC_BE : 0;
		} else if (kl == 3 && !crypto_memcmp(k, "pmf", 3)) {
			bool on = vl == 2 && !crypto_memcmp(v, "on", 2), off = vl == 3 && !crypto_memcmp(v, "off", 3);
			bool fw = vl == 2 && !crypto_memcmp(v, "fw", 2);
			if (!on && !off && !fw) { *why = "pmf=on, off or fw"; return -1; }
			c->no_pmf = off;
			c->pmf_fw = fw;
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
