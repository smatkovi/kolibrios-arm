/* KolibriOS for ARM: WMI-TLV, the WLAN firmware's control messages over
 * HTC (wmi.c; docs/pro1x-network.md, stage 4). After Linux's ath10k
 * wmi-tlv.h and wmi-tlv.c (the WCN3990's firmware speaks the TLV flavour,
 * ATH10K_FW_WMI_OP_VERSION_TLV): a command or event is a 4-byte header
 * (its id in bits 0-23) and a sequence of TLVs, each a little-endian
 * 16-bit length (of the value) and 16-bit tag, then the value. The first
 * steps: the target's SERVICE_READY (its capabilities, the service bitmap,
 * the host memory it wants), the host's INIT (the resource configuration,
 * that memory), the target's READY (the MAC address). */
#ifndef WMI_H
#define WMI_H
#include "modem.h"

#define WMI_HDR_LEN 4
#define WMI_ID_MASK 0x00ffffffu
#define WMI_TLV_HDR 4

/* ids (enum wmi_tlv_cmd_id, wmi_tlv_event_id: group << 12 | n). These
 * and the tags, structure sizes and offsets below are Linux 7.1's
 * wmi-tlv.h and wmi.h, read out by compiling them on the PC */
#define WMI_TLV_INIT_CMDID 0x0001
#define WMI_TLV_START_SCAN_CMDID 0x3001
#define WMI_TLV_STOP_SCAN_CMDID 0x3002
#define WMI_TLV_SCAN_CHAN_LIST_CMDID 0x3003
#define WMI_TLV_PDEV_SET_REGDOMAIN_CMDID 0x4001
#define WMI_TLV_PDEV_SET_PARAM_CMDID 0x4003
#define WMI_TLV_VDEV_CREATE_CMDID 0x5001
#define WMI_TLV_VDEV_START_REQUEST_CMDID 0x5003
#define WMI_TLV_VDEV_UP_CMDID 0x5005
#define WMI_TLV_VDEV_STOP_CMDID 0x5006
#define WMI_TLV_VDEV_DOWN_CMDID 0x5007
#define WMI_TLV_VDEV_SET_PARAM_CMDID 0x5008
#define WMI_TLV_VDEV_INSTALL_KEY_CMDID 0x5009
#define WMI_TLV_PEER_CREATE_CMDID 0x6001
#define WMI_TLV_PEER_DELETE_CMDID 0x6002
#define WMI_TLV_PEER_SET_PARAM_CMDID 0x6004
#define WMI_TLV_PEER_ASSOC_CMDID 0x6005
#define WMI_TLV_MGMT_TX_SEND_CMD 0x7008
#define WMI_TLV_STA_POWERSAVE_MODE_CMDID 0x9001
#define WMI_TLV_STA_POWERSAVE_PARAM_CMDID 0x9002
#define WMI_TLV_SERVICE_READY_EVENTID 0x0001
#define WMI_TLV_READY_EVENTID 0x0002
#define WMI_TLV_SERVICE_AVAILABLE_EVENTID 0x0003
#define WMI_TLV_SCAN_EVENTID 0x3001
#define WMI_TLV_CHAN_INFO_EVENTID 0x4002
#define WMI_TLV_TX_PAUSE_EVENTID 0x4005
#define WMI_TLV_PDEV_TEMPERATURE_EVENTID 0x4008
#define WMI_TLV_VDEV_START_RESP_EVENTID 0x5001
#define WMI_TLV_VDEV_STOPPED_EVENTID 0x5002
#define WMI_TLV_PEER_STA_KICKOUT_EVENTID 0x6001
#define WMI_TLV_PEER_ASSOC_CONF_EVENTID 0x6006
#define WMI_TLV_MGMT_RX_EVENTID 0x7001
#define WMI_TLV_MGMT_TX_COMPLETION_EVENTID 0x7006
#define WMI_TLV_MGMT_TX_BUNDLE_COMPLETION_EVENTID 0x7007
#define WMI_TLV_ROAM_EVENTID 0xb001
#define WMI_TLV_DEBUG_MESG_EVENTID 0x1d003

