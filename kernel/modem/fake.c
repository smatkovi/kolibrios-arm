/* KolibriOS for ARM: a modem in RAM.
 *
 * It lays out SMEM as the firmware does (the partition table, the private
 * partition of hosts 0 and 1, GLINK's descriptor and FIFOs in it), and is
 * the remote end there: it answers IPCRTR's OPEN, advertises intents, says
 * QRTR's HELLO, announces DMS, NAS, WMS and VOICE, and answers their
 * requests the way a modem does: online after a request, registered with
 * "KolibriNet", some messages stored, a sent message answered by an echo
 * (a message "ring" by a call), calls dialled, alerting and answered. It
 * also reads a sector through our rmtfs service, as the modem does. */
#include "fake.h"

#define F_LCID 7
#define F_NODE 0
#define PART_OFF 4096
#define PART_SIZE (96 * 1024)
#define FIFO_SIZE 16384

struct fake_stats fake_stats;
bool fake_preopened;
bool fake_adopted;

static volatile u8 *desc, *f0, *f1;       /* f0: ours to read, f1: ours to write */
static u8 *rmem;
static u8 efs[12 + 24 + 4096];
static u32 now;
static u32 (*clk)(void);

/* the channel */
static bool ch_open;          /* the AP opened it, we acknowledged and opened ours */
static bool ap_acked;         /* the AP acknowledged ours */
static struct { u32 id, size; bool used, valid; } ap_int[32];
static u8 rxbuf[4][4096];
static u32 rxoff[4];
static bool hello_sent, said_services, rmtfs_tried;

/* the QMI side */
static u32 ap_node = 1;
static u32 dms_mode = 1;      /* low power, as ofono leaves it */
static bool time_sent;
static struct { bool used; u8 tag; u32 len; u8 pdu[SMS_PDU_MAX]; } store[16];
static u32 rm_caller;
static struct { u32 at; int what; char number[32]; char text[160]; } todo[8];

/* the services: QMI service, port */
static const u32 svcs[][3] = { { 2, 1, 51 }, { 3, 1, 39 }, { 5, 1, 55 }, { 9, 2, 38 }, { 11, 1, 48 },
				{ 69, 1, 77 } };

/* --- SMEM -------------------------------------------------------------- */

static volatile u8 *add_item(volatile u8 *p, u32 *off, u16 item, u32 size)
{
	volatile u8 *e = p + *off;
	*(volatile u16 *)e = 0xa5a5;
	*(volatile u16 *)(e + 2) = item;
	sm_wr32(e + 4, size);
	*(volatile u16 *)(e + 8) = 0;
	*(volatile u16 *)(e + 10) = 0;
	sm_wr32(e + 12, 0);
	*off += 16 + size;
	return e + 16;
}

static void layout(u8 *base)
{
	mzero(base, FAKE_SMEM_SIZE);
	volatile u8 *pt = base + FAKE_SMEM_SIZE - 4096;
	sm_wr32(pt, 0x434f5424u);
	sm_wr32(pt + 4, 1);
	sm_wr32(pt + 8, 1);
	volatile u8 *e = pt + 32;
	sm_wr32(e, PART_OFF);
	sm_wr32(e + 4, PART_SIZE);
	*(volatile u16 *)(e + 12) = 0;
	*(volatile u16 *)(e + 14) = 1;
	sm_wr32(e + 16, 64);
	volatile u8 *p = base + PART_OFF;
	sm_wr32(p, 0x54525024u);
	*(volatile u16 *)(p + 4) = 0;
	*(volatile u16 *)(p + 6) = 1;
	sm_wr32(p + 8, PART_SIZE);
	u32 off = 32;
	desc = add_item(p, &off, 478, 32);
	f0 = add_item(p, &off, 479, FIFO_SIZE);
	f1 = add_item(p, &off, 480, FIFO_SIZE);
	sm_wr32(p + 12, off);
	sm_wr32(p + 16, PART_SIZE);
}

/* --- GLINK, the remote's side ------------------------------------------ */

static u32 in_avail(void)
{
	return glink_fifo_avail(sm_rd32(desc + 4), sm_rd32(desc), FIFO_SIZE);
}

