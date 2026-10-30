/* KolibriOS for ARM: the modem's services for calls and SMS.
 *
 * The link first: SMEM, GLINK, the IPCRTR channel opened (Linux closed
 * its own before the kexec, docs/pro1x-modem.md), QRTR's HELLO, then our
 * rmtfs service announced and the modem's services found. Then, as
 * ModemManager does: DMS operating mode online; WMS routes (messages
 * stored on the modem, a notification for each), its event report, the
 * stored messages listed and read; VOICE's call indications and the calls
 * there are; NAS's registration, operator and signal every few seconds,
 * the network's time from its serving system indication. Everything runs
 * from the timer tick: requests go out, responses and indications come
 * back through QRTR's ports, one per service. */
#include "phone.h"
#include "wlfw.h"

#define LCID_IPCRTR 0x100         /* far from the ids Linux's channels hold */
#define PORT_BASE 0x4000
#define NREQ 24
#define SMS_MAX 96
#define FRAGS 4
#define POLL_MS 5000

enum { SVC_DMS, SVC_NAS, SVC_WMS, SVC_VOICE, NSVC };
static const u32 svc_id[NSVC] = { QMI_SVC_DMS, QMI_SVC_NAS, QMI_SVC_WMS, QMI_SVC_VOICE };
static const char *const svc_name[NSVC] = { "DMS", "NAS", "WMS", "VOICE" };

enum { ST_OFF, ST_OPEN, ST_HELLO, ST_SERVICES, ST_READY, ST_FAILED };

/* WMS, VOICE, NAS, DMS messages */
enum {
	DMS_GET_MODE = 0x2d, DMS_SET_MODE = 0x2e,
	NAS_REGISTER = 0x03, NAS_SIGNAL = 0x20, NAS_SERVING = 0x24,
	WMS_EVENT_REPORT = 0x01, WMS_RAW_SEND = 0x20, WMS_RAW_READ = 0x22, WMS_DELETE = 0x24,
	WMS_LIST = 0x31, WMS_SET_ROUTES = 0x32, WMS_SEND_ACK = 0x37, WMS_MODIFY_TAG = 0x23,
	VOICE_REGISTER = 0x03, VOICE_DIAL = 0x20, VOICE_END = 0x21, VOICE_ANSWER = 0x22,
	VOICE_ALL_STATUS = 0x2e, VOICE_GET_ALL = 0x2f,
};

static const struct modem_hw *hw;
static struct smem smem;
static struct glink glink;
static struct qrtr qrtr;
static struct rmtfs rmtfs;
static struct glink_chan *ipcrtr;
static bool adopted;

static int st = ST_OFF;
static u32 now, st_at, next_poll, poll_ms = POLL_MS;
static char status[64];

struct client { bool have; u32 node, port; u16 txn; };
static struct client cl[NSVC];

struct req { bool used; int svc; u16 txn, msg; u32 tag, sent; };
static struct req reqs[NREQ];

/* the modem's state for the programs */
static u32 registration = KOS_REG_UNKNOWN, online;
static s32 signal_dbm;
static char operator_name[32];
static u32 sms_seq, call_seq;

struct rec {
	bool used;
	u32 id, time;
	u8 dir, state, storage;
	u32 index;                 /* on the modem; 0xffffffff: not stored */
	u8 parts, parts_done, parts_failed;
	char number[32];
	char text[KOS_SMS_TEXT];
};
static struct rec recs[SMS_MAX];
static u32 next_id = 1;

/* parts of a concatenated message, until all are there */
struct frag {
	bool used;
	char number[32];
	u16 ref;
	u8 total, have;            /* have: a bit per part */
	u32 time, first_at;
	u8 storage;
	u32 index;
	char text[8][SMS_TEXT_MAX / 2];
};
static struct frag frags[FRAGS];

struct call { u32 id, state, dir, started; char number[32]; };
static struct call call;
static u32 ended_at;

static void set_status(const char *s)
{
	mstrcpy(status, s, sizeof(status));
	mlog("modem: %s\n", s);
}

/* --- requests ---------------------------------------------------------- */

static struct qmi_buf qb;

static void begin(int svc, u16 msg)
{
	qmi_begin(&qb, QMI_REQUEST, 0, msg);
	(void)svc;
}