/* tags (enum wmi_tlv_tag) */
#define WMI_TLV_TAG_ARRAY_UINT32 16
#define WMI_TLV_TAG_ARRAY_BYTE 17
#define WMI_TLV_TAG_ARRAY_STRUCT 18
#define WMI_TLV_TAG_ARRAY_FIXED_STRUCT 19
#define WMI_TLV_TAG_STRUCT_SERVICE_READY_EVENT 32
#define WMI_TLV_TAG_STRUCT_SCAN_EVENT 36
#define WMI_TLV_TAG_STRUCT_VDEV_START_RESPONSE_EVENT 40
#define WMI_TLV_TAG_STRUCT_PEER_STA_KICKOUT_EVENT 43
#define WMI_TLV_TAG_STRUCT_MGMT_RX_HDR 44
#define WMI_TLV_TAG_STRUCT_ROAM_EVENT 48
#define WMI_TLV_TAG_STRUCT_START_SCAN_CMD 77
#define WMI_TLV_TAG_STRUCT_SCAN_CHAN_LIST_CMD 79
#define WMI_TLV_TAG_STRUCT_CHANNEL 80
#define WMI_TLV_TAG_STRUCT_PDEV_SET_REGDOMAIN_CMD 81
#define WMI_TLV_TAG_STRUCT_PDEV_SET_PARAM_CMD 82
#define WMI_TLV_TAG_STRUCT_VDEV_CREATE_CMD 86
#define WMI_TLV_TAG_STRUCT_VDEV_START_REQUEST_CMD 88
#define WMI_TLV_TAG_STRUCT_VDEV_UP_CMD 92
#define WMI_TLV_TAG_STRUCT_VDEV_STOP_CMD 93
#define WMI_TLV_TAG_STRUCT_VDEV_DOWN_CMD 94
#define WMI_TLV_TAG_STRUCT_VDEV_SET_PARAM_CMD 95
#define WMI_TLV_TAG_STRUCT_VDEV_INSTALL_KEY_CMD 96
#define WMI_TLV_TAG_STRUCT_PEER_CREATE_CMD 97
#define WMI_TLV_TAG_STRUCT_PEER_DELETE_CMD 98
#define WMI_TLV_TAG_STRUCT_PEER_SET_PARAM_CMD 100
#define WMI_TLV_TAG_STRUCT_PEER_ASSOC_COMPLETE_CMD 101
#define WMI_TLV_TAG_STRUCT_VHT_RATE_SET 102
#define WMI_TLV_TAG_STRUCT_STA_POWERSAVE_MODE_CMD 108
#define WMI_TLV_TAG_STRUCT_STA_POWERSAVE_PARAM_CMD 109
#define WMI_TLV_TAG_STRUCT_MGMT_TX_CMD 422
#define WMI_TLV_TAG_STRUCT_MGMT_TX_COMPL_EVENT 423
#define WMI_TLV_TAG_STRUCT_MGMT_TX_COMPL_BUNDLE_EVENT 552
#define WMI_TLV_TAG_STRUCT_HAL_REG_CAPABILITIES 33
#define WMI_TLV_TAG_STRUCT_WLAN_HOST_MEM_REQ 34
#define WMI_TLV_TAG_STRUCT_READY_EVENT 35
#define WMI_TLV_TAG_STRUCT_INIT_CMD 74
#define WMI_TLV_TAG_STRUCT_RESOURCE_CONFIG 75
#define WMI_TLV_TAG_STRUCT_WLAN_HOST_MEMORY_CHUNK 76

