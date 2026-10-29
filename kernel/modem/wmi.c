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
