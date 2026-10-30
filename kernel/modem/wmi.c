/* KolibriOS for ARM: WMI-TLV (wmi.h), after Linux's ath10k wmi-tlv.c:
 * ath10k_wmi_tlv_iter (with wmi_tlv_policies' minimum lengths),
 * ath10k_wmi_tlv_op_pull_svc_rdy_ev, ath10k_wmi_tlv_op_pull_rdy_ev,
 * ath10k_wmi_tlv_op_gen_init. */
#include "wmi.h"

void wmi_put_hdr(u8 *out, u32 id)
{
	put32(out, id & WMI_ID_MASK);
}

u32 wmi_id(const u8 *msg)
{
	return get32(msg) & WMI_ID_MASK;
}

void wmi_put_tlv(u8 *out, u16 tag, u16 len)
{
	put16(out, len);
	put16(out + 2, tag);
}

static u16 min_len(u16 tag)
{
	switch (tag) {
	case WMI_TLV_TAG_STRUCT_SERVICE_READY_EVENT: return WMI_SVC_RDY_LEN;
	case WMI_TLV_TAG_STRUCT_HAL_REG_CAPABILITIES: return WMI_HAL_REG_LEN;
	case WMI_TLV_TAG_STRUCT_WLAN_HOST_MEM_REQ: return WMI_MEM_REQ_LEN;
	case WMI_TLV_TAG_STRUCT_READY_EVENT: return WMI_RDY_LEN;
	case WMI_TLV_TAG_STRUCT_SCAN_EVENT: return WMI_SCAN_EVENT_LEN;
	case WMI_TLV_TAG_STRUCT_MGMT_RX_HDR: return WMI_MGMT_RX_LEN;
	case WMI_TLV_TAG_STRUCT_VDEV_START_RESPONSE_EVENT: return WMI_VDEV_START_RESP_LEN;
	case WMI_TLV_TAG_STRUCT_PEER_STA_KICKOUT_EVENT: return WMI_PEER_KICKOUT_LEN;
	case WMI_TLV_TAG_STRUCT_ROAM_EVENT: return WMI_ROAM_LEN;
	default: return 0;
	}
}

int wmi_tlv_each(const u8 *p, u32 len, int (*fn)(u16 tag, const u8 *v, u16 len, void *arg), void *arg)
{
	while (len) {
		if (len < WMI_TLV_HDR) return -1;
		u16 tl = get16(p), tag = get16(p + 2);
		p += WMI_TLV_HDR;
		len -= WMI_TLV_HDR;
		if (tl > len || tl < min_len(tag)) return -1;
		int r = fn(tag, p, tl, arg);
		if (r) return r;
		p += tl;
		len -= tl;
	}
	return 0;
}

/* --- SERVICE_READY ------------------------------------------------------ */

struct svc_parse {
	const u8 *ev, *reg, *bmap, *reqs;
	u16 bmap_len, reqs_len;
	struct wmi_svc_ready *out;
};

static int svc_tlv(u16 tag, const u8 *v, u16 len, void *arg)
{
	struct svc_parse *s = arg;
	switch (tag) {
	case WMI_TLV_TAG_STRUCT_SERVICE_READY_EVENT: s->ev = v; break;
	case WMI_TLV_TAG_STRUCT_HAL_REG_CAPABILITIES: s->reg = v; break;
	case WMI_TLV_TAG_ARRAY_STRUCT: s->reqs = v, s->reqs_len = len; break;
	case WMI_TLV_TAG_ARRAY_UINT32:
		/* the first is the service bitmap (then the DBS modes) */
		if (!s->bmap) s->bmap = v, s->bmap_len = len;
		break;
	}
	return 0;
}

static int req_tlv(u16 tag, const u8 *v, u16 len, void *arg)
{
	struct wmi_svc_ready *o = arg;
	if (tag != WMI_TLV_TAG_STRUCT_WLAN_HOST_MEM_REQ) return -1;
	if (o->nreqs >= WMI_MAX_MEM_REQS) return -1;
	struct wmi_mem_req *r = &o->req[o->nreqs++];
	r->req_id = get32(v);
	r->unit_size = get32(v + 4);
	r->num_unit_info = get32(v + 8);
	r->num_units = get32(v + 12);
	return 0;
}