/* the request in qb to the service; tag for its response */
static int send_req(int svc, u32 tag)
{
	struct client *c = &cl[svc];
	if (!c->have) return -1;
	struct req *r = 0;
	for (int i = 0; i < NREQ && !r; i++)
		if (!reqs[i].used) r = &reqs[i];
	if (!r) {
		mlog("modem: too many requests at once\n");
		return -1;
	}
	if (!++c->txn) c->txn = 1;
	put16(qb.d + 1, c->txn);
	r->used = true;
	r->svc = svc;
	r->txn = c->txn;
	r->msg = get16(qb.d + 3);
	r->tag = tag;
	r->sent = now;
	u32 n = qmi_end(&qb);
	return qrtr_send(&qrtr, PORT_BASE + svc, c->node, c->port, qb.d, n);
}

/* --- messages ---------------------------------------------------------- */

static struct rec *rec_new(void)
{
	struct rec *r = 0;
	for (int i = 0; i < SMS_MAX && !r; i++)
		if (!recs[i].used) r = &recs[i];
	if (!r) {                                  /* full: the oldest goes */
		r = &recs[0];
		for (int i = 1; i < SMS_MAX; i++)
			if (recs[i].id < r->id) r = &recs[i];
	}
	mzero(r, sizeof(*r));
	r->used = true;
	r->id = next_id++;
	r->index = 0xffffffffu;
	return r;
}

static bool have_stored(u8 storage, u32 index)
{
	for (int i = 0; i < SMS_MAX; i++)
		if (recs[i].used && recs[i].index == index && recs[i].storage == storage) return true;
	for (int i = 0; i < FRAGS; i++)
		if (frags[i].used && frags[i].index == index && frags[i].storage == storage) return true;
	return false;
}

static void store_rec(const struct sms_part *p, const char *text, u8 storage, u32 index, bool unread)
{
	struct rec *r = rec_new();
	r->dir = p->submit ? KOS_SMS_OUT : KOS_SMS_IN;
	r->state = p->submit ? KOS_SMS_SENT : unread ? KOS_SMS_UNREAD : KOS_SMS_READ;
	r->time = p->time ? p->time : hw->clock ? hw->clock() : 0;
	r->storage = storage;
	r->index = index;
	mstrcpy(r->number, p->number, sizeof(r->number));
	mstrcpy(r->text, text, sizeof(r->text));
	sms_seq++;
}

/* a part of a message: kept, or the whole message when it is complete */
static void got_part(const struct sms_part *p, u8 storage, u32 index, bool unread)
{
	if (!p->total) {
		store_rec(p, p->text, storage, index, unread);
		return;
	}
	struct frag *f = 0;
	for (int i = 0; i < FRAGS && !f; i++)
		if (frags[i].used && frags[i].ref == p->ref && frags[i].total == p->total &&
		    mstreq(frags[i].number, p->number))
			f = &frags[i];
	if (!f) {
		for (int i = 0; i < FRAGS && !f; i++)
			if (!frags[i].used) f = &frags[i];
		if (!f) {                          /* no room: the oldest goes as it is */
			f = &frags[0];
			for (int i = 1; i < FRAGS; i++)
				if (frags[i].first_at < f->first_at) f = &frags[i];
			f->used = false;
		}
		mzero(f, sizeof(*f));
		f->used = true;
		mstrcpy(f->number, p->number, sizeof(f->number));
		f->ref = p->ref;
		f->total = MIN(p->total, (u8)8);
		f->first_at = now;
		f->storage = storage;
		f->index = index;
	}
	if (p->seq <= f->total) {
		mstrcpy(f->text[p->seq - 1], p->text, sizeof(f->text[0]));
		f->have |= 1 << (p->seq - 1);
		if (p->seq == 1 || !f->time) f->time = p->time;
	}
	if (f->have != (1u << f->total) - 1) return;
	static char all[KOS_SMS_TEXT];
	u32 o = 0;
	for (int i = 0; i < f->total; i++)
		for (const char *s = f->text[i]; *s && o + 1 < sizeof(all); s++) all[o++] = *s;
	all[o] = 0;
	struct sms_part whole = *p;
	whole.time = f->time;
	store_rec(&whole, all, f->storage, f->index, unread);
	f->used = false;
}

static void read_stored(u8 storage, u32 index)
{
	begin(SVC_WMS, WMS_RAW_READ);
	u8 id[5];
	id[0] = storage;
	put32(id + 1, index);
	qmi_put(&qb, 0x01, id, 5);
	qmi_put_u8(&qb, 0x10, 1);              /* GSM/WCDMA */
	send_req(SVC_WMS, storage << 24 | (index & 0xffffff));
}

