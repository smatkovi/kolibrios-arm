/* KolibriOS for ARM: the RPM's GLINK edge and its requests (rpm.h).
 *
 * The message RAM ends in a table of contents ("grt0"): the FIFOs
 * "ap2r" (to the RPM) and "r2ap", each a tail and a head word, then the
 * data. GLINK commands in them are 8-byte aligned; data goes as TX_DATA
 * (16-byte header: command 9, the channel id, intent 0, the chunk's size,
 * what is left). A request: "req\0", its length, then the message id,
 * the set (0: active, 1: sleep; smd-rpm.h), the resource's type and id, the size of the
 * key/value pairs, the pairs (key, 4, value). The answer: "req\0", then
 * "msg#" with the id, and "err\0" with a text when refused. */
#include "rpm.h"

#define TOC_SIZE 256
#define TOC_MAGIC 0x67727430u                 /* "grt0" */
#define TX_ID 0x61703272u                     /* "ap2r" */
#define RX_ID 0x72326170u                     /* "r2ap" */
#define CMD_TX_DATA 9
#define REQ 0x00716572u                       /* "req\0" */
#define MSG_ID 0x2367736du                    /* "msg#" */
#define ERR 0x00727265u                       /* "err\0" */

struct fifo { volatile u8 *tail, *head, *data; u32 len; };
static struct fifo tx, rx;
static void (*kick)(void), (*delay)(u32);
static u32 lcid = 1, msg_id;
static bool ready;

static u32 rd(const struct fifo *f, u32 off)
{
	return sm_rd32(f->data + off % f->len);
}

static void wr(struct fifo *f, u32 off, u32 v)
{
	sm_wr32(f->data + off % f->len, v);
}

u32 rpm_lcid(void)
{
	return lcid;
}

bool rpm_ready(void)
{
	return ready;
}

/* the channel id Linux sent its requests with: the TX_DATA records
 * still in the FIFO */
static void find_lcid(void)
{
	u32 count[16] = { 0 };
	for (u32 o = 0; o < tx.len; o += 8) {
		u32 w = rd(&tx, o);
		u32 chunk = rd(&tx, o + 8);
		if ((w & 0xffff) == CMD_TX_DATA && (w >> 16) < 16 && rd(&tx, o + 16) == REQ &&
		    chunk >= 28 && chunk < 256 && rd(&tx, o + 12) == 0)
			count[w >> 16]++;
	}
	u32 best = 0;
	for (u32 i = 1; i < 16; i++)
		if (count[i] > count[best]) best = i;
	if (count[best]) lcid = best;
	mlog("rpm: Linux's channel id %u (%u of its requests seen)\n", lcid, count[best]);
}

int rpm_init(volatile u8 *ram, u32 size, void (*k)(void), void (*d)(u32))
{
	volatile u8 *toc = ram + size - TOC_SIZE;
	kick = k;
	delay = d;
	if (size < TOC_SIZE || sm_rd32(toc) != TOC_MAGIC) {
		mlog("rpm: no table of contents in the message RAM\n");
		return -1;
	}
	u32 n = sm_rd32(toc + 4);
	tx.len = rx.len = 0;
	for (u32 i = 0; i < n && 8 + 12 * (i + 1) <= TOC_SIZE; i++) {
		u32 id = sm_rd32(toc + 8 + 12 * i), off = sm_rd32(toc + 12 + 12 * i), len = sm_rd32(toc + 16 + 12 * i);
		if (off + 8 + len > size || len < 64 || (len & 7)) continue;
		struct fifo *f = id == TX_ID ? &tx : id == RX_ID ? &rx : NULL;
		if (!f) continue;
		f->tail = ram + off;
		f->head = ram + off + 4;
		f->data = ram + off + 8;
		f->len = len;
	}
	if (!tx.len || !rx.len) {
		mlog("rpm: no FIFOs in the table of contents\n");
		return -1;
	}
	find_lcid();
	/* what the RPM sent before (Linux read it): not ours */
	sm_wr32(rx.tail, sm_rd32(rx.head));
	ready = true;
	mlog("rpm: FIFOs of %u and %u bytes\n", tx.len, rx.len);
	return 0;
}

static u32 tx_room(void)
{
	u32 h = sm_rd32(tx.head), t = sm_rd32(tx.tail);
	return t <= h ? tx.len - h + t : t - h;
}

