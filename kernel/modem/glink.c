/* KolibriOS for ARM: GLINK over SMEM, the apps processor's side.
 *
 * Two ring buffers in SMEM, one each way, with their tail and head
 * indices in a descriptor item; every command an 8-byte header (cmd,
 * param1, param2), what follows it, the whole padded to 8 bytes. A
 * channel is open when both sides have sent OPEN and had it acknowledged;
 * data goes into a receive buffer ("intent") the other side advertised,
 * which comes back with RX_DONE. The link itself (VERSION) was
 * negotiated when the remote started: answered here if the remote asks.
 * After the write the remote is told by an interrupt (kick). */
#include "glink.h"

#define FIFO_RESERVE 16

static int memcmp_m(const u8 *a, const char *b, u32 n)
{
	for (u32 i = 0; i < n; i++)
		if (a[i] != (u8)b[i]) return 1;
	return 0;
}        /* kept free: FIFO_FULL_RESERVE + TX_BLOCKED_CMD_RESERVE */

u32 glink_fifo_avail(u32 head, u32 tail, u32 len)
{
	return head >= tail ? head - tail : len - tail + head;
}

void glink_fifo_peek(const volatile u8 *fifo, u32 len, u32 tail, u32 off, void *dst, u32 n)
{
	u32 t = tail + off;
	if (t >= len) t -= len;
	u32 a = MIN(n, len - t);
	sm_copy_from(dst, fifo + t, a);
	if (a < n) sm_copy_from((u8 *)dst + a, fifo, n - a);
}

u32 glink_fifo_write(volatile u8 *fifo, u32 len, u32 head, const void *src, u32 n)
{
	u32 a = MIN(n, len - head);
	sm_copy_to(fifo + head, src, a);
	if (a < n) sm_copy_to(fifo, (const u8 *)src + a, n - a);
	head += n;
	if (head >= len) head -= len;
	return head;
}

static u32 tx_free(struct glink *g)
{
	u32 tail = sm_rd32(g->desc), head = sm_rd32(g->desc + 4);
	u32 used = glink_fifo_avail(head, tail, g->tx_len);
	u32 free = g->tx_len - used;
	return free > FIFO_RESERVE ? free - FIFO_RESERVE : 0;
}

static bool rx_fifo(struct glink *g)
{
	if (g->rx_fifo) return true;
	u32 n;
	volatile u8 *f = smem_get(g->smem, GLINK_ITEM_FIFO1, &n);
	if (!f || n < 64) return false;
	g->rx_fifo = f;
	g->rx_len = n;
	return true;
}

static u32 rx_avail(struct glink *g)
{
	if (!rx_fifo(g)) return 0;
	u32 tail = sm_rd32(g->desc + 8), head = sm_rd32(g->desc + 12);
	if (tail >= g->rx_len || head >= g->rx_len) return 0;
	return glink_fifo_avail(head, tail, g->rx_len);
}

static void rx_peek(struct glink *g, u32 off, void *dst, u32 n)
{
	glink_fifo_peek(g->rx_fifo, g->rx_len, sm_rd32(g->desc + 8), off, dst, n);
}

static void rx_advance(struct glink *g, u32 n)
{
	u32 t = sm_rd32(g->desc + 8) + M_ALIGN(n, 8);
	if (t >= g->rx_len) t -= g->rx_len;
	sm_wr32(g->desc + 8, t);
}

/* the first queued command into the FIFO, if it fits; true: written */
static bool flush_one(struct glink *g)
{
	struct glink_out *o = &g->out[g->out_first];
	u32 n = M_ALIGN(o->len, 8);
	if (tx_free(g) < n) return false;
	u32 head = sm_rd32(g->desc + 4);
	head = glink_fifo_write(g->tx_fifo, g->tx_len, head, o->d, n);
	sm_barrier();
	sm_wr32(g->desc + 4, head);
	g->out_first = (g->out_first + 1) % GLINK_OUTQ;
	g->out_n--;
	g->kick_due = true;
	return true;
}

static void flush(struct glink *g)
{
	while (g->out_n && flush_one(g))
		;
	if (g->kick_due) {
		g->kick_due = false;
		if (g->kick) g->kick();
	}
}

