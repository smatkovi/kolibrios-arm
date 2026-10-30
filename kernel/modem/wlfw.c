/* KolibriOS for ARM: the WLAN firmware's QMI service (wlfw.h), as
 * Linux's ath10k (drivers/net/wireless/ath/ath10k/qmi.c) begins with it:
 * the indication register (client id, FW_READY and MSA_READY wanted; the
 * answer says whether the firmware is ready), then its capabilities. */
#include "wlfw.h"
#include "ce.h"

static struct qrtr *qr;
static struct wlfw_info wi;
static u16 txn;
static struct qmi_buf qb;
static void (*ready_cb)(void);
static void (*enable_cb)(int err);

void wlfw_on_ready(void (*cb)(void))
{
	ready_cb = cb;
	if (cb && wi.registered && wi.fw_ready && wi.have_cap) cb();
}

const struct wlfw_info *wlfw_info(void)
{
	return &wi;
}

static void send(void)
{
	u32 n = qmi_end(&qb);
	qrtr_send(qr, WLFW_PORT, wi.node, wi.port, qb.d, n);
}

static void ask_caps(void)
{
	qmi_begin(&qb, QMI_REQUEST, ++txn, WLFW_CAP);
	send();
}

/* a string in a structure: a length byte, then the characters */
static void get_str(const u8 *v, u32 n, char *out, u32 size)
{
	u32 l = n ? MIN((u32)v[0], n - 1) : 0;
	l = MIN(l, size - 1);
	mcopy(out, v + 1, l);
	out[l] = 0;
}

static void rx(void *arg, u32 node, u32 port, const u8 *d, u32 len)
{
	struct qmi_msg m;
	(void)arg;
	if (!qmi_parse(d, len, &m) || node != wi.node || port != wi.port) return;
	u32 n;
	const u8 *v;
	u16 err = 0;
	if (m.type == QMI_INDICATION) {
		wi.indications++;
		if (m.id == WLFW_FW_READY_IND) {
			wi.fw_ready = true;
			mlog("wlfw: the firmware is ready (indication)\n");
		} else if (m.id == WLFW_MSA_READY_IND) {
			mlog("wlfw: MSA ready (indication)\n");
		} else {
			mlog("wlfw: indication %x\n", m.id);
		}
		return;
	}
	if (m.type != QMI_RESPONSE) return;
	switch (m.id) {
	case WLFW_IND_REGISTER:
		if (!qmi_ok(&m, &err)) {
			mlog("wlfw: indication register refused (error %u)\n", err);
			wi.failed = true;
			return;
		}
		wi.registered = true;
		v = qmi_tlv(&m, 0x10, &n);
		if (v && n >= 8 && (get64(v) & WLFW_FW_READY)) wi.fw_ready = true;
		mlog("wlfw: registered, the firmware %s\n", wi.fw_ready ? "is ready" : "is not ready yet");
		ask_caps();
		return;
	case WLFW_CAP:
		if (!qmi_ok(&m, &err)) {
			mlog("wlfw: capabilities refused (error %u)\n", err);
			wi.failed = true;
			return;
		}
		if ((v = qmi_tlv(&m, 0x10, &n)) && n >= 8) wi.chip_id = get32(v), wi.chip_family = get32(v + 4);
		if ((v = qmi_tlv(&m, 0x11, &n)) && n >= 4) wi.board_id = get32(v);
		if ((v = qmi_tlv(&m, 0x12, &n)) && n >= 4) wi.soc_id = get32(v);
		if ((v = qmi_tlv(&m, 0x13, &n)) && n >= 4) {
			wi.fw_version = get32(v);
			get_str(v + 4, n - 4, wi.timestamp, sizeof(wi.timestamp));
		}
		if ((v = qmi_tlv(&m, 0x14, &n))) {
			u32 l = MIN(n, (u32)sizeof(wi.build) - 1);
			mcopy(wi.build, v, l);
			wi.build[l] = 0;
		}
		wi.have_cap = true;
		mlog("wlfw: chip %x family %x board %x soc %x, firmware %x (%s) %s\n", wi.chip_id,
		     wi.chip_family, wi.board_id, wi.soc_id, wi.fw_version, wi.timestamp, wi.build);
		/* ath10k_qmi_event_server_arrive: with the firmware ready
		 * (after a kexec from Linux) straight on to the driver
		 * (ATH10K_QMI_EVENT_FW_READY_IND); else Linux's full path
		 * (host capabilities, MSA, the board data from board-2.bin),
		 * which is not done here */
		if (!wi.fw_ready) mlog("wlfw: the firmware is not ready: the board data would be needed (not done)\n");
		else if (ready_cb) ready_cb();
		return;
	case WLFW_WLAN_CFG:
		if (!qmi_ok(&m, &err)) {
			mlog("wlfw: WLAN_CFG refused (error %u)\n", err);
			if (enable_cb) enable_cb(-1);
			return;
		}
		mlog("wlfw: WLAN_CFG taken\n");
		wlfw_mode_msg(&qb, ++txn, WLFW_MODE_MISSION);
		send();
		return;
	case WLFW_WLAN_MODE:
		if (!qmi_ok(&m, &err)) mlog("wlfw: WLAN_MODE refused (error %u)\n", err);
		else mlog("wlfw: WLAN_MODE mission\n");
		if (enable_cb) enable_cb(qmi_ok(&m, &err) ? 0 : -1);
		return;
	}
}