static void in_peek(u32 off, void *d, u32 n)
{
	glink_fifo_peek(f0, FIFO_SIZE, sm_rd32(desc), off, d, n);
}

static void in_advance(u32 n)
{
	u32 t = sm_rd32(desc) + M_ALIGN(n, 8);
	if (t >= FIFO_SIZE) t -= FIFO_SIZE;
	sm_wr32(desc, t);
}

static void out(const void *hdr, u32 hl, const void *data, u32 dl)
{
	u8 b[16 + 4096 + 8];
	mcopy(b, hdr, hl);
	if (dl) mcopy(b + hl, data, dl);
	u32 n = M_ALIGN(hl + dl, 8);
	mzero(b + hl + dl, n - hl - dl);
	u32 head = sm_rd32(desc + 12), tail = sm_rd32(desc + 8);
	u32 used = glink_fifo_avail(head, tail, FIFO_SIZE);
	if (FIFO_SIZE - used < n + 16) {
		mlog("fake: FIFO full\n");
		return;
	}
	head = glink_fifo_write(f1, FIFO_SIZE, head, b, n);
	sm_wr32(desc + 12, head);
}

static void out_cmd(u16 c, u16 p1, u32 p2)
{
	u8 h[8];
	put16(h, c);
	put16(h + 2, p1);
	put32(h + 4, p2);
	out(h, 8, 0, 0);
}

/* a QRTR packet to the AP in one of its intents */
static void send_pkt(const u8 *data, u32 len)
{
	for (int i = 0; i < 32; i++)
		if (ap_int[i].valid && !ap_int[i].used && ap_int[i].size >= len) {
			ap_int[i].used = true;
			u8 h[16];
			put16(h, GLINK_CMD_TX_DATA);
			put16(h + 2, F_LCID);
			put32(h + 4, ap_int[i].id);
			put32(h + 8, len);
			put32(h + 12, 0);
			out(h, 16, data, len);
			return;
		}
	mlog("fake: no intent of the AP's for %u bytes\n", len);
}

static u8 pk[4096 + 64];

static void qrtr_out(u32 type, u32 sport, u32 dport, const void *data, u32 len)
{
	u32 n = qrtr_pack(pk, type, F_NODE, sport, ap_node, dport, false, data, len);
	send_pkt(pk, n);
}

static void ctrl_out(u32 type, const u8 *p20)
{
	u8 b[20];
	if (p20) mcopy(b, p20, 20); else mzero(b, 20);
	put32(b, type);
	qrtr_out(type, QRTR_PORT_CTRL, QRTR_PORT_CTRL, b, 20);
}

/* --- QMI: the services --------------------------------------------------- */

static struct qmi_buf qo;

static void qmi_send(u32 sport, u32 dport)
{
	u32 n = qmi_end(&qo);
	qrtr_out(QRTR_TYPE_DATA, sport, dport, qo.d, n);
}

static u16 ind_txn;

/* VOICE: the call list, one call */
static struct { u8 id, state, dir; char number[32]; } fcall;

static void call_ind(u32 dport)
{
	qmi_begin(&qo, QMI_INDICATION, ++ind_txn, 0x2e);
	u8 info[1 + 7];
	info[0] = 1;
	info[1] = fcall.id; info[2] = fcall.state; info[3] = 0; info[4] = fcall.dir;
	info[5] = 4; info[6] = 0; info[7] = 0;
	qmi_put(&qo, 0x01, info, 8);
	u8 num[3 + 32];
	u32 l = mstrlen(fcall.number);
	num[0] = 1; num[1] = fcall.id; num[2] = 0; num[3] = l;
	mcopy(num + 4, fcall.number, l);
	qmi_put(&qo, 0x10, num, 4 + l);
	qmi_send(38, dport);
}

static u32 voice_client = 0, wms_client = 0, nas_client = 0;

static void later(u32 ms, int what, const char *number, const char *text)
{
	for (int i = 0; i < 8; i++)
		if (!todo[i].what) {
			todo[i].at = now + ms;
			todo[i].what = what;
			mstrcpy(todo[i].number, number, sizeof(todo[i].number));
			mstrcpy(todo[i].text, text ? text : "", sizeof(todo[i].text));
			return;
		}
}