/* a command, its payload after it: queued in order, written when there is room */
static int tx(struct glink *g, const void *hdr, u32 hlen, const void *data, u32 dlen)
{
	if (hlen + dlen > sizeof(g->out[0].d) - 8 || g->out_n == GLINK_OUTQ) {
		g->errors++;
		return -1;
	}
	struct glink_out *o = &g->out[(g->out_first + g->out_n) % GLINK_OUTQ];
	mcopy(o->d, hdr, hlen);
	if (dlen) mcopy(o->d + hlen, data, dlen);
	o->len = hlen + dlen;
	mzero(o->d + o->len, M_ALIGN(o->len, 8) - o->len);
	g->out_n++;
	return 0;
}

static int cmd(struct glink *g, u16 c, u16 p1, u32 p2)
{
	u8 h[8];
	put16(h, c);
	put16(h + 2, p1);
	put32(h + 4, p2);
	return tx(g, h, 8, 0, 0);
}

static struct glink_chan *by_lcid(struct glink *g, u16 lcid)
{
	for (int i = 0; i < GLINK_MAX_CHAN; i++)
		if (g->ch[i].used && g->ch[i].lcid == lcid) return &g->ch[i];
	return 0;
}

static struct glink_chan *by_rcid(struct glink *g, u16 rcid)
{
	for (int i = 0; i < GLINK_MAX_CHAN; i++)
		if (g->ch[i].used && g->ch[i].remote_open && g->ch[i].rcid == rcid) return &g->ch[i];
	return 0;
}

static struct glink_chan *by_name(struct glink *g, const char *name)
{
	for (int i = 0; i < GLINK_MAX_CHAN; i++)
		if (g->ch[i].used && mstreq(g->ch[i].name, name)) return &g->ch[i];
	return 0;
}

static u8 *arena_alloc(struct glink *g, u32 n)
{
	n = M_ALIGN(n, 8);
	if (g->arena_used + n > GLINK_ARENA) return 0;
	u8 *p = g->arena + g->arena_used;
	g->arena_used += n;
	return p;
}

/* a receive buffer of size for the channel, advertised; NULL: no memory */
static struct glink_lintent *add_intent(struct glink_chan *c, u32 size, bool reuse)
{
	struct glink *g = c->g;
	struct glink_lintent *li = 0;
	/* a free one of that size first (asked for again) */
	for (int i = 0; i < GLINK_LINTENTS && !li; i++)
		if (c->li[i].valid && !c->li[i].reuse && !c->li[i].busy && c->li[i].size >= size)
			li = &c->li[i];
	if (!li) {
		for (int i = 0; i < GLINK_LINTENTS && !li; i++)
			if (!c->li[i].valid) li = &c->li[i];
		if (!li) return 0;
		u8 *buf = arena_alloc(g, size);
		if (!buf) return 0;
		li->buf = buf;
		li->size = size;
		li->valid = true;
	}
	li->id = g->next_liid++;
	li->reuse = reuse;
	li->busy = false;
	li->off = 0;
	u8 m[16];
	put16(m, GLINK_CMD_INTENT);
	put16(m + 2, c->lcid);
	put32(m + 4, 1);
	put32(m + 8, li->size);
	put32(m + 12, li->id);
	tx(g, m, 16, 0, 0);
	return li;
}

static void send_intents(struct glink_chan *c)
{
	if (c->intents_sent) return;
	c->intents_sent = true;
	for (int i = 0; i < 8; i++)
		add_intent(c, 4096, true);
}

/* --- sending data ------------------------------------------------------ */

static struct glink_rintent *pick_rintent(struct glink_chan *c, u32 len)
{
	struct glink_rintent *best = 0;
	for (int i = 0; i < GLINK_RINTENTS; i++) {
		struct glink_rintent *r = &c->ri[i];
		if (r->valid && !r->used && r->size >= len && (!best || r->size < best->size)) best = r;
	}
	return best;
}