static void got_raw(const u8 *pdu, u32 n, u8 storage, u32 index, u8 tag)
{
	struct sms_part p;
	if (sms_decode(pdu, n, &p) < 0) {
		mlog("modem: message %u/%u is no SMS-DELIVER or -SUBMIT\n", storage, index);
		return;
	}
	got_part(&p, storage, index, tag == 1);
}

/* --- the call ---------------------------------------------------------- */

static u32 call_state(u8 s)
{
	switch (s) {
	case 1: case 4: case 10: return KOS_CALL_DIALING;
	case 5: return KOS_CALL_ALERTING;
	case 2: case 7: return KOS_CALL_INCOMING;
	case 3: return KOS_CALL_ACTIVE;
	case 6: return KOS_CALL_HELD;
	default: return KOS_CALL_ENDED;
	}
}

static int call_rank(u32 s)
{
	static const int rank[] = { 0, 3, 3, 5, 4, 2, 1 };
	return s < sizeof(rank) / sizeof(rank[0]) ? rank[s] : 0;
}

/* VOICE's call list (indication or Get All Call Info): the call that matters most */
static void calls(const u8 *info, u32 ni, const u8 *numbers, u32 nn)
{
	struct call best = { 0, KOS_CALL_NONE, 0, 0, "" };
	u32 count = ni ? info[0] : 0;
	for (u32 i = 0; i < count && 1 + 7 * (i + 1) <= ni; i++) {
		const u8 *c = info + 1 + 7 * i;
		u32 s = call_state(c[1]);
		if (best.state == KOS_CALL_NONE || call_rank(s) > call_rank(best.state)) {
			best.id = c[0];
			best.state = s;
			best.dir = c[3] == 2 ? KOS_CALL_IN : KOS_CALL_OUT;
			best.number[0] = 0;
		}
	}
	/* the numbers: id, presentation, a length octet, the digits */
	u32 nc = nn ? numbers[0] : 0, o = 1;
	for (u32 i = 0; i < nc && o + 3 <= nn; i++) {
		u8 id = numbers[o], len = numbers[o + 2];
		if (o + 3 + len > nn) break;
		if (id == best.id) {
			u32 l = MIN((u32)len, (u32)sizeof(best.number) - 1);
			mcopy(best.number, numbers + o + 3, l);
			best.number[l] = 0;
		}
		o += 3 + len;
	}
	if (best.state == KOS_CALL_NONE && call.state != KOS_CALL_NONE && call.state != KOS_CALL_ENDED) {
		call.state = KOS_CALL_ENDED;
		ended_at = now;
		call_seq++;
		return;
	}
	if (best.state == KOS_CALL_NONE) return;
	if (!best.number[0] && best.id == call.id) mstrcpy(best.number, call.number, sizeof(best.number));
	best.started = best.state == KOS_CALL_ACTIVE ?
		       (call.id == best.id && call.state == KOS_CALL_ACTIVE ? call.started : now) : 0;
	if (best.state == KOS_CALL_ENDED) ended_at = now;
	if (best.id != call.id || best.state != call.state || !mstreq(best.number, call.number)) {
		if (best.state != call.state)
			mlog("modem: call %u %s %s: state %u\n", best.id, best.dir == KOS_CALL_IN ? "from" : "to",
			     best.number, best.state);
		call = best;
		call_seq++;
	}
}

/* --- the services ---------------------------------------------------------- */

static void setup(void)
{
	set_status("setting the modem up");
	/* DMS: online */
	begin(SVC_DMS, DMS_GET_MODE);
	send_req(SVC_DMS, 0);
	/* NAS: serving system changes (with the network's time) */
	begin(SVC_NAS, NAS_REGISTER);
	qmi_put_u8(&qb, 0x13, 1);
	send_req(SVC_NAS, 0);
	/* WMS: messages stored on the modem, a notification for each */
	begin(SVC_WMS, WMS_SET_ROUTES);
	u8 routes[2 + 5 * 4];
	put16(routes, 5);
	for (int c = 0; c < 5; c++) {
		u8 *e = routes + 2 + 4 * c;
		e[0] = 0;                          /* point to point */
		e[1] = c;                          /* class 0..3, none */
		e[2] = c == 2 ? 0 : 1;             /* class 2 on the SIM, the others in the modem */
		e[3] = 1;                          /* store and notify */
	}
	qmi_put(&qb, 0x01, routes, sizeof(routes));
	send_req(SVC_WMS, 0);
	begin(SVC_WMS, WMS_EVENT_REPORT);
	qmi_put_u8(&qb, 0x10, 1);
	send_req(SVC_WMS, 0);
	for (u8 storage = 0; storage < 2; storage++) {
		begin(SVC_WMS, WMS_LIST);
		qmi_put_u8(&qb, 0x01, storage);
		qmi_put_u8(&qb, 0x12, 1);
		send_req(SVC_WMS, storage);
	}
	/* VOICE: call indications, the calls there are */
	begin(SVC_VOICE, VOICE_REGISTER);
	qmi_put_u8(&qb, 0x12, 1);
	qmi_put_u8(&qb, 0x13, 1);
	send_req(SVC_VOICE, 0);
	begin(SVC_VOICE, VOICE_GET_ALL);
	send_req(SVC_VOICE, 0);
	next_poll = now;
}