/* a message from number, stored, its index */
static int store_deliver(const char *number, const char *text)
{
	u8 pdus[2][SMS_PDU_MAX];
	u32 len[2];
	if (sms_encode(number, text, 1, pdus, len, 1) != 1) return -1;
	/* the SUBMIT made a DELIVER: first octet, no MR, a time stamp for the validity */
	u8 *p = pdus[0];
	u8 d[SMS_PDU_MAX];
	u32 o = 0, i = 1;
	d[o++] = 0;                                /* SMSC */
	d[o++] = 0x04;                             /* DELIVER, no more messages */
	i += 2;                                    /* first octet, MR */
	u32 al = 2 + (p[i] + 1) / 2;
	mcopy(d + o, p + i, al); o += al; i += al;
	d[o++] = p[i++];                           /* PID */
	d[o++] = p[i++];                           /* DCS */
	i++;                                       /* VP */
	static const u8 ts[7] = { 0x62, 0x01, 0x92, 0x21, 0x00, 0x00, 0x40 }; /* 2026-10-29 12:00 +1h */
	mcopy(d + o, ts, 7); o += 7;
	mcopy(d + o, p + i, len[0] - i); o += len[0] - i;
	for (int k = 0; k < 16; k++)
		if (!store[k].used) {
			store[k].used = true;
			store[k].tag = 1;
			store[k].len = o;
			mcopy(store[k].pdu, d, o);
			return k;
		}
	return -1;
}

static void wms_new(int index)
{
	if (index < 0 || !wms_client) return;
	qmi_begin(&qo, QMI_INDICATION, ++ind_txn, 0x01);
	u8 v[5];
	v[0] = 1;
	put32(v + 1, index);
	qmi_put(&qo, 0x10, v, 5);
	qmi_send(55, wms_client);
}