int wmi_service_ready(const u8 *p, u32 len, struct wmi_svc_ready *out)
{
	struct svc_parse s = { .out = out };
	mzero(out, sizeof(*out));
	if (wmi_tlv_each(p, len, svc_tlv, &s)) return -1;
	if (!s.ev || !s.reg || !s.bmap || !s.reqs) return -1;
	/* wmi_tlv_svc_rdy_ev: fw_build_vers, abi (24), then words */
	const u8 *e = s.ev;
	const u8 *abi = e + 4;
	if (get32(abi) != WMI_TLV_ABI_VER0 || get32(abi + 8) != WMI_TLV_ABI_VER_NS0 ||
	    get32(abi + 12) != WMI_TLV_ABI_VER_NS1 || get32(abi + 16) != WMI_TLV_ABI_VER_NS2 ||
	    get32(abi + 20) != WMI_TLV_ABI_VER_NS3)
		return -2;
	out->fw_build = get32(e);
	out->abi_ver0 = get32(abi);
	out->abi_ver1 = get32(abi + 4);
	const u8 *w = e + 28;
	out->phy_capability = get32(w);
	out->num_rf_chains = get32(w + 8);
	out->ht_cap = get32(w + 12);
	out->vht_cap = get32(w + 16);
	out->vht_supp_mcs = get32(w + 20);
	out->min_tx_power = get32(w + 24);
	out->max_tx_power = get32(w + 28);
	out->sys_cap_info = get32(w + 32);
	out->num_mem_reqs = get32(w + 44);
	out->max_num_scan_chans = get32(w + 48);
	/* hal_reg_capabilities: eeprom_rd, eeprom_rd_ext, regcap1, regcap2,
	 * wireless_modes, the channels */
	out->eeprom_rd = get32(s.reg);
	out->low_2ghz_chan = get32(s.reg + 20);
	out->high_2ghz_chan = get32(s.reg + 24);
	out->low_5ghz_chan = get32(s.reg + 28);
	out->high_5ghz_chan = get32(s.reg + 32);
	out->svc_map = s.bmap;
	out->svc_map_len = s.bmap_len;
	return wmi_tlv_each(s.reqs, s.reqs_len, req_tlv, out) ? -1 : 0;
}

bool wmi_service(const struct wmi_svc_ready *s, u32 id)
{
	if (!s->svc_map || id >= s->svc_map_len) return false;
	return get32(s->svc_map + 4 * (id / 4)) & 1u << (id % 4);
}

/* --- READY ---------------------------------------------------------------- */

static int rdy_tlv(u16 tag, const u8 *v, u16 len, void *arg)
{
	if (tag == WMI_TLV_TAG_STRUCT_READY_EVENT) *(const u8 **)arg = v;
	return 0;
}

int wmi_ready(const u8 *p, u32 len, struct wmi_ready *out)
{
	const u8 *ev = 0;
	mzero(out, sizeof(*out));
	if (wmi_tlv_each(p, len, rdy_tlv, &ev) || !ev) return -1;
	/* wmi_tlv_rdy_ev: abi (24), wmi_mac_addr (6 and 2 of padding), status */
	out->sw_version = get32(ev);
	out->abi_version = get32(ev + 4);
	mcopy(out->mac, ev + 24, 6);
	out->status = get32(ev + 32);
	return 0;
}

/* --- INIT ----------------------------------------------------------------- */

void wmi_init_defaults(struct wmi_res_cfg *c, bool rx_full_reorder, u32 wow_patterns, bool ack_rssi)
{
	mzero(c, sizeof(*c));
	c->num_vdevs = 4;                       /* TARGET_TLV_NUM_VDEVS */
	c->num_peers = 33;                      /* hw_params.num_peers: TARGET_HL_TLV_NUM_PEERS */
	c->ast_skid_limit = 16;                 /* TARGET_HL_TLV_AST_SKID_LIMIT */
	c->num_wds_entries = 2;                 /* TARGET_HL_TLV_NUM_WDS_ENTRIES */
	c->num_offload_peers = c->num_offload_reorder_bufs = rx_full_reorder ? 4 : 0;
	c->num_peer_keys = 2;
	c->num_tids = 33 * 2;                   /* num_peers * 2 */
	c->tx_chain_mask = c->rx_chain_mask = 0x7;
	c->rx_timeout_pri[0] = c->rx_timeout_pri[1] = c->rx_timeout_pri[2] = 0x64;
	c->rx_timeout_pri[3] = 0x28;
	c->rx_decap_mode = 1;                   /* ATH10K_HW_TXRX_NATIVE_WIFI */
	c->scan_max_pending_reqs = 4;
	c->bmiss_offload_max_vdev = c->roam_offload_max_vdev = 4;
	c->roam_offload_max_ap_profiles = 8;
	c->tx_dbg_log_size = 0x400;
	c->gtk_offload_max_vdev = 2;
	c->num_msdu_desc = 1024 + 32;           /* TARGET_TLV_NUM_MSDU_DESC */
	c->max_frag_entries = 2;
	c->num_tdls_vdevs = 1;                  /* TARGET_TLV_NUM_TDLS_VDEVS */
	c->num_tdls_conn_table_entries = 0x20;
	c->beacon_tx_offload_max_vdev = 2;
	c->num_multicast_filter_entries = 5;
	c->num_wow_filters = wow_patterns;
	c->num_keep_alive_pattern = 6;
	c->max_tdls_concurrent_sleep_sta = c->max_tdls_concurrent_buffer_sta = 1;
	c->host_capab = 1u << 9;                /* WMI_TLV_FLAG_MGMT_BUNDLE_TX_COMPL */
	if (ack_rssi) c->host_capab |= 1u << 18;   /* WMI_RSRC_CFG_FLAG_TX_ACK_RSSI */
}