/* the commands' structures */
#define WMI_PDEV_SET_PARAM_LEN 12       /* wmi_tlv_pdev_set_param_cmd: pdev, id, value */
#define WMI_PDEV_SET_RD_LEN 24          /* wmi_tlv_pdev_set_rd_cmd */
#define WMI_CHANNEL_LEN 24              /* wmi_channel */
#define WMI_START_SCAN_LEN 100          /* wmi_tlv_start_scan_cmd (wmi_start_scan_common 60) */
#define WMI_SSID_LEN 36                 /* wmi_ssid: length, 32 */
#define WMI_MAC_ADDR_LEN 8              /* wmi_mac_addr: 6 and 2 of padding */
#define WMI_VDEV_CREATE_LEN 20
#define WMI_VDEV_START_LEN 72           /* wmi_tlv_vdev_start_cmd */
#define WMI_VDEV_UP_LEN 16
#define WMI_VDEV_SET_PARAM_LEN 12
#define WMI_INSTALL_KEY_LEN 92          /* wmi_vdev_install_key_cmd */
#define WMI_PEER_CREATE_LEN 16
#define WMI_PEER_DELETE_LEN 12          /* wmi_peer_delete_cmd: vdev, mac */
#define WMI_PEER_ASSOC_LEN 76           /* wmi_tlv_peer_assoc_cmd */
#define WMI_VHT_RATE_SET_LEN 16
#define WMI_PEER_SET_PARAM_LEN 20
#define WMI_MGMT_TX_LEN 28              /* wmi_tlv_mgmt_tx_cmd */
#define WMI_MGMT_TX_FRAME_MAX 64        /* WMI_TLV_MGMT_TX_FRAME_MAX_LEN: the frame's start in the command */
#define WMI_STA_PS_MODE_LEN 8
#define WMI_STA_PS_PARAM_LEN 12
/* the events' (wmi_tlv_policies' min_len) */
#define WMI_SCAN_EVENT_LEN 24
#define WMI_MGMT_RX_LEN 40              /* wmi_tlv_mgmt_rx_ev */
#define WMI_VDEV_START_RESP_LEN 16
#define WMI_MGMT_TX_COMPL_LEN 20
#define WMI_PEER_KICKOUT_LEN 8
#define WMI_ROAM_LEN 12

/* values */
#define WMI_TLV_PDEV_PARAM_DYNAMIC_BW 10
#define WMI_TLV_PDEV_PARAM_PMF_QOS 34
#define WMI_TLV_PDEV_PARAM_ARP_AC_OVERRIDE 35
#define WMI_TLV_PDEV_PARAM_ANI_ENABLE 37
#define WMI_TLV_PDEV_PARAM_IDLE_PS_CONFIG 44
#define WMI_VDEV_TYPE_STA 2
#define WMI_VDEV_SUBTYPE_NONE 0
#define WMI_PEER_TYPE_DEFAULT 0
#define WMI_TLV_PEER_AUTHORIZE 3        /* the peer parameter */
#define WMI_TLV_PEER_AUTH 0x1           /* peer flags */
#define WMI_TLV_PEER_QOS 0x2
#define WMI_TLV_PEER_NEED_PTK_4_WAY 0x4
#define WMI_TLV_CIPHER_NONE 0
#define WMI_TLV_CIPHER_AES_CCM 4
#define WMI_KEY_PAIRWISE 0x0
#define WMI_KEY_GROUP 0x1
#define WMI_STA_PS_MODE_DISABLED 0
#define WMI_STA_PS_PARAM_RX_WAKE_POLICY 0
#define WMI_STA_PS_RX_WAKE_POLICY_WAKE 0
#define MODE_11A 0                      /* enum wmi_phy_mode */
#define MODE_11G 1
#define MODE_11B 2
#define MODE_11NA_HT20 4
#define MODE_11NG_HT20 5
#define MODE_11NA_HT40 6
#define MODE_11NG_HT40 7
#define WMI_CHAN_FLAG_HT40_PLUS 0x40
#define WMI_CHAN_FLAG_ADHOC_ALLOWED 0x100
#define WMI_CHAN_FLAG_ALLOW_HT 0x800
#define WMI_CHAN_FLAG_ALLOW_VHT 0x1000
#define WMI_CHAN_FLAG_PASSIVE 0x80
#define WMI_CHAN_FLAG_DFS 0x400
#define WMI_SCAN_PRIORITY_LOW 1
#define WMI_HOST_SCAN_REQ_ID_PREFIX 0xa000
#define WMI_HOST_SCAN_REQUESTOR_ID_PREFIX 0xa000
#define WMI_SCAN_EVENT_STARTED 0x1
#define WMI_SCAN_EVENT_COMPLETED 0x2
#define WMI_SCAN_EVENT_BSS_CHANNEL 0x4
#define WMI_SCAN_EVENT_FOREIGN_CHANNEL 0x8
#define WMI_SCAN_EVENT_DEQUEUED 0x10
#define WMI_SCAN_EVENT_START_FAILED 0x40
#define WMI_SCAN_EVENT_FOREIGN_CHANNEL_EXIT 0x100
#define WMI_SCAN_CHAN_STAT_EVENT 0x10
#define WMI_SCAN_FILTER_PROBE_REQ 0x20