static void poll_network(void)
{
	begin(SVC_NAS, NAS_SERVING);
	send_req(SVC_NAS, 0);
	begin(SVC_NAS, NAS_SIGNAL);
	send_req(SVC_NAS, 0);
}

static void serving_system(const struct qmi_msg *m, bool ind)
{
	u32 n;
	const u8 *v = qmi_tlv(m, 0x01, &n);
	if (v && n >= 1) {
		u32 reg = v[0] <= 4 ? v[0] : KOS_REG_UNKNOWN;
		const u8 *roam = qmi_tlv(m, 0x10, &n);
		if (reg == KOS_REG_HOME && roam && n >= 1 && roam[0] == 0) reg = KOS_REG_ROAMING;
		registration = reg;
	}
	v = qmi_tlv(m, 0x12, &n);
	if (v && n >= 5) {
		u32 l = MIN((u32)v[4], n - 5);
		l = MIN(l, (u32)sizeof(operator_name) - 1);
		if (l) {
			mcopy(operator_name, v + 5, l);
			operator_name[l] = 0;
		} else {
			/* no name: MCC-MNC */
			u32 mcc = get16(v), mnc = get16(v + 2), o = 0;
			char t[16];
			u32 x[2] = { mcc, mnc };
			for (int k = 0; k < 2; k++) {
				char d[6];
				int nd = 0;
				u32 val = x[k];
				do { d[nd++] = '0' + val % 10; val /= 10; } while (val && nd < 5);
				if (k == 1 && nd < 2) d[nd++] = '0';
				while (nd) t[o++] = d[--nd];
				if (!k) t[o++] = '-';
			}
			t[o] = 0;
			mstrcpy(operator_name, t, sizeof(operator_name));
		}
	}
	/* the network's time (indication: 0x1c) */
	v = ind ? qmi_tlv(m, 0x1c, &n) : 0;
	if (v && n >= 8 && hw->set_clock) {
		u32 y = get16(v), mo = v[2], d = v[3], h = v[4], mi = v[5], s = v[6];
		if (y >= 2020 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31) {
			int yy = y - (mo <= 2), era = yy / 400, yoe = yy - era * 400;
			int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
			u32 days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
			hw->set_clock(days * 86400 + h * 3600 + mi * 60 + s);
		}
	}
}