int wmi_init_cmd(u8 *out, u32 cap, const struct wmi_res_cfg *c, const struct wmi_mem_chunk *chunks, int n)
{
	u32 chunks_len = (u32)n * (WMI_TLV_HDR + WMI_MEM_CHUNK_LEN);
	u32 len = WMI_HDR_LEN + WMI_TLV_HDR + WMI_INIT_LEN + WMI_TLV_HDR + WMI_RES_CFG_LEN + WMI_TLV_HDR + chunks_len;
	if (n < 0 || len > cap || chunks_len > 0xffff) return -1;
	mzero(out, len);
	wmi_put_hdr(out, WMI_TLV_INIT_CMDID);
	u8 *p = out + WMI_HDR_LEN;
	/* wmi_tlv_init_cmd: the ABI, the number of chunks */
	wmi_put_tlv(p, WMI_TLV_TAG_STRUCT_INIT_CMD, WMI_INIT_LEN);
	p += WMI_TLV_HDR;
	put32(p, WMI_TLV_ABI_VER0);
	put32(p + 4, WMI_TLV_ABI_VER1);
	put32(p + 8, WMI_TLV_ABI_VER_NS0);
	put32(p + 12, WMI_TLV_ABI_VER_NS1);
	put32(p + 16, WMI_TLV_ABI_VER_NS2);
	put32(p + 20, WMI_TLV_ABI_VER_NS3);
	put32(p + 24, (u32)n);
	p += WMI_INIT_LEN;
	/* wmi_tlv_resource_config, its 44 words in order */
	wmi_put_tlv(p, WMI_TLV_TAG_STRUCT_RESOURCE_CONFIG, WMI_RES_CFG_LEN);
	p += WMI_TLV_HDR;
	const u32 w[44] = {
		c->num_vdevs, c->num_peers, c->num_offload_peers, c->num_offload_reorder_bufs, c->num_peer_keys,
		c->num_tids, c->ast_skid_limit, c->tx_chain_mask, c->rx_chain_mask, c->rx_timeout_pri[0],
		c->rx_timeout_pri[1], c->rx_timeout_pri[2], c->rx_timeout_pri[3], c->rx_decap_mode,
		c->scan_max_pending_reqs, c->bmiss_offload_max_vdev, c->roam_offload_max_vdev,
		c->roam_offload_max_ap_profiles, c->num_mcast_groups, c->num_mcast_table_elems, c->mcast2ucast_mode,
		c->tx_dbg_log_size, c->num_wds_entries, c->dma_burst_size, c->mac_aggr_delim,
		c->rx_skip_defrag_timeout_dup_detection_check, c->vow_config, c->gtk_offload_max_vdev,
		c->num_msdu_desc, c->max_frag_entries, c->num_tdls_vdevs, c->num_tdls_conn_table_entries,
		c->beacon_tx_offload_max_vdev, c->num_multicast_filter_entries, c->num_wow_filters,
		c->num_keep_alive_pattern, c->keep_alive_pattern_size, c->max_tdls_concurrent_sleep_sta,
		c->max_tdls_concurrent_buffer_sta, c->wmi_send_separate, c->num_ocb_vdevs, c->num_ocb_channels,
		c->num_ocb_schedules, c->host_capab,
	};
	for (int i = 0; i < 44; i++) put32(p + 4 * i, w[i]);
	p += WMI_RES_CFG_LEN;
	/* the chunks: an array of structures, each with its own TLV header
	 * (ath10k_wmi_tlv_put_host_mem_chunks) */
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_STRUCT, (u16)chunks_len);
	p += WMI_TLV_HDR;
	for (int i = 0; i < n; i++) {
		wmi_put_tlv(p, WMI_TLV_TAG_STRUCT_WLAN_HOST_MEMORY_CHUNK, WMI_MEM_CHUNK_LEN);
		put32(p + 4, chunks[i].req_id);
		put32(p + 8, (u32)chunks[i].pa);
		put32(p + 12, chunks[i].size);
		put32(p + 16, (u32)(chunks[i].pa >> 32));
		p += WMI_TLV_HDR + WMI_MEM_CHUNK_LEN;
	}
	return (int)len;
}

/* --- the commands of a station -------------------------------------------- */

/* a command's header and its first TLV; the TLV's value zeroed */
static u8 *cmd_start(u8 *o, u32 id, u16 tag, u16 len)
{
	wmi_put_hdr(o, id);
	wmi_put_tlv(o + WMI_HDR_LEN, tag, len);
	mzero(o + WMI_HDR_LEN + WMI_TLV_HDR, len);
	return o + WMI_HDR_LEN + WMI_TLV_HDR;
}