static int send_data(struct glink_chan *c, const u8 *data, u32 len)
{
	struct glink *g = c->g;
	struct glink_rintent *r = pick_rintent(c, len);
	if (!r) return 1;
	if (g->out_n + 1 > GLINK_OUTQ) return 1;
	r->used = true;
	u8 h[16];
	put16(h, GLINK_CMD_TX_DATA);
	put16(h + 2, c->lcid);
	put32(h + 4, r->id);
	put32(h + 8, len);
	put32(h + 12, 0);
	if (tx(g, h, 16, data, len) < 0) {
		r->used = false;
		return -1;
	}
	c->tx_msgs++;
	return 0;
}

/* messages waiting for an intent: out in order, or an intent asked for */
static void send_pending(struct glink_chan *c)
{
	while (c->npend) {
		int r = send_data(c, c->pend[0].data, c->pend[0].len);
		if (r) {
			if (r > 0 && !c->intent_req && !pick_rintent(c, c->pend[0].len)) {
				c->intent_req = c->pend[0].len;
				cmd(c->g, GLINK_CMD_RX_INTENT_REQ, c->lcid, c->pend[0].len);
			}
			return;
		}
		c->npend--;
		for (int i = 0; i < c->npend; i++) c->pend[i] = c->pend[i + 1];
	}
}

int glink_send(struct glink_chan *c, const void *data, u32 len)
{
	if (!c || len > GLINK_MSG_MAX) return -1;
	if (!c->npend && glink_ready(c) && send_data(c, data, len) == 0) {
		flush(c->g);
		return 0;
	}
	if (c->npend == GLINK_PEND) return -1;
	c->pend[c->npend].len = len;
	mcopy(c->pend[c->npend].data, data, len);
	c->npend++;
	if (glink_ready(c)) send_pending(c);
	flush(c->g);
	return 0;
}

bool glink_ready(const struct glink_chan *c)
{
	return c && c->open_acked && c->remote_open && !c->closed;
}

/* --- receiving ------------------------------------------------------- */

static void rx_open(struct glink *g, u16 rcid, u32 namelen)
{
	char name[33];
	u32 n = MIN(namelen, 32u);
	rx_peek(g, 8, name, n);
	name[n] = 0;
	struct glink_chan *c = by_name(g, name);
	rx_advance(g, 8 + namelen);
	if (!c) {
		mlog("glink: the remote opens %s (not used here)\n", name);
		return;
	}
	c->rcid = rcid;
	c->remote_open = true;
	c->closed = false;
	cmd(g, GLINK_CMD_OPEN_ACK, rcid, 0);
}

static void rx_intents(struct glink *g, u16 rcid, u32 count, u32 avail)
{
	u32 len = 8 + 8 * count;
	if (avail < len) return;
	struct glink_chan *c = by_rcid(g, rcid);
	for (u32 i = 0; c && i < count; i++) {
		u8 pair[8];
		rx_peek(g, 8 + 8 * i, pair, 8);
		u32 size = get32(pair), id = get32(pair + 4);
		struct glink_rintent *slot = 0;
		for (int j = 0; j < GLINK_RINTENTS; j++) {
			if (c->ri[j].valid && c->ri[j].id == id) { slot = &c->ri[j]; break; }
			if (!slot && !c->ri[j].valid) slot = &c->ri[j];
		}
		if (!slot) continue;
		slot->id = id;
		slot->size = size;
		slot->valid = true;
		slot->used = false;
	}
	rx_advance(g, len);
	if (c) {
		c->intent_req = 0;
		send_pending(c);
	}
}

static void rx_done(struct glink *g, u16 rcid, u32 iid, bool reuse)
{
	struct glink_chan *c = by_rcid(g, rcid);
	for (int i = 0; c && i < GLINK_RINTENTS; i++) {
		struct glink_rintent *r = &c->ri[i];
		if (!r->valid || r->id != iid) continue;
		r->used = false;
		if (!reuse) r->valid = false;
		break;
	}
	rx_advance(g, 8);
	if (c) send_pending(c);
}

static void rx_intent_req(struct glink *g, u16 rcid, u32 size)
{
	struct glink_chan *c = by_rcid(g, rcid);
	rx_advance(g, 8);
	if (!c) return;
	struct glink_lintent *li = size <= 64 * 1024 ? add_intent(c, size, false) : 0;
	cmd(g, GLINK_CMD_RX_INTENT_REQ_ACK, c->lcid, li != 0);
}