static void svc_request(u32 port, u32 sport, const struct qmi_msg *m)
{
	qmi_begin(&qo, QMI_RESPONSE, m->txn, m->id);
	u32 n, n2, n3;
	const u8 *v;
	switch (port << 16 | m->id) {
	case 51 << 16 | 0x2d:                      /* DMS: Get Operating Mode */
		qmi_put_result(&qo, 0, 0);
		qmi_put_u8(&qo, 0x01, dms_mode);
		break;
	case 51 << 16 | 0x2e:
		v = qmi_tlv(m, 0x01, &n);
		if (v) dms_mode = v[0];
		qmi_put_result(&qo, 0, 0);
		break;
	case 39 << 16 | 0x03:                      /* NAS: Register Indications */
		nas_client = sport;
		qmi_put_result(&qo, 0, 0);
		break;
	case 39 << 16 | 0x24: {                    /* NAS: Get Serving System */
		qmi_put_result(&qo, 0, 0);
		u8 s[6] = { dms_mode == 0 ? 1 : 0, 1, 1, 2, 1, 8 };
		qmi_put(&qo, 0x01, s, 6);
		qmi_put_u8(&qo, 0x10, 1);
		u8 plmn[5 + 10];
		put16(plmn, 232);
		put16(plmn + 2, 1);
		plmn[4] = 10;
		mcopy(plmn + 5, "KolibriNet", 10);
		qmi_put(&qo, 0x12, plmn, 15);
		break;
	}
	case 39 << 16 | 0x20: {                    /* NAS: Get Signal Strength */
		qmi_put_result(&qo, 0, 0);
		u8 s[2] = { (u8)(s8)-67, 8 };
		qmi_put(&qo, 0x01, s, 2);
		break;
	}
	case 55 << 16 | 0x01:                      /* WMS: Set Event Report */
		wms_client = sport;
		qmi_put_result(&qo, 0, 0);
		break;
	case 55 << 16 | 0x32:                      /* WMS: Set Routes */
	case 55 << 16 | 0x23:                      /* WMS: Modify Tag */
		qmi_put_result(&qo, 0, 0);
		break;
	case 55 << 16 | 0x24:                      /* WMS: Delete */
		v = qmi_tlv(m, 0x10, &n);
		if (v && n >= 4 && get32(v) < 16) store[get32(v)].used = false;
		qmi_put_result(&qo, 0, 0);
		break;
	case 55 << 16 | 0x31: {                    /* WMS: List Messages */
		v = qmi_tlv(m, 0x01, &n);
		qmi_put_result(&qo, 0, 0);
		u8 l[4 + 16 * 5];
		u32 c = 0;
		for (int k = 0; v && v[0] == 1 && k < 16; k++)
			if (store[k].used) {
				put32(l + 4 + 5 * c, k);
				l[4 + 5 * c + 4] = store[k].tag;
				c++;
			}
		put32(l, c);
		qmi_put(&qo, 0x01, l, 4 + 5 * c);
		break;
	}
	case 55 << 16 | 0x22: {                    /* WMS: Raw Read */
		v = qmi_tlv(m, 0x01, &n);
		u32 k = v && n >= 5 ? get32(v + 1) : 99;
		if (!v || v[0] != 1 || k >= 16 || !store[k].used) {
			qmi_put_result(&qo, 1, 0x10);
			break;
		}
		qmi_put_result(&qo, 0, 0);
		u8 r[4 + SMS_PDU_MAX];
		r[0] = store[k].tag;
		r[1] = 6;
		put16(r + 2, store[k].len);
		mcopy(r + 4, store[k].pdu, store[k].len);
		qmi_put(&qo, 0x01, r, 4 + store[k].len);
		break;
	}
	case 55 << 16 | 0x20: {                    /* WMS: Raw Send */
		v = qmi_tlv(m, 0x01, &n);
		struct sms_part p;
		if (!v || n < 3 || v[0] != 6 || sms_decode(v + 3, get16(v + 1), &p) < 0 || !p.submit) {
			qmi_put_result(&qo, 1, 0x30);
			break;
		}
		fake_stats.sent++;
		mstrcpy(fake_stats.last_text, p.text, sizeof(fake_stats.last_text));
		qmi_put_result(&qo, 0, 0);
		qmi_put_u16(&qo, 0x01, fake_stats.sent);
		if (mstreq(p.text, "ring")) later(2000, 2, p.number, 0);
		else if (!p.total || p.seq == p.total) later(1500, 1, p.number, p.text);
		break;
	}
	case 38 << 16 | 0x03:                      /* VOICE: Indication Register */
		voice_client = sport;
		qmi_put_result(&qo, 0, 0);
		break;
	case 38 << 16 | 0x2f: {                    /* VOICE: Get All Call Info */
		qmi_put_result(&qo, 0, 0);
		u8 none = 0;
		qmi_put(&qo, 0x10, &none, 1);
		break;
	}
	case 38 << 16 | 0x20:                      /* VOICE: Dial */
		v = qmi_tlv(m, 0x01, &n);
		fake_stats.dialled++;
		fcall.id = 1;
		fcall.state = 1;
		fcall.dir = 1;
		mzero(fcall.number, sizeof(fcall.number));
		if (v) mcopy(fcall.number, v, MIN(n, 31u));
		qmi_put_result(&qo, 0, 0);
		qmi_put_u8(&qo, 0x10, 1);
		later(800, 3, fcall.number, 0);       /* alerting */
		later(2500, 4, fcall.number, 0);      /* answered */
		break;
	case 38 << 16 | 0x22:                      /* VOICE: Answer */
		fake_stats.answered++;
		qmi_put_result(&qo, 0, 0);
		qmi_put_u8(&qo, 0x10, fcall.id);
		later(200, 4, fcall.number, 0);
		break;
	case 38 << 16 | 0x21:                      /* VOICE: End */
		fake_stats.ended++;
		qmi_put_result(&qo, 0, 0);
		qmi_put_u8(&qo, 0x10, fcall.id);
		later(200, 5, fcall.number, 0);
		break;
	case 77 << 16 | 0x20:                      /* WLFW: indication register */
		v = qmi_tlv(m, 0x15, &n);
		qmi_put_result(&qo, v && n == 4 && get32(v) == 0x4b4e454c ? 0 : 1, 0);
		qmi_put_u64(&qo, 0x10, 2);             /* fw_status: FW_READY */
		fake_stats.wlfw++;
		break;
	case 77 << 16 | 0x24: {                    /* WLFW: capabilities, the Pro1X's */
		u8 b[64];
		qmi_put_result(&qo, 0, 0);
		put32(b, 0x120);
		put32(b + 4, 0x4007);
		qmi_put(&qo, 0x10, b, 8);
		qmi_put_u32(&qo, 0x11, 0xff);
		qmi_put_u32(&qo, 0x12, 0x40670000);
		put32(b, 0x324103d6);
		b[4] = 16;
		mcopy(b + 5, "2021-12-02 08:20", 16);
		qmi_put(&qo, 0x13, b, 21);
		qmi_put(&qo, 0x14, "QC_IMAGE_VERSION_STRING=WLAN.HL.3.2.4", 37);
		fake_stats.wlfw++;
		break;
	}
	case 77 << 16 | 0x23: {                    /* WLFW: WLAN_CFG, as ath10k sends it */
		const u8 *t = qmi_tlv(m, 0x11, &n), *sv = qmi_tlv(m, 0x12, &n2), *sh = qmi_tlv(m, 0x13, &n3);
		bool ok = t && sv && sh && t[0] == 12 && n == 1 + 12 * 20 && sv[0] == 21 && sh[0] == 12;
		qmi_put_result(&qo, ok ? 0 : 1, ok ? 0 : 0x30);
		fake_stats.wlfw++;
		fake_stats.wlan_cfg++;
		break;
	}
	case 77 << 16 | 0x22:                      /* WLFW: WLAN_MODE */
		v = qmi_tlv(m, 0x01, &n);
		qmi_put_result(&qo, v && n == 4 && get32(v) <= 4 ? 0 : 1, 0);
		fake_stats.wlfw++;
		fake_stats.wlan_mode = v && n == 4 ? get32(v) : 99;
		break;
	default:
		qmi_put_result(&qo, 1, 0x47);          /* not supported */
		break;
	}
	qmi_send(port, sport);
}