static void response(int svc, struct req *r, const struct qmi_msg *m)
{
	u16 err;
	bool ok = qmi_ok(m, &err);
	u32 n;
	const u8 *v;
	switch (svc << 16 | r->msg) {
	case SVC_DMS << 16 | DMS_GET_MODE:
		v = qmi_tlv(m, 0x01, &n);
		online = ok && v && n >= 1 && v[0] == 0;
		if (ok && v && v[0] != 0) {
			mlog("modem: operating mode %u: online\n", v[0]);
			begin(SVC_DMS, DMS_SET_MODE);
			qmi_put_u8(&qb, 0x01, 0);
			send_req(SVC_DMS, 0);
		}
		break;
	case SVC_DMS << 16 | DMS_SET_MODE:
		if (ok) online = 1;
		else mlog("modem: online refused (%u)\n", err);
		break;
	case SVC_NAS << 16 | NAS_SERVING:
		if (ok) serving_system(m, false);
		break;
	case SVC_NAS << 16 | NAS_SIGNAL:
		v = qmi_tlv(m, 0x01, &n);
		if (ok && v && n >= 1) signal_dbm = (s8)v[0];
		break;
	case SVC_WMS << 16 | WMS_LIST:
		v = qmi_tlv(m, 0x01, &n);
		if (ok && v && n >= 4) {
			u32 count = get32(v);
			mlog("modem: %u messages stored (%s)\n", count, r->tag ? "modem" : "SIM");
			for (u32 i = 0; i < count && 4 + 5 * (i + 1) <= n; i++) {
				u32 index = get32(v + 4 + 5 * i);
				if (!have_stored(r->tag, index)) read_stored(r->tag, index);
			}
		}
		break;
	case SVC_WMS << 16 | WMS_RAW_READ:
		v = qmi_tlv(m, 0x01, &n);
		if (ok && v && n >= 4) {
			u32 len = MIN((u32)get16(v + 2), n - 4);
			if (!have_stored(r->tag >> 24, r->tag & 0xffffff))
				got_raw(v + 4, len, r->tag >> 24, r->tag & 0xffffff, v[0]);
		}
		break;
	case SVC_WMS << 16 | WMS_RAW_SEND:
		for (int i = 0; i < SMS_MAX; i++) {
			struct rec *s = &recs[i];
			if (!s->used || s->id != r->tag || s->state != KOS_SMS_SENDING) continue;
			if (ok) s->parts_done++;
			else s->parts_failed++;
			if (s->parts_done + s->parts_failed == s->parts)
				s->state = s->parts_failed ? KOS_SMS_FAILED : KOS_SMS_SENT;
			if (!ok) mlog("modem: sending to %s failed (%u)\n", s->number, err);
			sms_seq++;
		}
		break;
	case SVC_VOICE << 16 | VOICE_GET_ALL: {
		u32 nn = 0;
		v = qmi_tlv(m, 0x10, &n);
		const u8 *num = qmi_tlv(m, 0x11, &nn);
		if (ok) calls(v, v ? n : 0, num, num ? nn : 0);
		break;
	}
	case SVC_VOICE << 16 | VOICE_DIAL:
		v = qmi_tlv(m, 0x10, &n);
		if (ok && v && n >= 1) {
			call.id = v[0];
			call_seq++;
		} else {
			mlog("modem: dialling failed (%u)\n", err);
			call.state = KOS_CALL_ENDED;
			ended_at = now;
			call_seq++;
		}
		break;
	default:
		if (!ok) mlog("modem: %s message 0x%x: error %u\n", svc_name[svc], r->msg, err);
		break;
	}
}

static void indication(int svc, const struct qmi_msg *m)
{
	u32 n, nn;
	const u8 *v;
	switch (svc << 16 | m->id) {
	case SVC_WMS << 16 | WMS_EVENT_REPORT:
		v = qmi_tlv(m, 0x10, &n);
		if (v && n >= 5) {
			mlog("modem: a new message (%u/%u)\n", v[0], get32(v + 1));
			read_stored(v[0], get32(v + 1));
		}
		v = qmi_tlv(m, 0x11, &n);
		if (v && n >= 8) {                     /* transferred, not stored */
			u32 len = MIN((u32)get16(v + 6), n - 8);
			got_raw(v + 8, len, 0xff, 0xffffffffu, 1);
			if (v[0] == 0) {                   /* it wants an acknowledgement */
				begin(SVC_WMS, WMS_SEND_ACK);
				u8 a[6];
				put32(a, get32(v + 1));
				a[4] = 1;                      /* 3GPP */
				a[5] = 1;                      /* success */
				qmi_put(&qb, 0x01, a, 6);
				send_req(SVC_WMS, 0);
			}
		}
		break;
	case SVC_VOICE << 16 | VOICE_ALL_STATUS:
		v = qmi_tlv(m, 0x01, &n);
		{
			const u8 *num = qmi_tlv(m, 0x10, &nn);
			calls(v, v ? n : 0, num, num ? nn : 0);
		}
		break;
	case SVC_NAS << 16 | NAS_SERVING:
		serving_system(m, true);
		break;
	default:
		break;
	}
}

static void svc_rx(void *arg, u32 node, u32 port, const u8 *data, u32 len)
{
	int svc = (int)(long)arg;
	struct qmi_msg m;
	if (!qmi_parse(data, len, &m)) return;
	if (m.type == QMI_INDICATION) {
		indication(svc, &m);
		return;
	}
	if (m.type != QMI_RESPONSE) return;
	for (int i = 0; i < NREQ; i++) {
		struct req *r = &reqs[i];
		if (r->used && r->svc == svc && r->txn == m.txn) {
			r->used = false;
			response(svc, r, &m);
			return;
		}
	}
	(void)node; (void)port;
}