/* data into an intent Linux had advertised (an adopted channel): through
 * the bounce buffer, then the intent given back for reuse */
static void rx_linux_intent(struct glink *g, struct glink_chan *c, u32 liid, u32 chunk, u32 left)
{
	if (c->fliid != liid) {
		c->fliid = liid;
		c->foff = 0;
		c->fover = false;
	}
	if (c->foff + chunk <= sizeof(g->bounce)) rx_peek(g, 16, g->bounce + c->foff, chunk);
	else c->fover = true;
	c->foff += chunk;
	rx_advance(g, 16 + chunk);
	if (left) return;
	if (c->fover) {
		g->errors++;
		mlog("glink: %s: %u bytes, too long: dropped\n", c->name, c->foff);
	} else {
		c->rx_msgs++;
		if (c->rx) c->rx(c, g->bounce, c->foff, c->arg);
	}
	cmd(g, GLINK_CMD_RX_DONE_W_REUSE, c->lcid, liid);
	c->fliid = c->foff = 0;
	c->fover = false;
}

/* data, or its next chunk; -1: not all of it in the FIFO yet */
static int rx_data(struct glink *g, u16 rcid, u32 liid, u32 avail)
{
	if (avail < 16) return -1;
	u8 h[8];
	rx_peek(g, 8, h, 8);
	u32 chunk = get32(h), left = get32(h + 4);
	if (avail < 16 + chunk) return -1;
	struct glink_chan *c = by_rcid(g, rcid);
	struct glink_lintent *li = 0;
	for (int i = 0; c && i < GLINK_LINTENTS; i++)
		if (c->li[i].valid && c->li[i].id == liid) li = &c->li[i];
	if (!li && c && c->adopted) {
		rx_linux_intent(g, c, liid, chunk, left);
		return 0;
	}
	if (!li) {
		g->errors++;
		mlog("glink: data for channel %u intent %u: not known\n", rcid, liid);
	} else if (li->size - li->off < chunk) {
		g->errors++;
		mlog("glink: %u bytes do not fit intent %u\n", chunk, liid);
		li->off = 0;
	} else {
		rx_peek(g, 16, li->buf + li->off, chunk);
		li->off += chunk;
		li->busy = true;
	}
	rx_advance(g, 16 + chunk);
	if (li && !left) {
		u32 n = li->off;
		li->off = 0;
		c->rx_msgs++;
		if (c->rx) c->rx(c, li->buf, n, c->arg);
		li->busy = false;
		cmd(g, li->reuse ? GLINK_CMD_RX_DONE_W_REUSE : GLINK_CMD_RX_DONE, c->lcid, li->id);
		if (!li->reuse) li->id = 0;  /* free again (its buffer kept) */
	}
	return 0;
}

