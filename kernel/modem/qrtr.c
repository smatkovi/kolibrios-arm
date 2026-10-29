/* KolibriOS for ARM: QRTR on a GLINK channel.
 *
 * Each GLINK message is one packet: a header (version 1: eight
 * little-endian words; version 2, which some firmware sends: a shorter
 * one), the payload padded to 4 bytes. Packets to port 0xfffffffe are
 * control packets: HELLO when a node appears (the remote says it first,
 * the answer is a HELLO and the list of our services), NEW_SERVER and
 * DEL_SERVER as services come and go, DEL_CLIENT when a client port
 * goes, BYE when the node goes. The apps processor is node 1; KolibriOS
 * is its name service as well. */
#include "qrtr.h"

#define QRTR_PROTO_VER_1 1
#define QRTR_PROTO_VER_2 3

u32 qrtr_pack(u8 *out, u32 type, u32 snode, u32 sport, u32 dnode, u32 dport,
	      bool confirm, const void *data, u32 len)
{
	put32(out, QRTR_PROTO_VER_1);
	put32(out + 4, type);
	put32(out + 8, snode);
	put32(out + 12, sport);
	put32(out + 16, confirm);
	put32(out + 20, len);
	put32(out + 24, dnode);
	put32(out + 28, dport);
	mcopy(out + QRTR_HDR_V1, data, len);
	u32 n = QRTR_HDR_V1 + M_ALIGN(len, 4);
	mzero(out + QRTR_HDR_V1 + len, n - QRTR_HDR_V1 - len);
	return n;
}

bool qrtr_unpack(const u8 *p, u32 len, struct qrtr_pkt *k)
{
	if (len < 16) return false;
	if (get32(p) == QRTR_PROTO_VER_1) {
		if (len < QRTR_HDR_V1) return false;
		k->type = get32(p + 4);
		k->snode = get32(p + 8);
		k->sport = get32(p + 12);
		k->confirm = get32(p + 16) != 0;
		k->size = get32(p + 20);
		k->dnode = get32(p + 24);
		k->dport = get32(p + 28);
		k->data = p + QRTR_HDR_V1;
		return k->size <= len - QRTR_HDR_V1;
	}
	if (p[0] == QRTR_PROTO_VER_2) {
		u32 hl = 16 + p[3];          /* the optional header's length */
		k->type = p[1];
		k->confirm = p[2] & 1;
		k->size = get32(p + 4);
		k->snode = get16(p + 8);
		k->sport = get16(p + 10);
		k->dnode = get16(p + 12);
		k->dport = get16(p + 14);
		if (k->snode == 0xffff) k->snode = QRTR_NODE_BCAST;
		if (k->sport == 0xffff) k->sport = 0xffffffffu;
		if (k->sport == 0xfffe) k->sport = QRTR_PORT_CTRL;
		if (k->dnode == 0xffff) k->dnode = QRTR_NODE_BCAST;
		if (k->dport == 0xfffe) k->dport = QRTR_PORT_CTRL;
		k->data = p + hl;
		return hl <= len && k->size <= len - hl;
	}
	return false;
}

static u8 pkt[QRTR_HDR_V1 + GLINK_MSG_MAX];

int qrtr_send(struct qrtr *q, u32 port, u32 node, u32 dport, const void *data, u32 len)
{
	if (len + QRTR_HDR_V1 > GLINK_MSG_MAX) return -1;
	u32 n = qrtr_pack(pkt, QRTR_TYPE_DATA, q->node, port, node, dport, false, data, len);
	q->tx_pkts++;
	return glink_send(q->ch, pkt, n);
}

int qrtr_send_ctrl(struct qrtr *q, u32 type, u32 node, const u8 *payload20)
{
	u8 b[20];
	if (payload20) mcopy(b, payload20, 20);
	else mzero(b, 20);
	put32(b, type);
	u32 n = qrtr_pack(pkt, type, q->node, QRTR_PORT_CTRL, node, QRTR_PORT_CTRL, false, b, 20);
	q->tx_pkts++;
	return glink_send(q->ch, pkt, n);
}

static void announce(struct qrtr *q, const struct qrtr_server *s, u32 to)
{
	u8 b[20];
	put32(b, QRTR_TYPE_NEW_SERVER);
	put32(b + 4, s->service);
	put32(b + 8, s->instance);
	put32(b + 12, s->node);
	put32(b + 16, s->port);
	qrtr_send_ctrl(q, QRTR_TYPE_NEW_SERVER, to, b);
}

void qrtr_serve(struct qrtr *q, u32 service, u32 version, u32 instance, u32 port)
{
	if (q->nmine == (int)(sizeof(q->mine) / sizeof(q->mine[0]))) return;
	struct qrtr_server *s = &q->mine[q->nmine++];
	s->service = service;
	s->instance = version | instance << 8;
	s->node = q->node;
	s->port = port;
	if (q->hello) announce(q, s, q->remote);
}

const struct qrtr_server *qrtr_lookup(const struct qrtr *q, u32 service)
{
	for (int i = 0; i < q->nsrv; i++)
		if (q->srv[i].service == service) return &q->srv[i];
	return 0;
}

