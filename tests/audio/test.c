/* The call's sound (kernel/modem/apr.c, callaudio.c) on the PC: the APR
 * packets against the sizes of Linux's packed structures, the voice
 * path's sequence against a fake ADSP (and its failures), Linux's APR
 * channel found in the GLINK rings' past and taken over (the fake ADSP
 * behind a GLINK edge in memory), the session per call, the speaker
 * amplifier and the ADSP FIFO's draining. make -C tests/audio */
#include <assert.h>
#include <stdlib.h>
#include "../../kernel/modem/apr.h"
#include "../../kernel/modem/callaudio.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* --- the packets --------------------------------------------------------- */

static void test_packets(void)
{
	u8 p[128];
	struct apr_hdr h;

	/* sizes: APR_HDR_SIZE + Linux's payload structures (__packed) */
	CHECK(afe_hw_vote(p, 3, "LPASS_HW_MACRO") == 20 + 12);
	CHECK(memcmp(p + 24, "LPASS_H", 8) == 0);           /* strscpy into char[8] */
	CHECK(afe_hw_devote(p, 3, 7) == 20 + 8);
	CHECK(afe_clock_set(p, Q6AFE_LPASS_CLK_ID_RX_CORE_MCLK, LPASS_MCLK_HZ) == 20 + 16 + 12 + 20);
	CHECK(get32(p + 20) == 32 && get32(p + 36) == AFE_MODULE_CLOCK_SET && get16(p + 44) == 20);
	CHECK(get32(p + 52) == 0x30e && get32(p + 56) == 19200000 && get16(p + 60) == 1 && get32(p + 64) == 1);
	CHECK(afe_clock_set(p, 0x30e, 0) == 68 && get32(p + 64) == 0);
	CHECK(apr_get_hdr(p, 68, &h) && h.token == AFE_CLK_TOKEN && h.opcode == AFE_SVC_CMD_SET_PARAM);
	CHECK(h.src_svc == APR_SVC_AFE && h.dest_svc == APR_SVC_AFE && h.src_domain == APR_DOMAIN_APPS &&
	      h.dest_domain == APR_DOMAIN_ADSP && h.hdr_field == 0x250);
	CHECK(afe_cdc_dma_config(p, AFE_PORT_ID_RX_CODEC_DMA_RX_0, 113, 48000, 16, 2) == 20 + 16 + 12 + 36);
	CHECK(get16(p + 20) == 0xB030 && get16(p + 22) == 48 && get32(p + 40) == AFE_PARAM_ID_CODEC_DMA_CONFIG);
	CHECK(get32(p + 52) == 48000 && get16(p + 56) == 16 && get16(p + 60) == 2 && get16(p + 62) == 3);
	CHECK(afe_port_start(p, 0xB021, 110) == 24 && get16(p + 20) == 0xB021 && get32(p + 16) == AFE_PORT_CMD_DEVICE_START);
	CHECK(mvm_create_passive(p, 6, "11C05000") == 40 && memcmp(p + 20, "11C05000", 9) == 0);
	CHECK(apr_get_hdr(p, 40, &h) && h.src_port == 6 && h.dest_port == 0 && h.dest_svc == APR_SVC_MVM);
	CHECK(mvm_dual_control(p, 6, 3) == 21 && p[20] == 1);
	CHECK(mvm_attach(p, 6, 3, 9, true) == 22 && get16(p + 20) == 9 && get32(p + 16) == VSS_IMVM_CMD_ATTACH_VOCPROC);
	CHECK(mvm_start(p, 6, 3, false) == 20 && get32(p + 16) == VSS_IMVM_CMD_STOP_VOICE);
	CHECK(cvp_create(p, 6, false, 0xB021, 0xB030) == 64);
	CHECK(get16(p + 20) == 2 && get16(p + 22) == 0xB021 && get32(p + 24) == VSS_IVOCPROC_TOPOLOGY_ID_NONE);
	CHECK(get16(p + 28) == 0xB030 && get32(p + 30) == VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT);
	CHECK(get32(p + 34) == VSS_ICOMMON_CAL_NETWORK_ID_NONE && get32(p + 38) == VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING);
	CHECK(get16(p + 42) == 0xFFFF);
	/* offsetof(set_param, param_data.channel_info) + sizeof(channel_info) */
	CHECK(cvp_channel_info(p, 6, 9, true) == 64 && get32(p + 32) == 28 && get32(p + 48) == 1 && p[56] == 1);
	CHECK(cvp_media_format(p, 6, 9, 0xB030, false) == 68 && get32(p + 32) == 32 && get32(p + 48) == 0xB030);
	CHECK(get16(p + 52) == 2 && get32(p + 56) == 48000 && p[60] == 1 && p[61] == 2);
	CHECK(voice_destroy(p, APR_SVC_CVP, 6, 9) == 20 && get16(p + 10) == 9 && get32(p + 16) == APRV2_IBASIC_CMD_DESTROY_SESSION);
	CHECK(!apr_get_hdr(p, 10, &h));
}