/* the structures' sizes (their min_len in wmi_tlv_policies) */
#define WMI_ABI_LEN 24                  /* wmi_tlv_abi_version */
#define WMI_SVC_RDY_LEN 104             /* wmi_tlv_svc_rdy_ev */
#define WMI_HAL_REG_LEN 36              /* hal_reg_capabilities */
#define WMI_MEM_REQ_LEN 16              /* wlan_host_mem_req */
#define WMI_RDY_LEN 36                  /* wmi_tlv_rdy_ev */
#define WMI_INIT_LEN 28                 /* wmi_tlv_init_cmd */
#define WMI_RES_CFG_LEN 176             /* wmi_tlv_resource_config: 44 words */
#define WMI_MEM_CHUNK_LEN 16            /* host_memory_chunk_tlv */

/* the ABI both sides check (wmi-tlv.c) */
#define WMI_TLV_ABI_VER0 0x01000000u    /* major 1, minor 0 */
#define WMI_TLV_ABI_VER1 53
#define WMI_TLV_ABI_VER_NS0 0x5f414351u /* "QCA_" */
#define WMI_TLV_ABI_VER_NS1 0x00004c4du /* "ML" */
#define WMI_TLV_ABI_VER_NS2 0
#define WMI_TLV_ABI_VER_NS3 0

#define WMI_MAX_MEM_REQS 16
struct wmi_mem_req {
	u32 req_id, unit_size, num_unit_info, num_units;
};

struct wmi_svc_ready {
	u32 fw_build, abi_ver0, abi_ver1, phy_capability, num_rf_chains, ht_cap, vht_cap, vht_supp_mcs;
	u32 min_tx_power, max_tx_power, sys_cap_info, num_mem_reqs, max_num_scan_chans;
	u32 eeprom_rd, low_2ghz_chan, high_2ghz_chan, low_5ghz_chan, high_5ghz_chan;
	const u8 *svc_map;              /* the service bitmap (in the message), its length */
	u32 svc_map_len;
	int nreqs;
	struct wmi_mem_req req[WMI_MAX_MEM_REQS];
};

struct wmi_ready {
	u32 sw_version, abi_version, status;
	u8 mac[6];
};

/* a host memory chunk for INIT: its request's id, its address, its size
 * (below 4 GiB unless the firmware has WMI_SERVICE_SUPPORT_EXTEND_ADDRESS:
 * Linux sends ptr_high only then) */
struct wmi_mem_chunk {
	u32 req_id, size;
	u64 pa;
};

/* INIT's resource configuration: what ath10k_wmi_tlv_op_gen_init fills in
 * for the WCN3990 (wmi_init_defaults), by name */
