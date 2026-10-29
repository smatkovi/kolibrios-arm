/* KolibriOS for ARM: the WLAN firmware's QMI service (WLFW, 69) on the
 * modem: the first steps of the Pro1X's Wi-Fi (docs/pro1x-network.md).
 * After a kexec the firmware is loaded and ready from Linux's time; this
 * registers for its indications and asks what it is. Nothing is switched
 * on yet: WLAN_CFG and WLAN_MODE are encoded (wlfw_cfg_msg, wlfw_mode_msg)
 * but not sent. */
#ifndef WLFW_H
#define WLFW_H
#include "qmi.h"

#define WLFW_SVC 69
#define WLFW_PORT 0x4100              /* ours, for its answers */

enum { WLFW_IND_REGISTER = 0x20, WLFW_FW_READY_IND = 0x21, WLFW_WLAN_MODE = 0x22,
       WLFW_WLAN_CFG = 0x23, WLFW_CAP = 0x24, WLFW_MSA_READY_IND = 0x2b };
#define WLFW_CLIENT_ID 0x4b4e454c     /* ath10k's */
#define WLFW_FW_READY 2ull            /* the indication register's fw_status */

struct wlfw_info {
	bool found, registered, fw_ready, have_cap, failed;
	u32 node, port;
	u32 chip_id, chip_family, board_id, soc_id, fw_version;
	char timestamp[36], build[132];
	u32 indications;
};

/* WLAN_CFG's request (QMI_WLFW_WLAN_CFG_REQ_V01, as ath10k's
 * ath10k_qmi_cfg_send_sync_msg: no host version; 0x11 the target's pipes,
 * five words each; 0x12 the services' pipes, three words each; 0x13 the
 * shadow registers, two halfwords each; each array after a count byte),
 * and WLAN_MODE's (0x01 the mode: 0 mission, 4 off). From ce.h's tables:
 * ce_target_cfg, ce_svc_map, ce_shadow_map */
struct ce_pipe_cfg;
struct ce_svc_pipe;
struct ce_shadow_cfg;
#define WLFW_MODE_MISSION 0
#define WLFW_MODE_OFF 4
void wlfw_cfg_msg(struct qmi_buf *b, u16 txn, const struct ce_pipe_cfg *tgt, u32 ntgt,
		  const struct ce_svc_pipe *svc, u32 nsvc, const struct ce_shadow_cfg *sh, u32 nsh);
void wlfw_mode_msg(struct qmi_buf *b, u16 txn, u32 mode);

void wlfw_init(struct qrtr *q);
void wlfw_server(u32 node, u32 port);  /* the service appeared */
const struct wlfw_info *wlfw_info(void);

#endif