/* --- a fake ADSP ------------------------------------------------------------ */

struct fake_adsp {
	u8 out[64][128];               /* our commands as it got them */
	u32 out_len[64];
	int n;
	u32 refuse_op;                 /* this command fails (status 0x0d) */
	bool silent;                   /* it does not answer (answer() by hand) */
	u16 next_handle;
	int mvm_sessions, cvp_sessions, afe_ports, clocks, votes;
	void (*deliver)(const u8 *pkt, u32 len);   /* its replies: to vp, or over GLINK */
};
static struct fake_adsp adsp;
static struct voicepath vp;
static u32 now;

static void to_vp(const u8 *pkt, u32 len) { voicepath_rx(&vp, pkt, len, now); }

static void reply(const struct apr_hdr *c, u32 opcode, const u8 *pl, u32 pn, u16 src_port)
{
	u8 r[64];
	put16(r, APR_MSG_TYPE_CMD_RSP << 8 | 5 << 4);
	put16(r + 2, APR_HDR_SIZE + pn);
	r[4] = c->dest_svc; r[5] = APR_DOMAIN_ADSP; put16(r + 6, src_port);
	r[8] = c->src_svc; r[9] = APR_DOMAIN_APPS; put16(r + 10, c->src_port);
	put32(r + 12, c->token); put32(r + 16, opcode);
	memcpy(r + 20, pl, pn);
	adsp.deliver(r, APR_HDR_SIZE + pn);
}

/* the ADSP's answer to our command i */
static void answer(int i)
{
	const u8 *pkt = adsp.out[i];
	struct apr_hdr h;
	apr_get_hdr(pkt, adsp.out_len[i], &h);
	u8 pl[8];
	u16 port = h.dest_port;
	put32(pl, h.opcode);
	put32(pl + 4, h.opcode == adsp.refuse_op ? 0x0d : 0);
	switch (h.opcode) {
	case AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST:
		adsp.votes++;
		put32(pl, 0x40 + adsp.votes);      /* the client handle */
		reply(&h, AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST, pl, 4, 0);
		return;
	case AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST: adsp.votes--; break;
	case AFE_SVC_CMD_SET_PARAM: adsp.clocks += get32(pkt + 64) ? 1 : -1; break;
	case AFE_PORT_CMD_DEVICE_START: if (h.opcode != adsp.refuse_op) adsp.afe_ports++; break;
	case AFE_PORT_CMD_DEVICE_STOP: adsp.afe_ports--; break;
	case VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION:
		if (h.opcode == adsp.refuse_op) break;
		port = ++adsp.next_handle; adsp.mvm_sessions++; break;
	case VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2:
	case VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V3:
		port = ++adsp.next_handle; adsp.cvp_sessions++; break;
	case APRV2_IBASIC_CMD_DESTROY_SESSION:
		if (h.dest_svc == APR_SVC_MVM) adsp.mvm_sessions--; else adsp.cvp_sessions--;
		break;
	}
	reply(&h, APR_BASIC_RSP_RESULT, pl, 8, port);
}