struct wmi_res_cfg {
	u32 num_vdevs, num_peers, num_offload_peers, num_offload_reorder_bufs, num_peer_keys, num_tids;
	u32 ast_skid_limit, tx_chain_mask, rx_chain_mask, rx_timeout_pri[4], rx_decap_mode;
	u32 scan_max_pending_reqs, bmiss_offload_max_vdev, roam_offload_max_vdev, roam_offload_max_ap_profiles;
	u32 num_mcast_groups, num_mcast_table_elems, mcast2ucast_mode, tx_dbg_log_size, num_wds_entries;
	u32 dma_burst_size, mac_aggr_delim, rx_skip_defrag_timeout_dup_detection_check, vow_config;
	u32 gtk_offload_max_vdev, num_msdu_desc, max_frag_entries, num_tdls_vdevs, num_tdls_conn_table_entries;
	u32 beacon_tx_offload_max_vdev, num_multicast_filter_entries, num_wow_filters, num_keep_alive_pattern;
	u32 keep_alive_pattern_size, max_tdls_concurrent_sleep_sta, max_tdls_concurrent_buffer_sta;
	u32 wmi_send_separate, num_ocb_vdevs, num_ocb_channels, num_ocb_schedules, host_capab;
};
/* the WCN3990's: hw_params (num_peers 33, ast_skid_limit 16,
 * num_wds_entries 2: TARGET_HL_TLV_*), TARGET_TLV_NUM_VDEVS 4, NATIVE_WIFI
 * decapsulation, TARGET_TLV_NUM_MSDU_DESC (1024 + 32) descriptors;
 * rx_full_reorder (WMI_SERVICE_RX_FULL_REORDER), wow_patterns (0, or 22:
 * TARGET_TLV_NUM_WOW_PATTERNS with WMI_SERVICE_WOW) and ack_rssi
 * (WMI_SERVICE_TX_DATA_ACK_RSSI) from the service bitmap */
void wmi_init_defaults(struct wmi_res_cfg *c, bool rx_full_reorder, u32 wow_patterns, bool ack_rssi);

/* the command's or event's header */
void wmi_put_hdr(u8 *out, u32 id);
u32 wmi_id(const u8 *msg);
/* a TLV's header; the value follows */
void wmi_put_tlv(u8 *out, u16 tag, u16 len);
/* the TLVs of a message (after its header): fn for each (its tag, its
 * value, its length); -1 when one does not fit or is shorter than its
 * tag's structure (ath10k_wmi_tlv_iter's policy check), or fn's error */
int wmi_tlv_each(const u8 *p, u32 len, int (*fn)(u16 tag, const u8 *v, u16 len, void *arg), void *arg);

/* INIT (WMI_TLV_INIT_CMDID): header and TLVs into out (cap bytes): its
 * length, or -1 */
int wmi_init_cmd(u8 *out, u32 cap, const struct wmi_res_cfg *c, const struct wmi_mem_chunk *chunks, int n);
/* the events (after the header): 0, or -1 (missing parts, -2: another ABI) */
int wmi_service_ready(const u8 *p, u32 len, struct wmi_svc_ready *out);
int wmi_ready(const u8 *p, u32 len, struct wmi_ready *out);
/* a service of SERVICE_READY's bitmap (enum wmi_tlv_service: WOW 13,
 * RX_FULL_REORDER 65, ...): as Linux's WMI_SERVICE_IS_ENABLED, four
 * services a 32-bit word (bits 0-3), the number below the bitmap's
 * length in bytes */
#define WMI_TLV_SERVICE_WOW 13
#define WMI_TLV_SERVICE_RX_FULL_REORDER 65
bool wmi_service(const struct wmi_svc_ready *s, u32 id);
#define WMI_TLV_SERVICE_MGMT_TX_WMI 76  /* enum wmi_tlv_service */
#define WMI_TLV_SERVICE_EXT_ADDRESS 179 /* WMI_TLV_SERVICE_SUPPORT_EXTEND_ADDRESS (the extended map) */

/* --- the commands of a station (wmi-tlv.c's ath10k_wmi_tlv_op_gen_*):
 * header and TLVs into o (big enough: see each), their length --------- */

/* a channel (wmi_channel_arg), as ath10k_wmi_put_wmi_channel fills it:
 * the scan channel list's (ath10k_update_channel_list: HT and VHT
 * allowed, IBSS where not NO_IR, passive on NO_IR and radar, HT40+ where
 * the regulatory domain has it, the radar flag), VDEV_START's
 * (ath10k_vdev_start_restart for a station: the mode, the frequencies,
 * the power, no flags) */