static void put_mac(u8 *p, const u8 *mac)
{
	mcopy(p, mac, 6);
	p[6] = p[7] = 0;
}

static u32 round4(u32 n) { return (n + 3) & ~3u; }

u32 wmi_pdev_set_param(u8 *o, u32 id, u32 value)
{
	u8 *c = cmd_start(o, WMI_TLV_PDEV_SET_PARAM_CMDID, WMI_TLV_TAG_STRUCT_PDEV_SET_PARAM_CMD, WMI_PDEV_SET_PARAM_LEN);
	put32(c + 4, id);                       /* pdev_id 0 */
	put32(c + 8, value);
	return WMI_HDR_LEN + WMI_TLV_HDR + WMI_PDEV_SET_PARAM_LEN;
}

u32 wmi_pdev_set_rd(u8 *o, u32 rd, u32 rd2g, u32 rd5g, u32 ctl2g, u32 ctl5g, u32 dfs)
{
	(void)dfs;                              /* (not in the TLV form's structure) */
	u8 *c = cmd_start(o, WMI_TLV_PDEV_SET_REGDOMAIN_CMDID, WMI_TLV_TAG_STRUCT_PDEV_SET_REGDOMAIN_CMD, WMI_PDEV_SET_RD_LEN);
	put32(c + 4, rd);
	put32(c + 8, rd2g);
	put32(c + 12, rd5g);
	put32(c + 16, ctl2g);
	put32(c + 20, ctl5g);
	return WMI_HDR_LEN + WMI_TLV_HDR + WMI_PDEV_SET_RD_LEN;
}

/* ath10k_wmi_put_wmi_channel (20 and 40 MHz) */
static void put_channel(u8 *ch, const struct wmi_chan *c)
{
	u32 flags = 0;
	if (c->passive) flags |= WMI_CHAN_FLAG_PASSIVE;
	if (c->ibss) flags |= WMI_CHAN_FLAG_ADHOC_ALLOWED;
	if (c->list) flags |= WMI_CHAN_FLAG_ALLOW_HT | WMI_CHAN_FLAG_ALLOW_VHT;
	if (c->ht40plus) flags |= WMI_CHAN_FLAG_HT40_PLUS;
	if (c->dfs) flags |= WMI_CHAN_FLAG_DFS;
	mzero(ch, WMI_CHANNEL_LEN);
	put32(ch, c->freq);                     /* mhz */
	put32(ch + 4, c->center1 ? c->center1 : c->freq);   /* band_center_freq1 */
	put32(ch + 12, c->mode | flags);        /* the mode in the low byte, the flags around it */
	ch[16] = 0;                             /* min_power */
	ch[17] = c->max_power;
	ch[18] = c->reg_power;
	ch[20] = 0;                             /* antenna_max */
	ch[21] = c->max_power;                  /* max_tx_power */
}

u32 wmi_scan_chan_list(u8 *o, const struct wmi_chan *c, u32 n)
{
	u8 *p = cmd_start(o, WMI_TLV_SCAN_CHAN_LIST_CMDID, WMI_TLV_TAG_STRUCT_SCAN_CHAN_LIST_CMD, 4);
	put32(p, n);
	p += 4;
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_STRUCT, (u16)(n * (WMI_TLV_HDR + WMI_CHANNEL_LEN)));
	p += WMI_TLV_HDR;
	for (u32 i = 0; i < n; i++) {
		wmi_put_tlv(p, WMI_TLV_TAG_STRUCT_CHANNEL, WMI_CHANNEL_LEN);
		put_channel(p + WMI_TLV_HDR, &c[i]);
		p += WMI_TLV_HDR + WMI_CHANNEL_LEN;
	}
	return (u32)(p - o);
}