static void qrtr_event(void *arg, int ev, const struct qrtr_server *s, u32 node, u32 port)
{
	(void)arg;
	switch (ev) {
	case QRTR_EV_HELLO:
		if (st < ST_SERVICES) {
			st = ST_SERVICES;
			st_at = now;
			set_status("looking for the modem's services");
		}
		break;
	case QRTR_EV_BYE:
		set_status("the modem went away (BYE)");
		for (int i = 0; i < NSVC; i++) cl[i].have = false;
		st = ST_FAILED;
		break;
	case QRTR_EV_NEW_SERVER:
		if (s->service == WLFW_SVC && hw->wlfw) wlfw_server(s->node, s->port);
		for (int i = 0; i < NSVC; i++)
			if (s->service == svc_id[i] && !cl[i].have) {
				cl[i].have = true;
				cl[i].node = s->node;
				cl[i].port = s->port;
				mlog("modem: %s at %u:%u\n", svc_name[i], s->node, s->port);
			}
		break;
	case QRTR_EV_DEL_SERVER:
		for (int i = 0; i < NSVC; i++)
			if (cl[i].have && cl[i].node == s->node && cl[i].port == s->port) cl[i].have = false;
		break;
	case QRTR_EV_DEL_CLIENT:
		rmtfs_client_gone(&rmtfs, node, port);
		break;
	}
}

/* --- the link and the work ------------------------------------------------- */

/* a QRTR packet (version 1) of the apps processor's, or to it */
static bool qrtr_packet(const u8 *d, u32 len)
{
	return len >= QRTR_HDR_V1 && get32(d) == 1 && get32(d + 4) >= QRTR_TYPE_DATA &&
	       get32(d + 4) <= QRTR_TYPE_DEL_LOOKUP;
}

static bool qrtr_from_apps(const u8 *d, u32 len)
{
	return qrtr_packet(d, len) && get32(d + 8) == QRTR_NODE_APPS;
}

static bool qrtr_to_apps(const u8 *d, u32 len)
{
	return qrtr_packet(d, len) && (get32(d + 24) == QRTR_NODE_APPS || get32(d + 24) == QRTR_NODE_BCAST);
}

static void kick(void)
{
	if (hw->kick) hw->kick();
}

void modem_start(const struct modem_hw *h)
{
	hw = h;
	st = ST_FAILED;
	if (smem_init(&smem, h->smem, h->smem_size, SMEM_HOST_APPS, SMEM_HOST_MODEM) < 0) {
		set_status("no shared memory with the modem (SMEM)");
		return;
	}
	if (glink_init(&glink, &smem, kick) < 0) {
		set_status("no GLINK FIFOs in SMEM");
		return;
	}
	/* Linux's IPCRTR left open (tools/pro1x-kexec.sh -m: unbinding
	 * qrtr-smd from it hangs Linux 7.1): its ids from the QRTR packets
	 * still in the FIFOs, the channel taken over; else opened anew */
	ipcrtr = 0;
	adopted = false;
	if (h->adopt_ipcrtr) {
		s32 l = glink_scan_data(&glink, true, qrtr_from_apps), r = glink_scan_data(&glink, false, qrtr_to_apps);
		if (l >= 0 && r >= 0) ipcrtr = glink_adopt(&glink, "IPCRTR", l, r, qrtr_glink_rx, &qrtr);
		adopted = ipcrtr != 0;
		if (adopted) mlog("modem: Linux's IPCRTR taken over (lcid %d, rcid %d)\n", l, r);
		else mlog("modem: Linux's IPCRTR not in the FIFOs: opened anew\n");
	}
	if (!ipcrtr) ipcrtr = glink_open(&glink, "IPCRTR", LCID_IPCRTR, qrtr_glink_rx, &qrtr);
	qrtr_init(&qrtr, ipcrtr, QRTR_NODE_APPS, qrtr_event, 0);
	for (int i = 0; i < NSVC; i++) qrtr_bind(&qrtr, PORT_BASE + i, svc_rx, (void *)(long)i);
	rmtfs_init(&rmtfs, &qrtr, h->rmtfs_mem, h->rmtfs_pa, h->rmtfs_size, h->efs, h->efs_size);
	if (h->wlfw) wlfw_init(&qrtr);
	st = ST_OPEN;
	st_at = now;
	set_status("opening the modem's IPC router channel");
}

bool modem_ipcrtr_adopted(void)
{
	return adopted;
}

bool modem_present(void)
{
	return hw != 0;
}