/* the QMI_WLFW_MAX_NUM_* limits of qmi_wlfw_v01.h */
#define MAX_CE 12
#define MAX_SVC 24
#define MAX_SHADOW 24

void wlfw_cfg_msg(struct qmi_buf *b, u16 txn, const struct ce_pipe_cfg *tgt, u32 ntgt,
		  const struct ce_svc_pipe *svc, u32 nsvc, const struct ce_shadow_cfg *sh, u32 nsh)
{
	u8 v[1 + MAX_SVC * 12];
	ntgt = MIN(ntgt, (u32)MAX_CE);
	nsvc = MIN(nsvc, (u32)MAX_SVC);
	nsh = MIN(nsh, (u32)MAX_SHADOW);
	qmi_begin(b, QMI_REQUEST, txn, WLFW_WLAN_CFG);
	v[0] = (u8)ntgt;
	for (u32 i = 0; i < ntgt; i++) {
		u8 *e = v + 1 + 20 * i;               /* wlfw_ce_tgt_pipe_cfg_s_v01: no reserved word */
		put32(e, tgt[i].pipenum);
		put32(e + 4, tgt[i].pipedir);
		put32(e + 8, tgt[i].nentries);
		put32(e + 12, tgt[i].nbytes_max);
		put32(e + 16, tgt[i].flags);
	}
	qmi_put(b, 0x11, v, 1 + 20 * ntgt);
	v[0] = (u8)nsvc;
	for (u32 i = 0; i < nsvc; i++) {
		u8 *e = v + 1 + 12 * i;
		put32(e, svc[i].service_id);
		put32(e + 4, svc[i].pipedir);
		put32(e + 8, svc[i].pipenum);
	}
	qmi_put(b, 0x12, v, 1 + 12 * nsvc);
	v[0] = (u8)nsh;
	for (u32 i = 0; i < nsh; i++) {
		put16(v + 1 + 4 * i, sh[i].ce_id);
		put16(v + 1 + 4 * i + 2, sh[i].reg_offset);
	}
	qmi_put(b, 0x13, v, 1 + 4 * nsh);
}

void wlfw_mode_msg(struct qmi_buf *b, u16 txn, u32 mode)
{
	qmi_begin(b, QMI_REQUEST, txn, WLFW_WLAN_MODE);
	qmi_put_u32(b, 0x01, mode);
}

/* ath10k_snoc_wlan_enable -> ath10k_qmi_wlan_enable: WLAN_CFG, then
 * WLAN_MODE (mission); done(0) when both are taken */
void wlfw_wlan_enable(void (*done)(int err))
{
	enable_cb = done;
	if (!qr || !wi.found) {
		if (done) done(-1);
		return;
	}
	wlfw_cfg_msg(&qb, ++txn, ce_target_cfg, CE_COUNT, ce_svc_map, ce_svc_map_n, ce_shadow_map, ce_shadow_map_n);
	send();
}

void wlfw_init(struct qrtr *q)
{
	qr = q;
	mzero(&wi, sizeof(wi));
	qrtr_bind(q, WLFW_PORT, rx, 0);
}

void wlfw_server(u32 node, u32 port)
{
	if (!qr || wi.found) return;
	wi.found = true;
	wi.node = node;
	wi.port = port;
	mlog("wlfw: the WLAN firmware's service at %u:%u\n", node, port);
	qmi_begin(&qb, QMI_REQUEST, ++txn, WLFW_IND_REGISTER);
	qmi_put_u8(&qb, 0x10, 1);              /* fw_ready_enable */
	qmi_put_u8(&qb, 0x13, 1);              /* msa_ready_enable */
	qmi_put_u32(&qb, 0x15, WLFW_CLIENT_ID);
	send();
}