void glink_poll(struct glink *g)
{
	for (;;) {
		u32 avail = rx_avail(g);
		if (avail < 8) break;
		u8 h[8];
		rx_peek(g, 0, h, 8);
		u16 c = get16(h), p1 = get16(h + 2);
		u32 p2 = get32(h + 4);
		switch (c) {
		case GLINK_CMD_VERSION:
			rx_advance(g, 8);
			cmd(g, GLINK_CMD_VERSION_ACK, 1, g->features & p2);
			break;
		case GLINK_CMD_VERSION_ACK:
			rx_advance(g, 8);
			break;
		case GLINK_CMD_OPEN:
			if (avail < 8 + (p2 & 0xffff)) goto out;
			rx_open(g, p1, p2 & 0xffff);
			break;
		case GLINK_CMD_CLOSE: {
			struct glink_chan *ch = by_rcid(g, p1);
			rx_advance(g, 8);
			if (ch) {
				ch->remote_open = false;
				ch->closed = true;
				mlog("glink: the remote closed %s\n", ch->name);
			}
			cmd(g, GLINK_CMD_CLOSE_ACK, p1, 0);
			break;
		}
		case GLINK_CMD_OPEN_ACK: {
			struct glink_chan *ch = by_lcid(g, p1);
			rx_advance(g, 8);
			if (ch) {
				ch->open_acked = true;
				send_intents(ch);
			}
			break;
		}
		case GLINK_CMD_CLOSE_ACK:
		case GLINK_CMD_SIGNALS:
		case GLINK_CMD_RX_INTENT_REQ_ACK:
			rx_advance(g, 8);
			break;
		case GLINK_CMD_INTENT:
			if (avail < 8 + 8 * p2) goto out;
			rx_intents(g, p1, p2, avail);
			break;
		case GLINK_CMD_RX_DONE:
		case GLINK_CMD_RX_DONE_W_REUSE:
			rx_done(g, p1, p2, c == GLINK_CMD_RX_DONE_W_REUSE);
			break;
		case GLINK_CMD_RX_INTENT_REQ:
			rx_intent_req(g, p1, p2);
			break;
		case GLINK_CMD_TX_DATA:
		case GLINK_CMD_TX_DATA_CONT:
			if (rx_data(g, p1, p2, avail) < 0) goto out;
			break;
		case GLINK_CMD_READ_NOTIF:
			rx_advance(g, 8);
			g->kick_due = true;
			break;
		default:
			/* its length is not known: nothing after it can be read */
			mlog("glink: unknown command %u, the link stops\n", c);
			g->errors++;
			goto out;
		}
	}
out:
	for (int i = 0; i < GLINK_MAX_CHAN; i++) {
		struct glink_chan *ch = &g->ch[i];
		/* acknowledged, but the remote's own OPEN does not come: the one
		 * Linux left unanswered, after 3 s */
		if (ch->used && ch->open_acked && !ch->remote_open && !ch->closed && ch->hint_rcid >= 0 &&
		    ++ch->wait_polls == 300) {
			mlog("glink: %s: the remote's OPEN from before (%d) answered\n", ch->name, ch->hint_rcid);
			ch->rcid = ch->hint_rcid;
			ch->remote_open = true;
			cmd(g, GLINK_CMD_OPEN_ACK, ch->rcid, 0);
		}
		if (ch->used && glink_ready(ch)) send_pending(ch);
	}
	flush(g);
}

int glink_init(struct glink *g, struct smem *s, void (*kick)(void))
{
	mzero(g, sizeof(*g) - sizeof(g->arena));
	g->smem = s;
	g->kick = kick;
	g->features = GLINK_FEATURE_INTENT_REUSE;
	g->next_liid = 1;
	u32 n;
	g->desc = smem_get(s, GLINK_ITEM_DESC, &n);
	if (!g->desc || n != 32) return -1;
	g->tx_fifo = smem_get(s, GLINK_ITEM_FIFO0, &g->tx_len);
	if (!g->tx_fifo || g->tx_len < 1024) return -1;
	rx_fifo(g);
	return 0;
}

/* --- channels Linux left open ---------------------------------------- */

/* Each command starts 8-byte aligned. Going back from the head, a command
 * found back bytes behind it is whole only if it ends before the head. */
static void ring(struct glink *g, bool tx, volatile u8 **fifo, u32 *len, u32 *head)
{
	*fifo = tx ? g->tx_fifo : (rx_fifo(g) ? g->rx_fifo : 0);
	*len = tx ? g->tx_len : g->rx_len;
	*head = sm_rd32(g->desc + (tx ? 4 : 12));
}

s32 glink_scan_data(struct glink *g, bool tx, bool (*match)(const u8 *data, u32 len))
{
	volatile u8 *f;
	u32 len, head;
	ring(g, tx, &f, &len, &head);
	if (!f || head >= len || head % 8) return -1;
	u8 h[16 + 64];
	for (u32 back = 8; back + 16 <= len; back += 8) {
		u32 at = (head + len - back) % len;
		glink_fifo_peek(f, len, at, 0, h, 16);
		u32 chunk = get32(h + 8), left = get32(h + 12);
		if (get16(h) != GLINK_CMD_TX_DATA || left || !chunk || chunk > GLINK_MSG_MAX || 16 + chunk > back)
			continue;
		glink_fifo_peek(f, len, at, 16, h + 16, MIN(chunk, 64u));
		if (match(h + 16, chunk)) return get16(h + 2);
	}
	return -1;
}

