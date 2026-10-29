/* KolibriOS for ARM: GLINK over SMEM, the apps processor's side (Linux:
 * drivers/rpmsg/qcom_glink_smem.c, qcom_glink_native.c) */
#ifndef GLINK_H
#define GLINK_H
#include "smem.h"

#define GLINK_ITEM_DESC 478    /* the FIFOs' indices: tx tail, head, rx tail, head */
#define GLINK_ITEM_FIFO0 479   /* apps processor to the remote */
#define GLINK_ITEM_FIFO1 480   /* the remote to the apps processor */

#define GLINK_MAX_CHAN 2
#define GLINK_LINTENTS 12      /* our receive buffers, per channel */
#define GLINK_RINTENTS 32      /* the remote's, per channel */
#define GLINK_PEND 8           /* messages waiting for a remote intent */
#define GLINK_MSG_MAX 4096     /* the largest message sent */
#define GLINK_OUTQ 16          /* commands waiting for room in the FIFO */
#define GLINK_ARENA (192 * 1024) /* our receive buffers */

struct glink;
struct glink_chan;
typedef void (*glink_rx_fn)(struct glink_chan *c, const u8 *data, u32 len, void *arg);

struct glink_lintent { u32 id, size, off; bool valid, reuse, busy; u8 *buf; };
struct glink_rintent { u32 id, size; bool valid, used; };
struct glink_pend { u32 len; u8 data[GLINK_MSG_MAX]; };

struct glink_chan {
	struct glink *g;
	char name[32];
	u16 lcid, rcid;
	bool used, open_sent, open_acked, remote_open, intents_sent, closed;
	glink_rx_fn rx;
	void *arg;
	struct glink_lintent li[GLINK_LINTENTS];
	struct glink_rintent ri[GLINK_RINTENTS];
	struct glink_pend pend[GLINK_PEND];
	int npend;
	u32 intent_req;        /* the size asked of the remote, 0: none asked */
	s32 hint_rcid;         /* the remote's OPEN seen in the FIFO's past (Linux read it), -1: none */
	u32 wait_polls;        /* polls since our OPEN was acknowledged without the remote's */
	u32 rx_msgs, tx_msgs;
};

struct glink_out { u32 len; u8 d[16 + GLINK_MSG_MAX + 8]; };

struct glink {
	struct smem *smem;
	volatile u8 *desc;                 /* tx tail, tx head, rx tail, rx head */
	volatile u8 *tx_fifo, *rx_fifo;
	u32 tx_len, rx_len;
	void (*kick)(void);
	u32 features;
	struct glink_chan ch[GLINK_MAX_CHAN];
	struct glink_out out[GLINK_OUTQ];
	int out_first, out_n;
	u32 next_liid;
	u8 arena[GLINK_ARENA];
	u32 arena_used;
	bool kick_due;
	u32 errors;
};

/* the transport of the edge with remote (its SMEM host); 0, or -1 when
 * SMEM lacks its descriptor or FIFOs */
int glink_init(struct glink *g, struct smem *s, void (*kick)(void));
/* a channel opened by name; rx gets each whole message */
struct glink_chan *glink_open(struct glink *g, const char *name, u16 lcid, glink_rx_fn rx, void *arg);
bool glink_ready(const struct glink_chan *c);
/* a message on the channel: 0 sent or queued, -1 too long or no room */
int glink_send(struct glink_chan *c, const void *data, u32 len);
/* what arrived, then what waits to be sent */
void glink_poll(struct glink *g);

/* the FIFO arithmetic, also the fake remote's (fake.c) */
u32 glink_fifo_avail(u32 head, u32 tail, u32 len);
void glink_fifo_peek(const volatile u8 *fifo, u32 len, u32 tail, u32 off, void *dst, u32 n);
u32 glink_fifo_write(volatile u8 *fifo, u32 len, u32 head, const void *src, u32 n);

enum {
	GLINK_CMD_VERSION = 0, GLINK_CMD_VERSION_ACK = 1, GLINK_CMD_OPEN = 2, GLINK_CMD_CLOSE = 3,
	GLINK_CMD_OPEN_ACK = 4, GLINK_CMD_INTENT = 5, GLINK_CMD_RX_DONE = 6,
	GLINK_CMD_RX_INTENT_REQ = 7, GLINK_CMD_RX_INTENT_REQ_ACK = 8, GLINK_CMD_TX_DATA = 9,
	GLINK_CMD_CLOSE_ACK = 11, GLINK_CMD_TX_DATA_CONT = 12, GLINK_CMD_READ_NOTIF = 13,
	GLINK_CMD_RX_DONE_W_REUSE = 14, GLINK_CMD_SIGNALS = 15,
};
#define GLINK_FEATURE_INTENT_REUSE 1

#endif