/* rmtfs, as the modem uses it: open, the buffer, a sector read */
static u16 rm_txn;
static int rm_step;

static void rmtfs_next(const struct qmi_msg *m)
{
	u32 n;
	const u8 *v;
	switch (rm_step) {
	case 0:
		qmi_begin(&qo, QMI_REQUEST, ++rm_txn, 1);
		qmi_put(&qo, 0x01, "/boot/modem_fs1", 15);
		qmi_send(0x5000, RMTFS_PORT);
		rm_step = 1;
		break;
	case 1:
		v = qmi_tlv(m, 0x10, &n);
		if (!qmi_ok(m, 0) || !v) { fake_stats.rmtfs_bad++; rm_step = 9; break; }
		rm_caller = get32(v);
		qmi_begin(&qo, QMI_REQUEST, ++rm_txn, 4);
		qmi_put_u32(&qo, 0x01, rm_caller);
		qmi_put_u32(&qo, 0x02, 8192);
		qmi_send(0x5000, RMTFS_PORT);
		rm_step = 2;
		break;
	case 2: {
		v = qmi_tlv(m, 0x10, &n);
		if (!qmi_ok(m, 0) || !v || get64(v) != FAKE_RMTFS_PA) { fake_stats.rmtfs_bad++; rm_step = 9; break; }
		qmi_begin(&qo, QMI_REQUEST, ++rm_txn, 3);
		qmi_put_u32(&qo, 0x01, rm_caller);
		qmi_put_u8(&qo, 0x02, 0);
		u8 vec[1 + 12];
		vec[0] = 1;
		put32(vec + 1, 1);                     /* sector 1 */
		put32(vec + 5, FAKE_RMTFS_PA + 512);
		put32(vec + 9, 2);
		qmi_put(&qo, 0x03, vec, 13);
		qmi_put_u8(&qo, 0x04, 0);
		qmi_send(0x5000, RMTFS_PORT);
		rm_step = 3;
		break;
	}
	case 3: {
		bool same = qmi_ok(m, 0);
		for (u32 i = 0; same && i < 1024; i++) same = rmem[512 + i] == efs[12 + 24 + 512 + i];
		if (same) fake_stats.rmtfs_ok++; else fake_stats.rmtfs_bad++;
		mlog("fake: rmtfs read %s\n", same ? "the image's sectors" : "something else");
		rm_step = 9;
		break;
	}
	}
}

