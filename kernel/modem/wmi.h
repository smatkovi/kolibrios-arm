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

/* ids (enum wmi_tlv_cmd_id, wmi_tlv_event_id: group << 12 | n) */
#define WMI_TLV_INIT_CMDID 0x0001
#define WMI_TLV_START_SCAN_CMDID 0x3001
#define WMI_TLV_SERVICE_READY_EVENTID 0x0001
#define WMI_TLV_READY_EVENTID 0x0002

/* tags (enum wmi_tlv_tag) */
#define WMI_TLV_TAG_ARRAY_UINT32 16
#define WMI_TLV_TAG_ARRAY_BYTE 17
#define WMI_TLV_TAG_ARRAY_STRUCT 18
#define WMI_TLV_TAG_STRUCT_SERVICE_READY_EVENT 32
#define WMI_TLV_TAG_STRUCT_HAL_REG_CAPABILITIES 33
#define WMI_TLV_TAG_STRUCT_WLAN_HOST_MEM_REQ 34
#define WMI_TLV_TAG_STRUCT_READY_EVENT 35
#define WMI_TLV_TAG_STRUCT_INIT_CMD 74
#define WMI_TLV_TAG_STRUCT_RESOURCE_CONFIG 75
#define WMI_TLV_TAG_STRUCT_WLAN_HOST_MEMORY_CHUNK 76

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

#endif