static void adsp_got(const u8 *pkt, u32 len)
{
	struct apr_hdr h;
	assert(apr_get_hdr(pkt, len, &h) && h.pkt_size == len && adsp.n < 64);
	memcpy(adsp.out[adsp.n], pkt, len);
	adsp.out_len[adsp.n++] = len;
	if (!adsp.silent) answer(adsp.n - 1);
}

static int adsp_send(void *arg, const u8 *pkt, u32 len)
{
	adsp_got(pkt, len);
	return 0;
}

static u32 op_of(int i) { return get32(adsp.out[i] + 16); }

static void reset(bool v3, bool ports)
{
	memset(&adsp, 0, sizeof(adsp));
	adsp.deliver = to_vp;
	voicepath_init(&vp, adsp_send, NULL, v3, ports);
}

static void test_voicepath(void)
{
	/* up: Linux's order, every command answered at once */
	reset(false, true);
	CHECK(voicepath_up(&vp, now) == 0);
	CHECK(vp.state == VP_RUNNING);
	CHECK(adsp.n == 16);                       /* 2 votes, 4 clocks, MVM 2, CVP 2, attach, start, 2 x (config, start) */
	CHECK(op_of(0) == AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST && op_of(5) == AFE_SVC_CMD_SET_PARAM);
	CHECK(op_of(6) == VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION && op_of(7) == VSS_IMVM_CMD_SET_POLICY_DUAL_CONTROL);
	CHECK(op_of(8) == VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2 && op_of(9) == VSS_IVOCPROC_CMD_ENABLE);
	CHECK(op_of(10) == VSS_IMVM_CMD_ATTACH_VOCPROC && op_of(11) == VSS_IMVM_CMD_START_VOICE);
	CHECK(op_of(12) == AFE_PORT_CMD_SET_PARAM_V2 && op_of(13) == AFE_PORT_CMD_DEVICE_START);
	CHECK(get16(adsp.out[13] + 20) == 0xB030 && get16(adsp.out[14] + 20) == 0xB021);
	CHECK(vp.mvm == 1 && vp.cvp == 2 && vp.vote_macro == 0x41 && vp.vote_dcodec == 0x42);
	CHECK(get16(adsp.out[7] + 10) == 1 && get16(adsp.out[10] + 20) == 2);   /* handles used */
	CHECK(adsp.mvm_sessions == 1 && adsp.cvp_sessions == 1 && adsp.afe_ports == 2 && adsp.clocks == 4 && adsp.votes == 2);
	CHECK(voicepath_up(&vp, now) == -1);       /* already up */

	/* down: everything released */
	adsp.n = 0;
	CHECK(voicepath_down(&vp, now) == 0 && vp.state == VP_IDLE);
	CHECK(adsp.n == 13 && op_of(0) == AFE_PORT_CMD_DEVICE_STOP && op_of(2) == VSS_IMVM_CMD_STOP_VOICE);
	CHECK(op_of(12) == AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST && get32(adsp.out[12] + 24) == 0x41);
	CHECK(adsp.mvm_sessions == 0 && adsp.cvp_sessions == 0 && adsp.afe_ports == 0 && adsp.clocks == 0 && adsp.votes == 0);
	CHECK(vp.mvm == 0 && vp.cvp == 0);

	/* the ports running already (Linux's streams): MVM and CVP only, as q6voice */
	reset(false, false);
	CHECK(voicepath_up(&vp, now) == 0 && vp.state == VP_RUNNING && adsp.n == 6);
	CHECK(op_of(0) == VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION && op_of(5) == VSS_IMVM_CMD_START_VOICE);
	adsp.n = 0;
	CHECK(voicepath_down(&vp, now) == 0 && vp.state == VP_IDLE && adsp.n == 5);
	CHECK(op_of(0) == VSS_IMVM_CMD_STOP_VOICE && op_of(4) == APRV2_IBASIC_CMD_DESTROY_SESSION);
	CHECK(adsp.mvm_sessions == 0 && adsp.cvp_sessions == 0 && adsp.afe_ports == 0 && adsp.votes == 0);

	/* CVD 2.3: create v3 and its parameters */
	reset(true, true);
	CHECK(voicepath_up(&vp, now) == 0 && vp.state == VP_RUNNING && adsp.n == 21);
	CHECK(op_of(8) == VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V3 && op_of(13) == VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT);

	/* a refused command: what was set up is undone, then VP_FAILED */
	reset(false, true);
	adsp.refuse_op = VSS_IMVM_CMD_START_VOICE;
	CHECK(voicepath_up(&vp, now) == 0);
	CHECK(vp.state == VP_FAILED && vp.status == 0x0d);
	CHECK(adsp.mvm_sessions == 0 && adsp.cvp_sessions == 0 && adsp.clocks == 0 && adsp.votes == 0);
	CHECK(voicepath_up(&vp, now) == 0 || vp.state == VP_FAILED);   /* may be tried again */

	/* no answer: 300 ms, then undone (no answer to that either: given up step by step) */
	reset(false, true);
	adsp.silent = true;
	now = 1000;
	CHECK(voicepath_up(&vp, now) == 0 && vp.state == VP_STARTING && adsp.n == 1);
	now += 299; voicepath_poll(&vp, now);
	CHECK(vp.state == VP_STARTING);
	now += 1; voicepath_poll(&vp, now);
	CHECK(vp.state == VP_STOPPING);
	for (int i = 0; i < 40 && vp.state == VP_STOPPING; i++) { now += 300; voicepath_poll(&vp, now); }
	CHECK(vp.state == VP_FAILED);

	/* stopped while starting: the command under way answered first (its
	 * handle), then down, and idle, not failed */
	reset(false, false);
	adsp.silent = true;
	CHECK(voicepath_up(&vp, now) == 0 && adsp.n == 1);
	answer(0);                                 /* MVM created */
	CHECK(adsp.n == 2 && op_of(1) == VSS_IMVM_CMD_SET_POLICY_DUAL_CONTROL);
	CHECK(voicepath_down(&vp, now) == 0 && vp.state == VP_STARTING && adsp.n == 2);
	answer(1);
	CHECK(vp.state == VP_STOPPING && adsp.n == 3 && op_of(2) == VSS_IMVM_CMD_STOP_VOICE);
	for (int i = 3; i < 10 && vp.state == VP_STOPPING; i++) answer(adsp.n - 1);
	CHECK(vp.state == VP_IDLE && adsp.mvm_sessions == 0 && adsp.cvp_sessions == 0);
	CHECK(op_of(adsp.n - 1) == APRV2_IBASIC_CMD_DESTROY_SESSION && adsp.n == 5);   /* stop, detach, destroy */

	/* replies to others are not ours */
	reset(false, true);
	adsp.silent = true;
	voicepath_up(&vp, now);
	u8 r[28] = { 0 };
	put16(r, 0x150); put16(r + 2, 28); r[4] = APR_SVC_MVM; r[5] = APR_DOMAIN_ADSP;
	put32(r + 16, APR_BASIC_RSP_RESULT); put32(r + 20, AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST);
	voicepath_rx(&vp, r, 28, now);
	CHECK(vp.state == VP_STARTING && vp.step == 0);
}