struct wmi_chan {
	u16 freq, center1;                  /* the primary's, the whole channel's center (band_center_freq1) */
	u8 mode;                            /* MODE_11A/G/B, MODE_11NA/NG_HT20/HT40 */
	bool list;                          /* a channel list entry: HT and VHT allowed */
	bool passive, dfs, ibss, ht40plus;
	u8 max_power, reg_power;            /* in 0.5 dBm, as Linux's channel max_power * 2 */
};
u32 wmi_pdev_set_param(u8 *o, u32 id, u32 value);
u32 wmi_pdev_set_rd(u8 *o, u32 rd, u32 rd2g, u32 rd5g, u32 ctl2g, u32 ctl5g, u32 dfs);
u32 wmi_scan_chan_list(u8 *o, const struct wmi_chan *c, u32 n);           /* 8 + 28 n */
/* START_SCAN as ath10k_hw_scan with ath10k_wmi_start_scan_init's values:
 * the channels (MHz), one SSID (or none: passive), the probe's elements */
u32 wmi_start_scan(u8 *o, u32 vdev, u32 scan_id, const u16 *freqs, u32 nfreq, const u8 *ssid, u32 ssid_len,
		   const u8 *ie, u32 ie_len);
u32 wmi_vdev_create(u8 *o, u32 vdev, u32 type, u32 subtype, const u8 mac[6]);
u32 wmi_vdev_start(u8 *o, u32 vdev, u32 bcn_int, u32 dtim, const struct wmi_chan *c);
u32 wmi_vdev_up(u8 *o, u32 vdev, u32 aid, const u8 bssid[6]);
u32 wmi_vdev_set_param(u8 *o, u32 vdev, u32 id, u32 value);
u32 wmi_install_key(u8 *o, u32 vdev, const u8 mac[6], u32 idx, u32 flags, u32 cipher, const u8 *key, u32 len);
u32 wmi_vdev_down(u8 *o, u32 vdev);
u32 wmi_vdev_stop(u8 *o, u32 vdev);
u32 wmi_peer_create(u8 *o, u32 vdev, const u8 mac[6], u32 type);
u32 wmi_peer_delete(u8 *o, u32 vdev, const u8 mac[6]);
u32 wmi_peer_set_param(u8 *o, u32 vdev, const u8 mac[6], u32 id, u32 value);
/* PEER_ASSOC for a legacy (no HT/VHT) peer: ath10k_peer_assoc_h_basic,
 * _crypto, _rates, _phymode (rates as ath10k_mac_bitrate_to_rate: 500
 * kb/s, bit 7 for CCK) */
u32 wmi_peer_assoc(u8 *o, u32 vdev, const u8 mac[6], u32 aid, u32 flags, u32 caps, u32 listen, u32 phymode,
		   const u8 *rates, u32 nrates);
u32 wmi_sta_ps_mode(u8 *o, u32 vdev, u32 mode);
u32 wmi_sta_ps_param(u8 *o, u32 vdev, u32 id, u32 value);
/* MGMT_TX_SEND (ATH10K_FW_FEATURE_MGMT_TX_BY_REF): the frame by its
 * address, its first 64 bytes in the command */
u32 wmi_mgmt_tx(u8 *o, u32 vdev, u32 desc_id, u64 pa, const u8 *frame, u32 len);

/* --- events ---------------------------------------------------------- */
struct wmi_scan_ev { u32 type, reason, freq, req_id, scan_id, vdev; };
int wmi_scan_event(const u8 *p, u32 len, struct wmi_scan_ev *e);
struct wmi_mgmt_rx { u32 channel, snr, rate, phy_mode, len, status; const u8 *frame; };
int wmi_mgmt_rx(const u8 *p, u32 len, struct wmi_mgmt_rx *e);
struct wmi_vdev_start_resp { u32 vdev, req_id, resp_type, status; };
int wmi_vdev_start_resp(const u8 *p, u32 len, struct wmi_vdev_start_resp *e);
/* MGMT_TX_COMPLETION (one) or MGMT_TX_BUNDLE_COMPLETION: each report's
 * descriptor id and status to fn */
int wmi_mgmt_tx_compl(const u8 *p, u32 len, bool bundle, void (*fn)(u32 id, u32 status, void *arg), void *arg);
/* PEER_STA_KICKOUT: the peer's address */
int wmi_peer_kickout(const u8 *p, u32 len, u8 mac[6]);

#endif