u32 wmi_start_scan(u8 *o, u32 vdev, u32 scan_id, const u16 *freqs, u32 nfreq, const u8 *ssid, u32 ssid_len,
		   const u8 *ie, u32 ie_len)
{
	u8 *c = cmd_start(o, WMI_TLV_START_SCAN_CMDID, WMI_TLV_TAG_STRUCT_START_SCAN_CMD, WMI_START_SCAN_LEN);
	u32 nssid = ssid ? 1 : 0;
	/* wmi_start_scan_common: ath10k_wmi_start_scan_init's values */
	put32(c, WMI_HOST_SCAN_REQ_ID_PREFIX | scan_id);
	put32(c + 4, WMI_HOST_SCAN_REQUESTOR_ID_PREFIX | 1);
	put32(c + 8, vdev);
	put32(c + 12, WMI_SCAN_PRIORITY_LOW);
	put32(c + 16, WMI_SCAN_EVENT_STARTED | WMI_SCAN_EVENT_COMPLETED | WMI_SCAN_EVENT_BSS_CHANNEL |
		      WMI_SCAN_EVENT_FOREIGN_CHANNEL | WMI_SCAN_EVENT_FOREIGN_CHANNEL_EXIT | WMI_SCAN_EVENT_DEQUEUED);
	put32(c + 20, 50);                      /* dwell_time_active */
	put32(c + 24, 150);                     /* dwell_time_passive */
	put32(c + 28, 50);                      /* min_rest_time */
	put32(c + 32, 500);                     /* max_rest_time */
	put32(c + 48, 20000);                   /* max_scan_time */
	put32(c + 52, 5);                       /* probe_delay */
	u32 flags = WMI_SCAN_CHAN_STAT_EVENT | (nssid ? 0 : 1);   /* WMI_SCAN_FLAG_PASSIVE without an SSID */
	put32(c + 56, flags ^ WMI_SCAN_FILTER_PROBE_REQ);        /* the TLV firmware's inverted flag */
	put32(c + 64, nfreq);
	put32(c + 68, 1);                       /* num_bssids: the broadcast address */
	put32(c + 72, nssid);
	put32(c + 76, ie_len);
	put32(c + 80, 3);                       /* num_probes */
	u8 *p = c + WMI_START_SCAN_LEN;
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_UINT32, (u16)(4 * nfreq));
	p += WMI_TLV_HDR;
	for (u32 i = 0; i < nfreq; i++, p += 4) put32(p, freqs[i]);
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_FIXED_STRUCT, (u16)(WMI_SSID_LEN * nssid));
	p += WMI_TLV_HDR;
	if (nssid) {
		mzero(p, WMI_SSID_LEN);
		put32(p, ssid_len);
		mcopy(p + 4, ssid, MIN(ssid_len, 32u));
		p += WMI_SSID_LEN;
	}
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_FIXED_STRUCT, WMI_MAC_ADDR_LEN);
	p += WMI_TLV_HDR;
	put_mac(p, (const u8 *)"\xff\xff\xff\xff\xff\xff");
	p += WMI_MAC_ADDR_LEN;
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_BYTE, (u16)round4(ie_len));
	p += WMI_TLV_HDR;
	mzero(p, round4(ie_len));
	mcopy(p, ie, ie_len);
	p += round4(ie_len);
	return (u32)(p - o);
}

u32 wmi_vdev_create(u8 *o, u32 vdev, u32 type, u32 subtype, const u8 mac[6])
{
	u8 *c = cmd_start(o, WMI_TLV_VDEV_CREATE_CMDID, WMI_TLV_TAG_STRUCT_VDEV_CREATE_CMD, WMI_VDEV_CREATE_LEN);
	put32(c, vdev);
	put32(c + 4, type);
	put32(c + 8, subtype);
	put_mac(c + 12, mac);
	return WMI_HDR_LEN + WMI_TLV_HDR + WMI_VDEV_CREATE_LEN;
}

u32 wmi_vdev_start(u8 *o, u32 vdev, u32 bcn_int, u32 dtim, const struct wmi_chan *ch)
{
	u8 *c = cmd_start(o, WMI_TLV_VDEV_START_REQUEST_CMDID, WMI_TLV_TAG_STRUCT_VDEV_START_REQUEST_CMD, WMI_VDEV_START_LEN);
	put32(c, vdev);
	put32(c + 8, bcn_int);
	put32(c + 12, dtim);
	/* flags 0, no SSID (a station's), rate, power, no NoA, hw ack on */
	u8 *p = c + WMI_VDEV_START_LEN;
	wmi_put_tlv(p, WMI_TLV_TAG_STRUCT_CHANNEL, WMI_CHANNEL_LEN);
	put_channel(p + WMI_TLV_HDR, ch);
	p += WMI_TLV_HDR + WMI_CHANNEL_LEN;
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_STRUCT, 0);        /* the NoA descriptors: none */
	p += WMI_TLV_HDR;
	return (u32)(p - o);
}

u32 wmi_vdev_up(u8 *o, u32 vdev, u32 aid, const u8 bssid[6])
{
	u8 *c = cmd_start(o, WMI_TLV_VDEV_UP_CMDID, WMI_TLV_TAG_STRUCT_VDEV_UP_CMD, WMI_VDEV_UP_LEN);
	put32(c, vdev);
	put32(c + 4, aid);
	put_mac(c + 8, bssid);
	return WMI_HDR_LEN + WMI_TLV_HDR + WMI_VDEV_UP_LEN;
}

u32 wmi_vdev_set_param(u8 *o, u32 vdev, u32 id, u32 value)
{
	u8 *c = cmd_start(o, WMI_TLV_VDEV_SET_PARAM_CMDID, WMI_TLV_TAG_STRUCT_VDEV_SET_PARAM_CMD, WMI_VDEV_SET_PARAM_LEN);
	put32(c, vdev);
	put32(c + 4, id);
	put32(c + 8, value);
	return WMI_HDR_LEN + WMI_TLV_HDR + WMI_VDEV_SET_PARAM_LEN;
}