void modem_poll(u32 ms)
{
	now = ms;
	if (!hw || st == ST_OFF) return;
	glink_poll(&glink);
	switch (st) {
	case ST_OPEN:
		if (glink_ready(ipcrtr)) {
			st = ST_HELLO;
			st_at = now;
			set_status("waiting for the modem's HELLO");
		} else if (now - st_at > 10000) {
			set_status("the modem does not open IPCRTR");
			st_at = now;
		}
		break;
	case ST_HELLO:
		/* the remote says HELLO first; after a while we do */
		if (now - st_at > 3000 && !qrtr.hello) {
			qrtr_send_ctrl(&qrtr, QRTR_TYPE_HELLO, 0, 0);
			st_at = now;
		}
		break;
	case ST_SERVICES: {
		int have = 0;
		for (int i = 0; i < NSVC; i++) have += cl[i].have;
		if (have == NSVC || (have && now - st_at > 5000)) {
			setup();
			st = ST_READY;
			set_status(have == NSVC ? "ready" : "ready (not all services there)");
		}
		break;
	}
	case ST_READY:
		if ((s32)(now - next_poll) >= 0) {
			poll_network();
			next_poll = now + poll_ms;
		}
		break;
	default:
		break;
	}
	/* requests without an answer */
	for (int i = 0; i < NREQ; i++)
		if (reqs[i].used && now - reqs[i].sent > 15000) {
			mlog("modem: no answer from %s to 0x%x\n", svc_name[reqs[i].svc], reqs[i].msg);
			reqs[i].used = false;
		}
	/* an ended call shown for a while, then gone */
	if (call.state == KOS_CALL_ENDED && now - ended_at > 4000) {
		mzero(&call, sizeof(call));
		call_seq++;
	}
	glink_poll(&glink);
}

/* the network's poll (registration, signal) every ms: 5 s, slower while
 * nobody looks (the display off); faster again at once */
void modem_poll_interval(u32 ms)
{
	if (ms == poll_ms) return;
	if (ms < poll_ms && (s32)(next_poll - (now + ms)) > 0) next_poll = now + ms;
	poll_ms = ms;
}

/* tickless idle: milliseconds from ms until modem_poll has work of its
 * own (its setup, an answer awaited, a call, the next network poll);
 * what the modem sends comes with its interrupt (or the next poll).
 * 0xffffffff: none */
u32 modem_next_ms(u32 ms)
{
	if (!hw || st == ST_OFF || st == ST_FAILED) return 0xffffffffu;
	if (st != ST_READY || call.state != KOS_CALL_NONE) return 0;
	for (int i = 0; i < NREQ; i++)
		if (reqs[i].used) return 0;
	s32 d = (s32)(next_poll - ms);
	return d > 0 ? (u32)d : 0;
}

/* --- for the programs ------------------------------------------------------- */

void modem_status(struct kos_modem *m)
{
	mzero(m, sizeof(*m));
	if (!hw) {
		mstrcpy(m->status, "no modem", sizeof(m->status));
		return;
	}
	m->state = st == ST_READY ? KOS_MODEM_READY : st == ST_FAILED ? KOS_MODEM_FAILED :
		   st == ST_SERVICES ? KOS_MODEM_SERVICES : KOS_MODEM_STARTING;
	m->registration = registration;
	m->signal = signal_dbm;
	m->online = online;
	m->sms_seq = sms_seq;
	m->call_seq = call_seq;
	m->fake = hw->fake;
	for (int i = 0; i < SMS_MAX; i++)
		if (recs[i].used) {
			m->sms_count++;
			if (recs[i].state == KOS_SMS_UNREAD) m->sms_unread++;
		}
	mstrcpy(m->operator_name, operator_name, sizeof(m->operator_name));
	mstrcpy(m->status, status, sizeof(m->status));
}

/* the messages in the order they came */
static struct rec *nth(u32 index)
{
	struct rec *list[SMS_MAX];
	int n = 0;
	for (int i = 0; i < SMS_MAX; i++)
		if (recs[i].used) {
			int j = n++;
			while (j > 0 && list[j - 1]->id > recs[i].id) { list[j] = list[j - 1]; j--; }
			list[j] = &recs[i];
		}
	return index < (u32)n ? list[index] : 0;
}

int modem_sms(u32 index, struct kos_sms *s)
{
	struct rec *r = nth(index);
	if (!r) return -1;
	mzero(s, sizeof(*s));
	s->id = r->id;
	s->time = r->time;
	s->dir = r->dir;
	s->state = r->state;
	mstrcpy(s->number, r->number, sizeof(s->number));
	mstrcpy(s->text, r->text, sizeof(s->text));
	return 0;
}