s32 glink_scan_open(struct glink *g, bool tx, const char *name)
{
	volatile u8 *f;
	u32 len, head, nl = mstrlen(name) + 1;
	ring(g, tx, &f, &len, &head);
	if (!f || head >= len || nl > 32) return -1;
	u8 h[8 + 32];
	for (u32 back = 8; back <= len; back += 8) {
		u32 at = (head + len - back) % len;
		glink_fifo_peek(f, len, at, 0, h, 8 + nl);
		if (get16(h) == GLINK_CMD_OPEN && (get32(h + 4) & 0xffff) == nl && !h[8 + nl - 1] &&
		    !memcmp_m(h + 8, name, nl))
			return get16(h + 2);
	}
	return -1;
}

struct glink_chan *glink_adopt(struct glink *g, const char *name, u16 lcid, u16 rcid, glink_rx_fn rx, void *arg)
{
	struct glink_chan *c = 0;
	for (int i = 0; i < GLINK_MAX_CHAN && !c; i++)
		if (!g->ch[i].used) c = &g->ch[i];
	if (!c) return 0;
	mzero(c, sizeof(*c));
	c->g = g;
	c->used = c->adopted = true;
	c->open_sent = c->open_acked = c->remote_open = true;
	c->lcid = lcid;
	c->rcid = rcid;
	c->hint_rcid = -1;
	c->rx = rx;
	c->arg = arg;
	mstrcpy(c->name, name, sizeof(c->name));
	/* our own receive buffers too, numbered clear of Linux's (its ids
	 * count from 1 per channel) */
	if (g->next_liid < 0x10000) g->next_liid = 0x10000;
	send_intents(c);
	flush(g);
	return c;
}

/* The remote's last OPEN of name in what was read of the receive FIFO
 * already: the ring keeps the bytes behind its tail. Linux reads the
 * modem's OPEN when the modem reopens IPCRTR after Linux closed it (before
 * the kexec) and leaves it unanswered; KolibriOS then needs its id. Each
 * command starts 8-byte aligned; a match is the command, its name length
 * and the name. */
static s32 scan_open(struct glink *g, const char *name)
{
	if (!rx_fifo(g)) return -1;
	u32 nl = mstrlen(name) + 1, tail = sm_rd32(g->desc + 8);
	if (tail >= g->rx_len || nl > 32) return -1;
	for (u32 back = 8; back <= g->rx_len; back += 8) {
		u32 at = (tail + g->rx_len - back) % g->rx_len;
		u8 h[8 + 32];
		glink_fifo_peek(g->rx_fifo, g->rx_len, at, 0, h, 8 + nl);
		if (get16(h) == GLINK_CMD_OPEN && (get32(h + 4) & 0xffff) == nl && !h[8 + nl - 1] &&
		    !memcmp_m(h + 8, name, nl))
			return get16(h + 2);
	}
	return -1;
}

struct glink_chan *glink_open(struct glink *g, const char *name, u16 lcid, glink_rx_fn rx, void *arg)
{
	struct glink_chan *c = by_name(g, name);
	if (!c)
		for (int i = 0; i < GLINK_MAX_CHAN && !c; i++)
			if (!g->ch[i].used) c = &g->ch[i];
	if (!c) return 0;
	mzero(c, sizeof(*c) - sizeof(c->pend));
	c->npend = 0;
	c->g = g;
	c->used = true;
	c->lcid = lcid;
	c->rx = rx;
	c->arg = arg;
	mstrcpy(c->name, name, sizeof(c->name));
	u32 nl = mstrlen(c->name) + 1;
	u8 m[8 + 40];
	mzero(m, sizeof(m));
	put16(m, GLINK_CMD_OPEN);
	put16(m + 2, lcid);
	put32(m + 4, nl);
	mcopy(m + 8, c->name, nl);
	tx(g, m, 8 + nl, 0, 0);
	c->open_sent = true;
	c->hint_rcid = scan_open(g, c->name);
	if (c->hint_rcid >= 0)
		mlog("glink: the remote opened %s as %d before (in the FIFO's past)\n", c->name, c->hint_rcid);
	flush(g);
	return c;
}