/* --- the ADSP's GLINK edge, as Linux left it -------------------------------- */

#define RING 4096
static struct glink edge;
static u32 desc[8];                        /* tx tail, tx head, rx tail, rx head */
static u8 txf[RING], rxf[RING];
static int kicks;
static void t_kick(void) { kicks++; }

/* the remote (the ADSP) as GLINK sees it: Linux's ids, and what it got */
#define LINUX_LCID 2                       /* apr_audio_svc; fastrpc is 1 */
#define ADSP_LCID 5                        /* the ADSP's for it; fastrpc 4 */
#define LINUX_LIID 3                       /* one of the intents Linux advertised */
static struct {
	u32 our_intents, intent_reqs, rx_done_reuse, rx_done_liid, data, bad_lcid;
	u32 next_iid;
} rem;

static void put_ring(u8 *f, u32 *head, const void *d, u32 n)
{
	u8 pad[8] = { 0 };
	*head = glink_fifo_write(f, RING, *head, d, n);
	if (n % 8) *head = glink_fifo_write(f, RING, *head, pad, 8 - n % 8);
}

static void data_cmd(u8 *f, u32 *head, u16 cid, u32 iid, const u8 *d, u32 n)
{
	u8 h[16];
	put16(h, GLINK_CMD_TX_DATA); put16(h + 2, cid); put32(h + 4, iid);
	put32(h + 8, n); put32(h + 12, 0);
	*head = glink_fifo_write(f, RING, *head, h, 16);
	put_ring(f, head, d, n);
}