static void server_add(struct qrtr *q, const struct qrtr_server *s)
{
	for (int i = 0; i < q->nsrv; i++)
		if (q->srv[i].node == s->node && q->srv[i].port == s->port) {
			q->srv[i] = *s;
			return;
		}
	if (q->nsrv < QRTR_MAX_SERVERS) q->srv[q->nsrv++] = *s;
}

static void server_del(struct qrtr *q, u32 node, u32 port)
{
	for (int i = 0; i < q->nsrv; i++)
		if (q->srv[i].node == node && (port == 0xffffffffu || q->srv[i].port == port)) {
			q->srv[i--] = q->srv[--q->nsrv];
		}
}

static void ctrl(struct qrtr *q, const struct qrtr_pkt *k)
{
	u8 b[20];
	mzero(b, 20);
	mcopy(b, k->data, MIN(k->size, 20u));
	u32 type = k->type;
	struct qrtr_server s = { get32(b + 4), get32(b + 8), get32(b + 12), get32(b + 16) };
	switch (type) {
	case QRTR_TYPE_HELLO:
		q->remote = k->snode;
		q->hello = true;
		qrtr_send_ctrl(q, QRTR_TYPE_HELLO, k->snode, 0);
		for (int i = 0; i < q->nmine; i++) announce(q, &q->mine[i], k->snode);
		if (q->event) q->event(q->earg, QRTR_EV_HELLO, 0, k->snode, 0);
		break;
	case QRTR_TYPE_BYE:
		server_del(q, k->snode, 0xffffffffu);
		q->hello = false;
		if (q->event) q->event(q->earg, QRTR_EV_BYE, 0, k->snode, 0);
		break;
	case QRTR_TYPE_NEW_SERVER:
		if (!s.service && !s.instance && !s.node && !s.port) break;  /* a lookup's end */
		server_add(q, &s);
		if (q->event) q->event(q->earg, QRTR_EV_NEW_SERVER, &s, s.node, s.port);
		break;
	case QRTR_TYPE_DEL_SERVER:
		server_del(q, s.node, s.port);
		if (q->event) q->event(q->earg, QRTR_EV_DEL_SERVER, &s, s.node, s.port);
		break;
	case QRTR_TYPE_DEL_CLIENT:
		/* client: node, port in the words after the command */
		if (q->event) q->event(q->earg, QRTR_EV_DEL_CLIENT, 0, get32(b + 4), get32(b + 8));
		break;
	case QRTR_TYPE_NEW_LOOKUP:
		/* our services that match, then an empty one: the list's end */
		for (int i = 0; i < q->nmine; i++)
			if ((!s.service || q->mine[i].service == s.service) &&
			    (!s.instance || q->mine[i].instance == s.instance)) {
				u8 r[20];
				put32(r, QRTR_TYPE_NEW_SERVER);
				put32(r + 4, q->mine[i].service);
				put32(r + 8, q->mine[i].instance);
				put32(r + 12, q->mine[i].node);
				put32(r + 16, q->mine[i].port);
				u32 n = qrtr_pack(pkt, QRTR_TYPE_NEW_SERVER, q->node, QRTR_PORT_CTRL,
						  k->snode, k->sport, false, r, 20);
				glink_send(q->ch, pkt, n);
			}
		mzero(b, 20);
		put32(b, QRTR_TYPE_NEW_SERVER);
		{
			u32 n = qrtr_pack(pkt, QRTR_TYPE_NEW_SERVER, q->node, QRTR_PORT_CTRL,
					  k->snode, k->sport, false, b, 20);
			glink_send(q->ch, pkt, n);
		}
		break;
	default:
		break;
	}
}

void qrtr_glink_rx(struct glink_chan *c, const u8 *data, u32 len, void *arg)
{
	struct qrtr *q = arg;
	struct qrtr_pkt k;
	if (!qrtr_unpack(data, len, &k)) {
		q->errors++;
		return;
	}
	q->rx_pkts++;
	if (k.type != QRTR_TYPE_DATA || k.dport == QRTR_PORT_CTRL) {
		ctrl(q, &k);
		return;
	}
	for (int i = 0; i < q->nports; i++)
		if (q->ports[i].port == k.dport) {
			q->ports[i].rx(q->ports[i].arg, k.snode, k.sport, k.data, k.size);
			break;
		}
	if (k.confirm) {
		u8 b[20];
		mzero(b, 20);
		put32(b, QRTR_TYPE_RESUME_TX);
		put32(b + 4, q->node);
		put32(b + 8, k.dport);
		u32 n = qrtr_pack(pkt, QRTR_TYPE_RESUME_TX, q->node, k.dport, k.snode, k.sport, false, b, 20);
		glink_send(q->ch, pkt, n);
	}
}

void qrtr_bind(struct qrtr *q, u32 port, qrtr_rx_fn rx, void *arg)
{
	if (q->nports == QRTR_MAX_PORTS) return;
	q->ports[q->nports].port = port;
	q->ports[q->nports].rx = rx;
	q->ports[q->nports].arg = arg;
	q->nports++;
}

void qrtr_init(struct qrtr *q, struct glink_chan *ch, u32 node, qrtr_event_fn ev, void *arg)
{
	mzero(q, sizeof(*q));
	q->ch = ch;
	q->node = node;
	q->event = ev;
	q->earg = arg;
}