u32 wmi_install_key(u8 *o, u32 vdev, const u8 mac[6], u32 idx, u32 flags, u32 cipher, const u8 *key, u32 len)
{
	u8 *c = cmd_start(o, WMI_TLV_VDEV_INSTALL_KEY_CMDID, WMI_TLV_TAG_STRUCT_VDEV_INSTALL_KEY_CMD, WMI_INSTALL_KEY_LEN);
	put32(c, vdev);
	put_mac(c + 4, mac);
	put32(c + 12, idx);
	put32(c + 16, flags);
	put32(c + 20, cipher);
	put32(c + 80, len);
	/* key_txmic_len, key_rxmic_len: 0 for CCMP */
	u8 *p = c + WMI_INSTALL_KEY_LEN;
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_BYTE, (u16)round4(len));
	p += WMI_TLV_HDR;
	mzero(p, round4(len));
	if (key) mcopy(p, key, len);
	p += round4(len);
	return (u32)(p - o);
}

u32 wmi_vdev_down(u8 *o, u32 vdev)
{
	u8 *c = cmd_start(o, WMI_TLV_VDEV_DOWN_CMDID, WMI_TLV_TAG_STRUCT_VDEV_DOWN_CMD, 4);
	put32(c, vdev);
	return WMI_HDR_LEN + WMI_TLV_HDR + 4;
}

u32 wmi_vdev_stop(u8 *o, u32 vdev)
{
	u8 *c = cmd_start(o, WMI_TLV_VDEV_STOP_CMDID, WMI_TLV_TAG_STRUCT_VDEV_STOP_CMD, 4);
	put32(c, vdev);
	return WMI_HDR_LEN + WMI_TLV_HDR + 4;
}

u32 wmi_peer_delete(u8 *o, u32 vdev, const u8 mac[6])
{
	u8 *c = cmd_start(o, WMI_TLV_PEER_DELETE_CMDID, WMI_TLV_TAG_STRUCT_PEER_DELETE_CMD, WMI_PEER_DELETE_LEN);
	put32(c, vdev);
	put_mac(c + 4, mac);
	return WMI_HDR_LEN + WMI_TLV_HDR + WMI_PEER_DELETE_LEN;
}

u32 wmi_peer_create(u8 *o, u32 vdev, const u8 mac[6], u32 type)
{
	u8 *c = cmd_start(o, WMI_TLV_PEER_CREATE_CMDID, WMI_TLV_TAG_STRUCT_PEER_CREATE_CMD, WMI_PEER_CREATE_LEN);
	put32(c, vdev);
	put_mac(c + 4, mac);
	put32(c + 12, type);
	return WMI_HDR_LEN + WMI_TLV_HDR + WMI_PEER_CREATE_LEN;
}

u32 wmi_peer_set_param(u8 *o, u32 vdev, const u8 mac[6], u32 id, u32 value)
{
	u8 *c = cmd_start(o, WMI_TLV_PEER_SET_PARAM_CMDID, WMI_TLV_TAG_STRUCT_PEER_SET_PARAM_CMD, WMI_PEER_SET_PARAM_LEN);
	put32(c, vdev);
	put_mac(c + 4, mac);
	put32(c + 12, id);
	put32(c + 16, value);
	return WMI_HDR_LEN + WMI_TLV_HDR + WMI_PEER_SET_PARAM_LEN;
}

u32 wmi_peer_assoc(u8 *o, const struct wmi_peer_assoc_arg *a)
{
	u8 *c = cmd_start(o, WMI_TLV_PEER_ASSOC_CMDID, WMI_TLV_TAG_STRUCT_PEER_ASSOC_COMPLETE_CMD, WMI_PEER_ASSOC_LEN);
	put_mac(c, a->mac);
	put32(c + 8, a->vdev);
	put32(c + 12, 1);                       /* new_assoc */
	put32(c + 16, a->aid);
	put32(c + 20, a->flags);
	put32(c + 24, a->caps);
	put32(c + 28, a->listen);
	put32(c + 32, a->ht_caps);
	put32(c + 36, a->max_mpdu);
	put32(c + 40, a->mpdu_density);
	put32(c + 44, a->rate_caps);
	put32(c + 48, a->nss);
	put32(c + 56, a->phymode);              /* (vht_caps at 52, ht_info at 60: 0, as Linux) */
	put32(c + 68, a->nrates);
	put32(c + 72, a->n_ht);
	u8 *p = c + WMI_PEER_ASSOC_LEN;
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_BYTE, (u16)round4(a->nrates));
	p += WMI_TLV_HDR;
	mzero(p, round4(a->nrates));
	mcopy(p, a->rates, a->nrates);
	p += round4(a->nrates);
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_BYTE, (u16)round4(a->n_ht));
	p += WMI_TLV_HDR;
	mzero(p, round4(a->n_ht));
	mcopy(p, a->ht_rates, a->n_ht);
	p += round4(a->n_ht);
	wmi_put_tlv(p, WMI_TLV_TAG_STRUCT_VHT_RATE_SET, WMI_VHT_RATE_SET_LEN);
	mzero(p + WMI_TLV_HDR, WMI_VHT_RATE_SET_LEN);
	p += WMI_TLV_HDR + WMI_VHT_RATE_SET_LEN;
	return (u32)(p - o);
}