static void open_cmd(u8 *f, u32 *head, u16 cid, const char *name)
{
	u8 m[40] = { 0 };
	u32 nl = strlen(name) + 1;
	put16(m, GLINK_CMD_OPEN); put16(m + 2, cid); put32(m + 4, nl);
	memcpy(m + 8, name, nl);
	put_ring(f, head, m, 8 + nl);
}

static void cmd8(u8 *f, u32 *head, u16 c, u16 p1, u32 p2)
{
	u8 m[8];
	put16(m, c); put16(m + 2, p1); put32(m + 4, p2);
	put_ring(f, head, m, 8);
}

/* the ADSP's replies, into Linux's intent */
static void over_glink(const u8 *pkt, u32 len) { data_cmd(rxf, &desc[3], ADSP_LCID, LINUX_LIID, pkt, len); }

/* the remote reads what we wrote */
static void remote_poll(void)
{
	while (desc[0] != desc[1]) {
		u8 h[16];
		glink_fifo_peek(txf, RING, desc[0], 0, h, 16);
		u16 c = get16(h), p1 = get16(h + 2);
		u32 p2 = get32(h + 4), n = 8;
		switch (c) {
		case GLINK_CMD_INTENT:
			rem.our_intents += p2;
			CHECK(p1 == LINUX_LCID && get32(h + 12) >= 0x10000);    /* clear of Linux's ids */
			n = 8 + 8 * p2;
			break;
		case GLINK_CMD_RX_INTENT_REQ: {
			rem.intent_reqs++;
			CHECK(p1 == LINUX_LCID);
			cmd8(rxf, &desc[3], GLINK_CMD_RX_INTENT_REQ_ACK, ADSP_LCID, 1);
			u8 m[16];
			put16(m, GLINK_CMD_INTENT); put16(m + 2, ADSP_LCID); put32(m + 4, 1);
			put32(m + 8, 512); put32(m + 12, 100 + rem.next_iid++);
			put_ring(rxf, &desc[3], m, 16);
			break;
		}
		case GLINK_CMD_TX_DATA: {
			u32 len = get32(h + 8);
			u8 d[256];
			glink_fifo_peek(txf, RING, desc[0], 16, d, len);
			rem.data++;
			if (p1 != LINUX_LCID) rem.bad_lcid++;
			/* the intent used: back at once, for reuse */
			cmd8(rxf, &desc[3], GLINK_CMD_RX_DONE_W_REUSE, ADSP_LCID, p2);
			adsp_got(d, len);
			n = M_ALIGN(16 + len, 8);
			break;
		}
		case GLINK_CMD_RX_DONE_W_REUSE:
			rem.rx_done_reuse++;
			rem.rx_done_liid = p2;
			break;
		}
		desc[0] = (desc[0] + n) % RING;
	}
}

/* the edge as Linux leaves it at the kexec: in the tx ring's past an OPEN
 * of boot, fastrpc's and APR's data; in the rx ring's past the ADSP's, and
 * one APR event still unread */