/* the answer to request id: 1 acknowledged, -2 refused, 0 none yet */
static int answer(u32 id)
{
	for (;;) {
		u32 h = sm_rd32(rx.head), t = sm_rd32(rx.tail);
		u32 avail = h >= t ? h - t : rx.len - t + h;
		if (avail < 8 || h >= rx.len || t >= rx.len) return 0;
		u32 w = rd(&rx, t), cmd = w & 0xffff, adv = 8;
		int r = 0;
		if (cmd == CMD_TX_DATA) {
			u32 chunk = rd(&rx, t + 8);
			adv = M_ALIGN(16 + chunk, 8);
			if (avail < adv) return 0;
			if (chunk >= 8 && rd(&rx, t + 16) == REQ) {
				u32 len = MIN(rd(&rx, t + 20), chunk - 8), o = 0;
				bool mine = false, err = false;
				char text[32];
				while (o + 8 <= len) {
					u32 type = rd(&rx, t + 24 + o), ml = rd(&rx, t + 28 + o);
					if (type == MSG_ID && ml >= 4) mine = rd(&rx, t + 32 + o) == id;
					if (type == ERR) {
						err = true;
						u32 k;
						for (k = 0; k < ml && k + 1 < sizeof(text); k++)
							text[k] = (char)(rd(&rx, t + 32 + o + (k & ~3u)) >> (8 * (k & 3)));
						text[k] = 0;
						mlog("rpm: refused: %s\n", text);
					}
					o += 8 + M_ALIGN(ml, 4);
				}
				if (mine) r = err ? -2 : 1;
			}
		} else if (cmd == 2) {                        /* OPEN: its name after it */
			adv = M_ALIGN(8 + (rd(&rx, t + 4) & 0xffff), 8);
		}
		sm_wr32(rx.tail, (t + adv) % rx.len);
		if (r) return r;
	}
}

int rpm_write(u32 type, u32 id, const u32 *keys, const u32 *values, int n)
{
	return rpm_write_set(RPM_ACTIVE, type, id, keys, values, n);
}

int rpm_write_set(u32 set, u32 type, u32 id, const u32 *keys, const u32 *values, int n)
{
	if (!ready || n > 8) return -1;
	u32 m[7 + 3 * 8];
	u32 dlen = 4 * (7 + 3 * n);
	m[0] = REQ;
	m[1] = dlen - 8;
	m[2] = ++msg_id;
	m[3] = set;                                       /* the active set (0) or the sleep set (1) */
	m[4] = type;
	m[5] = id;
	m[6] = 12 * n;
	for (int i = 0; i < n; i++) {
		m[7 + 3 * i] = keys[i];
		m[8 + 3 * i] = 4;
		m[9 + 3 * i] = values[i];
	}
	u32 total = M_ALIGN(16 + dlen, 8);
	if (tx_room() <= total) return -1;
	u32 h = sm_rd32(tx.head);
	wr(&tx, h, CMD_TX_DATA | lcid << 16);
	wr(&tx, h + 4, 0);
	wr(&tx, h + 8, dlen);
	wr(&tx, h + 12, 0);
	for (u32 i = 0; i < dlen / 4; i++) wr(&tx, h + 16 + 4 * i, m[i]);
	if (total > 16 + dlen) wr(&tx, h + 16 + dlen, 0);
	sm_barrier();
	sm_wr32(tx.head, (h + total) % tx.len);
	sm_barrier();
	if (kick) kick();
	for (int i = 0; i < 2000; i++) {                  /* 200 ms */
		int r = answer(m[2]);
		if (r) return r > 0 ? 0 : -2;
		if (delay) delay(100);
	}
	mlog("rpm: no answer to request %u (%x %u)\n", m[2], type, id);
	return -1;
}

int rpm_ldo(u32 ldo, u32 uv, bool on)
{
	u32 k[2] = { RPM_KEY_UV, RPM_KEY_SWEN }, v[2] = { uv, on };
	int r = rpm_write(RPM_LDOA, ldo, uv ? k : k + 1, uv ? v : v + 1, uv ? 2 : 1);
	mlog("rpm: L%u %s at %u uV: %s\n", ldo, on ? "on" : "off", uv, r == 0 ? "done" : r == -2 ? "refused" : "no answer");
	return r;
}