u32 wmi_vdev_wmm(u8 *o, u32 vdev, const struct wmi_wmm_ac ac[4])
{
	u8 *c = cmd_start(o, WMI_TLV_VDEV_SET_WMM_PARAMS_CMDID, WMI_TLV_TAG_STRUCT_VDEV_SET_WMM_PARAMS_CMD, WMI_VDEV_WMM_LEN);
	put32(c, vdev);
	for (int i = 0; i < 4; i++) {
		/* wmi_tlv_vdev_wmm_params: a dummy word, then wmi_wmm_params
		 * (cwmin, cwmax, aifs, txop, acm, no_ack: ath10k_wmi_set_wmm_param) */
		u8 *w = c + 4 + 28 * i + 4;
		put32(w, ac[i].cwmin);
		put32(w + 4, ac[i].cwmax);
		put32(w + 8, ac[i].aifs);
		put32(w + 12, ac[i].txop);
		put32(w + 16, ac[i].acm);
		put32(w + 20, 0);
	}
	return WMI_HDR_LEN + WMI_TLV_HDR + WMI_VDEV_WMM_LEN;
}

u32 wmi_uapsd_auto_trig(u8 *o, u32 vdev, const u8 peer[6], u32 ac, u32 prio)
{
	u8 *c = cmd_start(o, WMI_TLV_STA_UAPSD_AUTO_TRIG_CMDID, WMI_TLV_TAG_STRUCT_STA_UAPSD_AUTO_TRIG_CMD, WMI_UAPSD_TRIG_LEN);
	put32(c, vdev);
	put_mac(c + 4, peer);
	put32(c + 12, 1);                       /* num_ac */
	u8 *p = c + WMI_UAPSD_TRIG_LEN;
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_STRUCT, WMI_TLV_HDR + WMI_UAPSD_TRIG_AC_LEN);
	p += WMI_TLV_HDR;
	wmi_put_tlv(p, WMI_TLV_TAG_STRUCT_STA_UAPSD_AUTO_TRIG_PARAM, WMI_UAPSD_TRIG_AC_LEN);
	p += WMI_TLV_HDR;
	put32(p, ac);                           /* wmm_ac */
	put32(p + 4, prio);                     /* user_priority */
	put32(p + 8, 0);                        /* service_interval: the auto trigger off */
	put32(p + 12, WMI_STA_UAPSD_MAX_INTERVAL_MSEC);   /* suspend_interval */
	put32(p + 16, WMI_STA_UAPSD_MAX_INTERVAL_MSEC);   /* delay_interval */
	p += WMI_UAPSD_TRIG_AC_LEN;
	return (u32)(p - o);
}

u32 wmi_sta_ps_mode(u8 *o, u32 vdev, u32 mode)
{
	u8 *c = cmd_start(o, WMI_TLV_STA_POWERSAVE_MODE_CMDID, WMI_TLV_TAG_STRUCT_STA_POWERSAVE_MODE_CMD, WMI_STA_PS_MODE_LEN);
	put32(c, vdev);
	put32(c + 4, mode);
	return WMI_HDR_LEN + WMI_TLV_HDR + WMI_STA_PS_MODE_LEN;
}

u32 wmi_sta_ps_param(u8 *o, u32 vdev, u32 id, u32 value)
{
	u8 *c = cmd_start(o, WMI_TLV_STA_POWERSAVE_PARAM_CMDID, WMI_TLV_TAG_STRUCT_STA_POWERSAVE_PARAM_CMD, WMI_STA_PS_PARAM_LEN);
	put32(c, vdev);
	put32(c + 4, id);
	put32(c + 8, value);
	return WMI_HDR_LEN + WMI_TLV_HDR + WMI_STA_PS_PARAM_LEN;
}