enum { RINGS_EMPTY, RINGS_OPENS, RINGS_FULL };
static void edge_linux(int what)
{
	bool apr = what == RINGS_FULL;
	memset(&edge, 0, sizeof(edge));
	memset(desc, 0, sizeof(desc));
	memset(txf, 0x5a, sizeof(txf));
	memset(rxf, 0xa5, sizeof(rxf));
	memset(&rem, 0, sizeof(rem));
	edge.desc = (volatile u8 *)desc;
	edge.tx_fifo = txf; edge.tx_len = RING;
	edge.rx_fifo = rxf; edge.rx_len = RING;
	edge.kick = t_kick;
	edge.features = GLINK_FEATURE_INTENT_REUSE;
	edge.next_liid = 1;
	u8 pkt[32] = { 0 }, rpc[40];
	memset(rpc, 0x11, sizeof(rpc));
	u32 tx = RING - 512, rx = RING - 256;     /* the rings have wrapped */
	if (what != RINGS_EMPTY) open_cmd(txf, &tx, 9, "apr_audio_svc");   /* of boot, the ids since changed */
	if (apr) {
		afe_port_start(pkt, 0xB030, 113);
		data_cmd(txf, &tx, LINUX_LCID, 7, pkt, 24);
	}
	data_cmd(txf, &tx, 1, 2, rpc, sizeof(rpc));
	desc[0] = desc[1] = tx;                   /* all read by the ADSP */
	if (what != RINGS_EMPTY) open_cmd(rxf, &rx, 8, "apr_audio_svc");
	if (apr) {
		u8 ev[28] = { 0 };
		put16(ev, 0x050); put16(ev + 2, 28); ev[4] = 7; ev[5] = APR_DOMAIN_ADSP; ev[8] = 7; ev[9] = APR_DOMAIN_APPS;
		data_cmd(rxf, &rx, ADSP_LCID, 1, ev, 28);
		data_cmd(rxf, &rx, 4, 1, rpc, sizeof(rpc));   /* fastrpc's, newer */
		desc[2] = rx;
		data_cmd(rxf, &rx, ADSP_LCID, 2, ev, 28);    /* unread: Linux's intent 2 */
	} else {
		desc[2] = rx;
		data_cmd(rxf, &rx, 4, 1, rpc, sizeof(rpc));   /* fastrpc's, unread */
	}
	desc[3] = rx;
	memset(&adsp, 0, sizeof(adsp));
	adsp.deliver = over_glink;
}

/* the kernel's tick: the modem's call, then the remote */
static void tick(int call, u32 id)
{
	now += 10;
	callaudio_poll(now, call, id);
	remote_poll();
}

static void run(int call, u32 id, int ticks)
{
	for (int i = 0; i < ticks; i++) tick(call, id);
}

static void t_gpio(int pin, bool high);
static bool t_i2c_write(u8 addr, u8 reg, u8 val);
static bool t_i2c_read(u8 addr, u8 reg, u8 *val);
static void t_delay(u32 us) {}