static void qmi_in(u32 sport, u32 dport, const u8 *d, u32 n)
{
	struct qmi_msg m;
	if (!qmi_parse(d, n, &m)) return;
	if (dport == 0x5000) {                     /* our rmtfs client */
		if (m.type == QMI_RESPONSE) rmtfs_next(&m);
		return;
	}
	if (m.type == QMI_REQUEST) svc_request(dport, sport, &m);
}

static void qrtr_in(const u8 *d, u32 n)
{
	struct qrtr_pkt k;
	if (!qrtr_unpack(d, n, &k)) return;
	if (k.type == QRTR_TYPE_DATA && k.dport != QRTR_PORT_CTRL) {
		qmi_in(k.sport, k.dport, k.data, k.size);
		return;
	}
	if (k.type == QRTR_TYPE_HELLO && !said_services) {
		said_services = true;
		for (unsigned i = 0; i < sizeof(svcs) / sizeof(svcs[0]); i++) {
			u8 b[20];
			put32(b, QRTR_TYPE_NEW_SERVER);
			put32(b + 4, svcs[i][0]);
			put32(b + 8, svcs[i][1]);
			put32(b + 12, F_NODE);
			put32(b + 16, svcs[i][2]);
			ctrl_out(QRTR_TYPE_NEW_SERVER, b);
		}
	}
	if (k.type == QRTR_TYPE_NEW_SERVER && k.size >= 20 && get32(k.data + 4) == QMI_SVC_RMTFS && !rmtfs_tried) {
		rmtfs_tried = true;
		rmtfs_next(0);
	}
}

static void glink_in(void)
{
	for (;;) {
		u32 avail = in_avail();
		if (avail < 8) return;
		u8 h[8];
		in_peek(0, h, 8);
		u16 c = get16(h), p1 = get16(h + 2);
		u32 p2 = get32(h + 4);
		switch (c) {
		case GLINK_CMD_OPEN: {
			char name[33];
			u32 nl = p2 & 0xffff;
			if (avail < 8 + nl) return;
			in_peek(8, name, MIN(nl, 32u));
			name[MIN(nl, 32u)] = 0;
			in_advance(8 + nl);
			if (!mstreq(name, "IPCRTR")) break;
			out_cmd(GLINK_CMD_OPEN_ACK, p1, 0);
			if (!fake_preopened) {         /* (else ours is in the FIFO's past) */
				u8 m[16] = { 0 };
				put16(m, GLINK_CMD_OPEN);
				put16(m + 2, F_LCID);
				put32(m + 4, 7);
				mcopy(m + 8, "IPCRTR", 7);
				out(m, 15, 0, 0);
			}
			ch_open = true;
			break;
		}
		case GLINK_CMD_OPEN_ACK:
			in_advance(8);
			if (p1 == F_LCID) {
				ap_acked = true;
				for (u32 i = 0; i < 4; i++) {
					u8 m[16];
					put16(m, GLINK_CMD_INTENT);
					put16(m + 2, F_LCID);
					put32(m + 4, 1);
					put32(m + 8, 4096);
					put32(m + 12, 100 + i);
					out(m, 16, 0, 0);
				}
			}
			break;
		case GLINK_CMD_INTENT: {
			if (avail < 8 + 8 * p2) return;
			for (u32 i = 0; i < p2; i++) {
				u8 pr[8];
				in_peek(8 + 8 * i, pr, 8);
				for (int j = 0; j < 32; j++)
					if (!ap_int[j].valid) {
						ap_int[j].valid = true;
						ap_int[j].used = false;
						ap_int[j].size = get32(pr);
						ap_int[j].id = get32(pr + 4);
						break;
					}
			}
			in_advance(8 + 8 * p2);
			break;
		}
		case GLINK_CMD_RX_DONE:
		case GLINK_CMD_RX_DONE_W_REUSE:
			in_advance(8);
			for (int j = 0; j < 32; j++)
				if (ap_int[j].valid && ap_int[j].id == p2) {
					ap_int[j].used = false;
					if (c == GLINK_CMD_RX_DONE) ap_int[j].valid = false;
				}
			break;
		case GLINK_CMD_TX_DATA:
		case GLINK_CMD_TX_DATA_CONT: {
			if (avail < 16) return;
			u8 x[8];
			in_peek(8, x, 8);
			u32 chunk = get32(x), left = get32(x + 4);
			if (avail < 16 + chunk) return;
			u32 k = p2 - 100;
			if (k < 4 && rxoff[k] + chunk <= 4096) {
				in_peek(16, rxbuf[k] + rxoff[k], chunk);
				rxoff[k] += chunk;
			}
			in_advance(16 + chunk);
			if (k < 4 && !left) {
				u32 len = rxoff[k];
				rxoff[k] = 0;
				out_cmd(GLINK_CMD_RX_DONE_W_REUSE, F_LCID, p2);
				qrtr_in(rxbuf[k], len);
			}
			break;
		}
		case GLINK_CMD_RX_INTENT_REQ: {
			in_advance(8);
			u8 m[16];
			put16(m, GLINK_CMD_INTENT);
			put16(m + 2, F_LCID);
			put32(m + 4, 1);
			put32(m + 8, 4096);
			put32(m + 12, 103);
			out(m, 16, 0, 0);
			out_cmd(GLINK_CMD_RX_INTENT_REQ_ACK, F_LCID, 1);
			break;
		}
		default:
			in_advance(8);
			break;
		}
	}
}