int modem_send(const char *number, const char *text)
{
	if (st != ST_READY || !cl[SVC_WMS].have || !number[0]) return -1;
	static u8 pdus[6][SMS_PDU_MAX];
	u32 len[6];
	static u8 ref;
	int parts = sms_encode(number, text, ++ref, pdus, len, 6);
	if (!parts) return -1;
	struct rec *r = rec_new();
	r->dir = KOS_SMS_OUT;
	r->state = KOS_SMS_SENDING;
	r->parts = parts;
	r->time = hw->clock ? hw->clock() : 0;
	mstrcpy(r->number, number, sizeof(r->number));
	mstrcpy(r->text, text, sizeof(r->text));
	for (int i = 0; i < parts; i++) {
		begin(SVC_WMS, WMS_RAW_SEND);
		u8 raw[3 + SMS_PDU_MAX];
		raw[0] = 6;                            /* GSM/WCDMA point to point */
		put16(raw + 1, len[i]);
		mcopy(raw + 3, pdus[i], len[i]);
		qmi_put(&qb, 0x01, raw, 3 + len[i]);
		if (send_req(SVC_WMS, r->id) < 0) r->parts_failed++;
	}
	if (r->parts_failed == parts) r->state = KOS_SMS_FAILED;
	sms_seq++;
	return 0;
}

int modem_dial(const char *number)
{
	if (st != ST_READY || !cl[SVC_VOICE].have || !number[0]) return -1;
	if (call.state != KOS_CALL_NONE && call.state != KOS_CALL_ENDED) return -1;
	begin(SVC_VOICE, VOICE_DIAL);
	qmi_put(&qb, 0x01, number, mstrlen(number));
	mzero(&call, sizeof(call));
	call.state = KOS_CALL_DIALING;
	call.dir = KOS_CALL_OUT;
	call.id = 0xff;
	mstrcpy(call.number, number, sizeof(call.number));
	call_seq++;
	return send_req(SVC_VOICE, 0) < 0 ? -1 : 0;
}

int modem_answer(void)
{
	if (call.state != KOS_CALL_INCOMING) return -1;
	begin(SVC_VOICE, VOICE_ANSWER);
	qmi_put_u8(&qb, 0x01, call.id);
	return send_req(SVC_VOICE, 0) < 0 ? -1 : 0;
}

int modem_hangup(void)
{
	if (call.state == KOS_CALL_NONE || call.state == KOS_CALL_ENDED || call.id == 0xff) return -1;
	begin(SVC_VOICE, VOICE_END);
	qmi_put_u8(&qb, 0x01, call.id);
	return send_req(SVC_VOICE, 0) < 0 ? -1 : 0;
}

void modem_call(struct kos_call *c)
{
	mzero(c, sizeof(*c));
	c->id = call.id;
	c->state = call.state;
	c->dir = call.dir;
	c->seconds = call.state == KOS_CALL_ACTIVE ? (now - call.started) / 1000 : 0;
	mstrcpy(c->number, call.number, sizeof(c->number));
}

int modem_delete(u32 index)
{
	struct rec *r = nth(index);
	if (!r) return -1;
	if (r->index != 0xffffffffu && cl[SVC_WMS].have && r->storage <= 1) {
		begin(SVC_WMS, WMS_DELETE);
		qmi_put_u8(&qb, 0x01, r->storage);
		qmi_put_u32(&qb, 0x10, r->index);
		qmi_put_u8(&qb, 0x12, 1);
		send_req(SVC_WMS, 0);
	}
	r->used = false;
	sms_seq++;
	return 0;
}

int modem_mark_read(u32 index)
{
	struct rec *r = nth(index);
	if (!r) return -1;
	if (r->state == KOS_SMS_UNREAD) {
		r->state = KOS_SMS_READ;
		if (r->index != 0xffffffffu && cl[SVC_WMS].have && r->storage <= 1) {
			/* Modify Tag: read */
			begin(SVC_WMS, WMS_MODIFY_TAG);
			u8 t[6];
			t[0] = r->storage;
			put32(t + 1, r->index);
			t[5] = 0;
			qmi_put(&qb, 0x01, t, 6);
			qmi_put_u8(&qb, 0x10, 1);
			send_req(SVC_WMS, 0);
		}
		sms_seq++;
	}
	return 0;
}