static void test_adopt(void)
{
	struct callaudio_hw h = {
		.i2c_write = t_i2c_write, .i2c_read = t_i2c_read, .gpio_set = t_gpio, .delay_us = t_delay,
		.adsp = &edge, .prepared = true,
	};
	/* the ids from the data, newest first; not the OPENs of boot */
	edge_linux(RINGS_FULL);
	CHECK(glink_scan_data(&edge, true, callaudio_apr_from_apps) == LINUX_LCID);
	CHECK(glink_scan_data(&edge, false, callaudio_apr_from_adsp) == ADSP_LCID);
	CHECK(glink_scan_data(&edge, true, callaudio_apr_from_adsp) == -1);    /* the directions */
	CHECK(glink_scan_open(&edge, true, "apr_audio_svc") == 9 && glink_scan_open(&edge, false, "apr_audio_svc") == 8);
	CHECK(glink_scan_open(&edge, false, "apr_audio") == -1);

	callaudio_init(&h);
	CHECK(callaudio_state() == CALLAUDIO_PREPARED && callaudio_voice() == VP_IDLE);
	remote_poll();
	/* Linux's unread event: received and its intent given back */
	CHECK(rem.rx_done_reuse == 1 && rem.rx_done_liid == 2 && desc[2] == desc[3]);
	CHECK(rem.our_intents == 8 && edge.errors == 0);

	/* no call: nothing sent */
	run(CALLAUDIO_CALL_NONE, 0, 5);
	CHECK(adsp.n == 0 && rem.data == 0);

	/* a call: the session started on Linux's channel (an intent asked for,
	 * the replies into Linux's intent) */
	run(CALLAUDIO_CALL_SETUP, 1, 20);
	CHECK(callaudio_voice() == VP_RUNNING && adsp.n == 6 && rem.bad_lcid == 0);
	CHECK(op_of(0) == VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION && op_of(5) == VSS_IMVM_CMD_START_VOICE);
	CHECK(rem.intent_reqs == 1 && rem.rx_done_liid == LINUX_LIID && rem.rx_done_reuse == 7);
	CHECK(adsp.mvm_sessions == 1 && adsp.cvp_sessions == 1 && adsp.afe_ports == 0);
	run(CALLAUDIO_CALL_ACTIVE, 1, 5);          /* answered: nothing more */
	CHECK(adsp.n == 6);

	/* over: stopped */
	run(CALLAUDIO_CALL_NONE, 0, 20);
	CHECK(callaudio_voice() == VP_IDLE && adsp.n == 11 && adsp.mvm_sessions == 0 && adsp.cvp_sessions == 0);

	/* another call while one is up (a call waiting taking over): anew */
	run(CALLAUDIO_CALL_SETUP, 2, 20);
	CHECK(callaudio_voice() == VP_RUNNING && adsp.n == 17);
	run(CALLAUDIO_CALL_ACTIVE, 3, 30);
	CHECK(callaudio_voice() == VP_RUNNING && adsp.n == 28 && adsp.mvm_sessions == 1);

	/* refused while ringing out: once more when answered, then not again */
	run(CALLAUDIO_CALL_NONE, 0, 20);
	adsp.refuse_op = VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION;
	int n0 = adsp.n;
	run(CALLAUDIO_CALL_SETUP, 4, 20);
	CHECK(callaudio_voice() == VP_FAILED && adsp.n == n0 + 1);
	run(CALLAUDIO_CALL_ACTIVE, 4, 20);
	CHECK(callaudio_voice() == VP_FAILED && adsp.n == n0 + 2);
	run(CALLAUDIO_CALL_ACTIVE, 4, 20);
	CHECK(adsp.n == n0 + 2);
	adsp.refuse_op = 0;
	run(CALLAUDIO_CALL_NONE, 0, 5);
	run(CALLAUDIO_CALL_ACTIVE, 5, 20);         /* the next call: fine */
	CHECK(callaudio_voice() == VP_RUNNING && adsp.mvm_sessions == 1);
	run(CALLAUDIO_CALL_NONE, 0, 20);
	CHECK(adsp.mvm_sessions == 0 && adsp.cvp_sessions == 0 && edge.errors == 0);

	/* no APR data, only the OPENs of boot: their ids */
	edge_linux(RINGS_OPENS);
	callaudio_init(&h);
	CHECK(callaudio_state() == CALLAUDIO_PREPARED && edge.ch[0].lcid == 9 && edge.ch[0].rcid == 8);

	/* nothing of APR in the rings: no sound, the FIFO drained instead */
	edge_linux(RINGS_EMPTY);
	callaudio_init(&h);
	CHECK(callaudio_state() == CALLAUDIO_NONE && callaudio_set_route(CALLAUDIO_SPEAKER) == -1);
	CHECK(desc[2] != desc[3]);
	int k = kicks;
	callaudio_poll(now += 30, CALLAUDIO_CALL_SETUP, 1);
	CHECK(desc[2] == desc[3] && callaudio_drained() == 56 && kicks == k + 1);
	callaudio_poll(now += 30, CALLAUDIO_CALL_SETUP, 1);
	CHECK(kicks == k + 1);                     /* nothing new: no kick */
}