/* a call from number ms from now (kolibri.fakering: the standby's end by a call) */
void fake_ring_later(u32 ms, const char *number)
{
	later(ms, 2, number, 0);
}

void fake_ring(const char *number)
{
	fcall.id = 2;
	fcall.state = 2;
	fcall.dir = 2;
	mstrcpy(fcall.number, number, sizeof(fcall.number));
	if (voice_client) call_ind(voice_client);
}

/* when its next timed event is due (ms from ms), 0xffffffff: none */
u32 fake_next_ms(u32 ms)
{
	u32 n = 0xffffffffu;
	for (int i = 0; i < 8; i++) {
		if (!todo[i].what) continue;
		s32 d = (s32)(todo[i].at - ms);
		n = MIN(n, d > 0 ? (u32)d : 0u);
	}
	return n;
}

void fake_poll(u32 ms)
{
	now = ms;
	glink_in();
	if (ch_open && ap_acked && !hello_sent) {
		for (int i = 0; i < 32; i++)
			if (ap_int[i].valid) {
				hello_sent = true;
				ctrl_out(QRTR_TYPE_HELLO, 0);
				break;
			}
	}
	/* online: the network's time, once */
	if (!time_sent && dms_mode == 0 && nas_client) {
		time_sent = true;
		qmi_begin(&qo, QMI_INDICATION, ++ind_txn, 0x24);
		u8 s[6] = { 1, 1, 1, 2, 1, 8 };
		qmi_put(&qo, 0x01, s, 6);
		u8 t[8];
		put16(t, 2026); t[2] = 10; t[3] = 29; t[4] = 11; t[5] = 0; t[6] = 0; t[7] = 4;
		qmi_put(&qo, 0x1c, t, 8);
		qmi_send(39, nas_client);
	}
	for (int i = 0; i < 8; i++) {
		if (!todo[i].what || (s32)(now - todo[i].at) < 0) continue;
		int w = todo[i].what;
		todo[i].what = 0;
		if (w == 1) {
			char e[180] = "Echo: ";
			mstrcpy(e + 6, todo[i].text, sizeof(e) - 6);
			wms_new(store_deliver(todo[i].number, e));
		} else if (w == 2) {
			fake_ring(todo[i].number);
		} else if (w >= 3 && voice_client && fcall.id) {
			if (w == 3 && fcall.state != 1) continue;     /* alerting: still dialling */
			if (w == 4 && fcall.state == 9) continue;
			fcall.state = w == 3 ? 5 : w == 4 ? 3 : 9;
			call_ind(voice_client);
			if (w == 5) fcall.id = 0;
		}
	}
	(void)clk;
}

