/* KolibriOS for ARM: QRTR, Qualcomm's IPC router (Linux: net/qrtr), on a
 * GLINK channel ("IPCRTR"), with the apps processor's name service */
#ifndef QRTR_H
#define QRTR_H
#include "glink.h"

#define QRTR_PORT_CTRL 0xfffffffeu
#define QRTR_NODE_BCAST 0xffffffffu
#define QRTR_NODE_APPS 1
#define QRTR_MAX_SERVERS 64
#define QRTR_MAX_PORTS 8

enum {
	QRTR_TYPE_DATA = 1, QRTR_TYPE_HELLO = 2, QRTR_TYPE_BYE = 3, QRTR_TYPE_NEW_SERVER = 4,
	QRTR_TYPE_DEL_SERVER = 5, QRTR_TYPE_DEL_CLIENT = 6, QRTR_TYPE_RESUME_TX = 7,
	QRTR_TYPE_EXIT = 8, QRTR_TYPE_PING = 9, QRTR_TYPE_NEW_LOOKUP = 10, QRTR_TYPE_DEL_LOOKUP = 11,
};

/* a service: instance is the QMI version | instance << 8 */
struct qrtr_server { u32 service, instance, node, port; };

enum { QRTR_EV_HELLO, QRTR_EV_BYE, QRTR_EV_NEW_SERVER, QRTR_EV_DEL_SERVER, QRTR_EV_DEL_CLIENT };

typedef void (*qrtr_rx_fn)(void *arg, u32 node, u32 port, const u8 *data, u32 len);
typedef void (*qrtr_event_fn)(void *arg, int ev, const struct qrtr_server *s, u32 node, u32 port);

struct qrtr {
	struct glink_chan *ch;
	u32 node, remote;
	bool hello;
	struct qrtr_server srv[QRTR_MAX_SERVERS];
	int nsrv;
	struct { u32 port; qrtr_rx_fn rx; void *arg; } ports[QRTR_MAX_PORTS];
	int nports;
	struct qrtr_server mine[4];   /* our servers (node, port ours) */
	int nmine;
	qrtr_event_fn event;
	void *earg;
	u32 rx_pkts, tx_pkts, errors;
};

void qrtr_init(struct qrtr *q, struct glink_chan *ch, u32 node, qrtr_event_fn ev, void *arg);
/* the GLINK channel's receive function: q is its argument */
void qrtr_glink_rx(struct glink_chan *c, const u8 *data, u32 len, void *arg);
void qrtr_bind(struct qrtr *q, u32 port, qrtr_rx_fn rx, void *arg);
/* a service of ours, announced to the remote (now, or after its HELLO) */
void qrtr_serve(struct qrtr *q, u32 service, u32 version, u32 instance, u32 port);
int qrtr_send(struct qrtr *q, u32 port, u32 node, u32 dport, const void *data, u32 len);
/* a control packet (HELLO, ...) from our control port */
int qrtr_send_ctrl(struct qrtr *q, u32 type, u32 node, const u8 *payload20);
const struct qrtr_server *qrtr_lookup(const struct qrtr *q, u32 service);

/* the header on the wire (also the fake remote's) */
#define QRTR_HDR_V1 32
u32 qrtr_pack(u8 *out, u32 type, u32 snode, u32 sport, u32 dnode, u32 dport,
	      bool confirm, const void *data, u32 len);
struct qrtr_pkt { u32 type, snode, sport, dnode, dport, size; bool confirm; const u8 *data; };
bool qrtr_unpack(const u8 *p, u32 len, struct qrtr_pkt *k);

#endif
