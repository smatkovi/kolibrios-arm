/* KolibriOS for ARM: the WLAN firmware's QMI service (wlfw.h), as
 * Linux's ath10k (drivers/net/wireless/ath/ath10k/qmi.c) begins with it:
 * the indication register (client id, FW_READY and MSA_READY wanted; the
 * answer says whether the firmware is ready), then its capabilities. */
#include "wlfw.h"

static struct qrtr *qr;
static struct wlfw_info wi;
static u16 txn;
static struct qmi_buf qb;

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
		return;
	}
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