/* --- the speaker ------------------------------------------------------------- */

static u8 amp_regs[2][0x70];
static bool amp_pin[2], amp_present = true;
static int i2c_writes;

static int amp_of(u8 addr) { return addr == AW87339_SPK0_ADDR ? 0 : addr == AW87339_SPK1_ADDR ? 1 : -1; }
static bool t_i2c_write(u8 addr, u8 reg, u8 val)
{
	int a = amp_of(addr);
	if (a < 0 || !amp_pin[a] || !amp_present) return false;
	amp_regs[a][reg] = val;
	i2c_writes++;
	return true;
}
static bool t_i2c_read(u8 addr, u8 reg, u8 *val)
{
	int a = amp_of(addr);
	if (a < 0 || !amp_pin[a] || !amp_present) return false;
	*val = reg == 0 ? 0x39 : amp_regs[a][reg];
	return true;
}
static void t_gpio(int pin, bool high)
{
	if (pin == AW87339_SPK0_GPIO) amp_pin[0] = high;
	if (pin == AW87339_SPK1_GPIO) amp_pin[1] = high;
}

static void test_speaker(void)
{
	/* no ADSP (QEMU): the speaker switch only */
	struct callaudio_hw h = {
		.i2c_write = t_i2c_write, .i2c_read = t_i2c_read, .gpio_set = t_gpio, .delay_us = t_delay,
		.prepared = false,
	};
	callaudio_init(&h);
	CHECK(callaudio_state() == CALLAUDIO_NONE && callaudio_set_route(CALLAUDIO_SPEAKER) == -1);
	h.prepared = true;
	callaudio_init(&h);
	CHECK(callaudio_state() == CALLAUDIO_PREPARED);

	/* only while a call is up */
	CHECK(callaudio_set_route(CALLAUDIO_SPEAKER) == 0);
	callaudio_poll(200, CALLAUDIO_CALL_NONE, 0);
	CHECK(!amp_pin[0]);
	callaudio_poll(210, CALLAUDIO_CALL_ACTIVE, 1);
	CHECK(amp_pin[0] && amp_regs[0][1] == 0x0e && amp_regs[0][5] == 0x0c && amp_regs[0][0x0a] == 0x96 && i2c_writes == 14);
	CHECK(!amp_pin[1]);
	callaudio_poll(220, CALLAUDIO_CALL_ACTIVE, 1);
	CHECK(i2c_writes == 14);                   /* not again */
	CHECK(callaudio_set_route(CALLAUDIO_EARPIECE) == 0);
	callaudio_poll(230, CALLAUDIO_CALL_ACTIVE, 1);
	CHECK(!amp_pin[0] && amp_regs[0][1] == 0);
	callaudio_set_route(CALLAUDIO_SPEAKER);
	callaudio_poll(240, CALLAUDIO_CALL_SETUP, 1);
	CHECK(amp_pin[0]);
	callaudio_poll(250, CALLAUDIO_CALL_NONE, 0);   /* the call is over: off, the earpiece next time */
	CHECK(!amp_pin[0] && callaudio_route() == CALLAUDIO_EARPIECE);
	callaudio_poll(260, CALLAUDIO_CALL_ACTIVE, 2);
	CHECK(!amp_pin[0]);

	/* no amplifier: back to the earpiece, not tried every tick */
	amp_present = false;
	i2c_writes = 0;
	callaudio_set_route(CALLAUDIO_SPEAKER);
	callaudio_poll(270, CALLAUDIO_CALL_ACTIVE, 2);
	CHECK(!amp_pin[0] && callaudio_route() == CALLAUDIO_EARPIECE && i2c_writes == 0);
	amp_present = true;
}

int main(void)
{
	test_packets();
	test_voicepath();
	test_adopt();
	test_speaker();
	printf("%s: %d failed\n", fails ? "FAIL" : "ok", fails);
	return fails != 0;
}