void fake_init(struct modem_hw *hw, u8 *smem, u8 *rmtfs, u32 (*clock)(void), void (*set_clock)(u32))
{
	layout(smem);
	if (fake_preopened) {
		/* some traffic, our OPEN of IPCRTR, more traffic: all read (the
		 * tail at the head), as Linux leaves it */
		out_cmd(GLINK_CMD_SIGNALS, 3, 0);
		u8 m[16] = { 0 };
		put16(m, GLINK_CMD_OPEN);
		put16(m + 2, F_LCID);
		put32(m + 4, 7);
		mcopy(m + 8, "IPCRTR", 7);
		out(m, 15, 0, 0);
		out_cmd(GLINK_CMD_SIGNALS, 5, 0);
		sm_wr32(desc + 8, sm_rd32(desc + 12));
	}
	if (fake_adopted) {
		/* the channel open on both sides from Linux's time, QRTR packets
		 * of both in the FIFOs' past (all read), no OPEN anywhere */
		u8 m[16 + 64], q[64];
		u32 n = qrtr_pack(q, QRTR_TYPE_NEW_SERVER, F_NODE, QRTR_PORT_CTRL, 1, QRTR_PORT_CTRL, false,
				  "\x04\0\0\0\x02\0\0\0\x01\0\0\0\0\0\0\0\x33\0\0\0", 20);
		mzero(m, sizeof(m));
		put16(m, GLINK_CMD_TX_DATA);
		put16(m + 2, F_LCID);
		put32(m + 4, 1);
		put32(m + 8, n);
		mcopy(m + 16, q, n);
		out(m, 16 + n, 0, 0);
		sm_wr32(desc + 8, sm_rd32(desc + 12));
		n = qrtr_pack(q, QRTR_TYPE_DATA, 1, 0x4000, F_NODE, 51, false, "\0\x01\0\x2d\0\0\0", 7);
		mzero(m, sizeof(m));
		put16(m, GLINK_CMD_TX_DATA);
		put16(m + 2, 3);                       /* Linux's lcid */
		put32(m + 4, 1);
		put32(m + 8, n);
		mcopy(m + 16, q, n);
		u32 head = glink_fifo_write(f0, FIFO_SIZE, sm_rd32(desc + 4), m, M_ALIGN(16 + n, 8));
		sm_wr32(desc + 4, head);
		sm_wr32(desc, head);
		ch_open = ap_acked = true;
	}
	rmem = rmtfs;
	clk = clock;
	/* an EFS bundle of one image: modemst1, 4 KiB of a pattern */
	mzero(efs, sizeof(efs));
	mcopy(efs, "KOSEFS01", 8);
	put32(efs + 8, 1);
	mcopy(efs + 12, "modemst1", 8);
	put32(efs + 12 + 16, 36);
	put32(efs + 12 + 20, 4096);
	for (u32 i = 0; i < 4096; i++) efs[36 + i] = (u8)(i * 7 + i / 512);
	/* the stored messages */
	static const u8 pdu0[] = {             /* "How are you?" (the classic example) */
		0x07, 0x91, 0x13, 0x26, 0x04, 0x00, 0x00, 0xf0, 0x04, 0x0b, 0x91, 0x13, 0x46, 0x61,
		0x00, 0x89, 0xf6, 0x00, 0x00, 0x20, 0x80, 0x62, 0x91, 0x73, 0x14, 0x08, 0x0c, 0xc8,
		0xf7, 0x1d, 0x14, 0x96, 0x97, 0x41, 0xf9, 0x77, 0xfd, 0x07,
	};
	store[0].used = true;
	store[0].tag = 0;
	store[0].len = sizeof(pdu0);
	mcopy(store[0].pdu, pdu0, sizeof(pdu0));
	store_deliver("+436641234567", "Gr\xc3\xbc\xc3\x9f" "e aus Wien! Kommst du heute Abend?");
	store_deliver("+436641234567", "Ich bin ab 19 Uhr da.");
	mzero(hw, sizeof(*hw));
	hw->smem = smem;
	hw->smem_size = FAKE_SMEM_SIZE;
	hw->rmtfs_mem = rmtfs;
	hw->rmtfs_pa = FAKE_RMTFS_PA;
	hw->rmtfs_size = FAKE_RMTFS_SIZE;
	hw->efs = efs;
	hw->efs_size = sizeof(efs);
	hw->clock = clock;
	hw->set_clock = set_clock;
	hw->fake = true;
}