u32 wmi_mgmt_tx(u8 *o, u32 vdev, u32 desc_id, u64 pa, const u8 *frame, u32 len)
{
	u8 *c = cmd_start(o, WMI_TLV_MGMT_TX_SEND_CMD, WMI_TLV_TAG_STRUCT_MGMT_TX_CMD, WMI_MGMT_TX_LEN);
	u32 buf = round4(MIN(len, (u32)WMI_MGMT_TX_FRAME_MAX));
	put32(c, vdev);
	put32(c + 4, desc_id);
	put32(c + 8, 0);                        /* chanfreq: the vdev's */
	put64(c + 12, pa);
	put32(c + 20, len);                     /* frame_len */
	put32(c + 24, buf);                     /* buf_len */
	u8 *p = c + WMI_MGMT_TX_LEN;
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_BYTE, (u16)buf);
	p += WMI_TLV_HDR;
	mzero(p, buf);
	mcopy(p, frame, MIN(len, buf));
	p += buf;
	return (u32)(p - o);
}

/* --- events ------------------------------------------------------------------ */

struct find { u16 tag; const u8 *v; u16 len; const u8 *bytes; u16 blen; };

static int find_tlv(u16 tag, const u8 *v, u16 len, void *arg)
{
	struct find *f = arg;
	if (tag == f->tag && !f->v) f->v = v, f->len = len;
	if (tag == WMI_TLV_TAG_ARRAY_BYTE && !f->bytes) f->bytes = v, f->blen = len;
	return 0;
}

static const u8 *find(const u8 *p, u32 len, u16 tag, struct find *f)
{
	mzero(f, sizeof(*f));
	f->tag = tag;
	if (wmi_tlv_each(p, len, find_tlv, f)) return NULL;
	return f->v;
}

int wmi_scan_event(const u8 *p, u32 len, struct wmi_scan_ev *e)
{
	struct find f;
	const u8 *v = find(p, len, WMI_TLV_TAG_STRUCT_SCAN_EVENT, &f);
	if (!v) return -1;
	e->type = get32(v);
	e->reason = get32(v + 4);
	e->freq = get32(v + 8);
	e->req_id = get32(v + 12);
	e->scan_id = get32(v + 16);
	e->vdev = get32(v + 20);
	return 0;
}

int wmi_mgmt_rx(const u8 *p, u32 len, struct wmi_mgmt_rx *e)
{
	struct find f;
	const u8 *v = find(p, len, WMI_TLV_TAG_STRUCT_MGMT_RX_HDR, &f);
	if (!v || !f.bytes) return -1;
	e->channel = get32(v);
	e->snr = get32(v + 4);
	e->rate = get32(v + 8);
	e->phy_mode = get32(v + 12);
	e->len = get32(v + 16);
	e->status = get32(v + 20);
	e->frame = f.bytes;
	return e->len <= f.blen ? 0 : -1;
}

int wmi_vdev_start_resp(const u8 *p, u32 len, struct wmi_vdev_start_resp *e)
{
	struct find f;
	const u8 *v = find(p, len, WMI_TLV_TAG_STRUCT_VDEV_START_RESPONSE_EVENT, &f);
	if (!v) return -1;
	e->vdev = get32(v);
	e->req_id = get32(v + 4);
	e->resp_type = get32(v + 8);
	e->status = get32(v + 12);
	return 0;
}

struct bundle { const u8 *num, *ids, *status; u16 nids, nstatus; };

static int bundle_tlv(u16 tag, const u8 *v, u16 len, void *arg)
{
	struct bundle *b = arg;
	if (tag == WMI_TLV_TAG_STRUCT_MGMT_TX_COMPL_BUNDLE_EVENT && len >= 4) b->num = v;
	else if (tag == WMI_TLV_TAG_ARRAY_UINT32) {
		/* (ath10k_wmi_tlv_mgmt_tx_bundle_compl_parse: the ids, the
		 * statuses, then the PPDU ids and the ACK RSSIs) */
		if (!b->ids) b->ids = v, b->nids = len / 4;
		else if (!b->status) b->status = v, b->nstatus = len / 4;
	}
	return 0;
}

int wmi_mgmt_tx_compl(const u8 *p, u32 len, bool bundle, void (*fn)(u32 id, u32 status, void *arg), void *arg)
{
	if (!bundle) {
		struct find f;
		const u8 *v = find(p, len, WMI_TLV_TAG_STRUCT_MGMT_TX_COMPL_EVENT, &f);
		if (!v || f.len < 8) return -1;
		fn(get32(v), get32(v + 4), arg);
		return 0;
	}
	struct bundle b = { 0 };
	if (wmi_tlv_each(p, len, bundle_tlv, &b) || !b.num || !b.ids || !b.status) return -1;
	u32 n = get32(b.num);
	if (n > b.nids || n > b.nstatus) return -1;
	for (u32 i = 0; i < n; i++) fn(get32(b.ids + 4 * i), get32(b.status + 4 * i), arg);
	return 0;
}

int wmi_peer_kickout(const u8 *p, u32 len, u8 mac[6])
{
	struct find f;
	const u8 *v = find(p, len, WMI_TLV_TAG_STRUCT_PEER_STA_KICKOUT_EVENT, &f);
	if (!v) return -1;
	mcopy(mac, v, 6);
	return 0;
}
