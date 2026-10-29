/* KolibriOS for ARM: APR and the voice path of a call (apr.h) */
#include "apr.h"

#define TIMEOUT_MS 300

/* the DAI ids Linux's q6afe takes as the ports' tokens */
#define TOKEN_RX_CODEC_DMA_RX_0 113
#define TOKEN_VA_CODEC_DMA_TX_0 110

void apr_put_hdr(u8 *p, u32 pkt_size, u8 svc, u16 src_port, u16 dest_port, u32 token, u32 opcode)
{
	put16(p, APR_MSG_TYPE_SEQ_CMD << 8 | (APR_HDR_SIZE / 4) << 4);
	put16(p + 2, pkt_size);
	p[4] = svc;
	p[5] = APR_DOMAIN_APPS;
	put16(p + 6, src_port);
	p[8] = svc;
	p[9] = APR_DOMAIN_ADSP;
	put16(p + 10, dest_port);
	put32(p + 12, token);
	put32(p + 16, opcode);
}

bool apr_get_hdr(const u8 *p, u32 len, struct apr_hdr *h)
{
	if (len < APR_HDR_SIZE) return false;
	h->hdr_field = get16(p);
	h->pkt_size = get16(p + 2);
	h->src_svc = p[4];
	h->src_domain = p[5];
	h->src_port = get16(p + 6);
	h->dest_svc = p[8];
	h->dest_domain = p[9];
	h->dest_port = get16(p + 10);
	h->token = get32(p + 12);
	h->opcode = get32(p + 16);
	u32 hl = (h->hdr_field >> 4 & 0xf) * 4;
	return hl >= APR_HDR_SIZE && hl <= len && h->pkt_size <= len && (h->hdr_field & 0xf) <= 1;
}

/* --- the audio front end -------------------------------------------- */

u32 afe_hw_vote(u8 *p, u32 block, const char *client)
{
	u32 n = APR_HDR_SIZE + 4 + 8;
	mzero(p, n);
	apr_put_hdr(p, n, APR_SVC_AFE, 0, 0, block, AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST);
	put32(p + 20, block);
	mstrcpy((char *)p + 24, client, 8);
	return n;
}

u32 afe_hw_devote(u8 *p, u32 block, u32 handle)
{
	u32 n = APR_HDR_SIZE + 8;
	apr_put_hdr(p, n, APR_SVC_AFE, 0, 0, block, AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST);
	put32(p + 20, block);
	put32(p + 24, handle);
	return n;
}

/* AFE_SVC_CMD_SET_PARAM: the payload in band (no memory map) */
u32 afe_clock_set(u8 *p, u32 clk_id, u32 hz)
{
	u32 ps = 20, n = APR_HDR_SIZE + 16 + 12 + ps;
	mzero(p, n);
	apr_put_hdr(p, n, APR_SVC_AFE, 0, 0, AFE_CLK_TOKEN, AFE_SVC_CMD_SET_PARAM);
	put32(p + 20, 12 + ps);                    /* payload_size */
	put32(p + 36, AFE_MODULE_CLOCK_SET);
	put32(p + 40, AFE_PARAM_ID_CLOCK_SET);
	put16(p + 44, ps);
	u8 *c = p + 48;                            /* struct afe_clk_set */
	put32(c, 1);                               /* minor version */
	put32(c + 4, clk_id);
	put32(c + 8, hz);
	put16(c + 12, 1);                          /* attribute: COUPLE_NO */
	put16(c + 14, 0);                          /* root: default */
	put32(c + 16, hz != 0);
	return n;
}

/* AFE_PORT_CMD_SET_PARAM_V2 with the port's codec DMA configuration, as
 * big as Linux's union of port configurations */
u32 afe_cdc_dma_config(u8 *p, u16 port, u16 token, u32 rate, u16 bits, u16 channels)
{
	u32 ps = AFE_PORT_CONFIG_SIZE, n = APR_HDR_SIZE + 16 + 12 + ps;
	mzero(p, n);
	apr_put_hdr(p, n, APR_SVC_AFE, 0, 0, token, AFE_PORT_CMD_SET_PARAM_V2);
	put16(p + 20, port);
	put16(p + 22, 12 + ps);
	put32(p + 36, AFE_MODULE_AUDIO_DEV_INTERFACE);
	put32(p + 40, AFE_PARAM_ID_CODEC_DMA_CONFIG);
	put16(p + 44, ps);
	u8 *c = p + 48;                            /* struct afe_param_id_cdc_dma_cfg */
	put32(c, 1);                               /* minor version */
	put32(c + 4, rate);
	put16(c + 8, bits);
	put16(c + 10, 0);                          /* linear PCM */
	put16(c + 12, channels);
	put16(c + 14, (1u << channels) - 1);       /* the active channels */
	return n;
}

static u32 afe_port_cmd(u8 *p, u16 port, u16 token, u32 op)
{
	u32 n = APR_HDR_SIZE + 4;
	apr_put_hdr(p, n, APR_SVC_AFE, 0, 0, token, op);
	put16(p + 20, port);
	put16(p + 22, 0);
	return n;
}

u32 afe_port_start(u8 *p, u16 port, u16 token) { return afe_port_cmd(p, port, token, AFE_PORT_CMD_DEVICE_START); }
u32 afe_port_stop(u8 *p, u16 port, u16 token) { return afe_port_cmd(p, port, token, AFE_PORT_CMD_DEVICE_STOP); }

/* --- the voice services (src_port: the path, dest_port: the session) - */

u32 mvm_create_passive(u8 *p, u16 path, const char *name)
{
	u32 n = APR_HDR_SIZE + 20;
	mzero(p, n);
	apr_put_hdr(p, n, APR_SVC_MVM, path, 0, 0, VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION);
	mstrcpy((char *)p + 20, name, 20);
	return n;
}

u32 mvm_dual_control(u8 *p, u16 path, u16 handle)
{
	u32 n = APR_HDR_SIZE + 1;
	apr_put_hdr(p, n, APR_SVC_MVM, path, handle, 0, VSS_IMVM_CMD_SET_POLICY_DUAL_CONTROL);
	p[20] = 1;                                 /* the modem's state machine in control */
	return n;
}

u32 mvm_attach(u8 *p, u16 path, u16 handle, u16 cvp_handle, bool attach)
{
	u32 n = APR_HDR_SIZE + 2;
	apr_put_hdr(p, n, APR_SVC_MVM, path, handle, 0,
		attach ? VSS_IMVM_CMD_ATTACH_VOCPROC : VSS_IMVM_CMD_DETACH_VOCPROC);
	put16(p + 20, cvp_handle);
	return n;
}

u32 mvm_start(u8 *p, u16 path, u16 handle, bool start)
{
	apr_put_hdr(p, APR_HDR_SIZE, APR_SVC_MVM, path, handle, 0,
		start ? VSS_IMVM_CMD_START_VOICE : VSS_IMVM_CMD_STOP_VOICE);
	return APR_HDR_SIZE;
}

/* struct vss_ivocproc_cmd_create_full_control_session_v2_cmd (v3: the
 * same with another opcode) */
u32 cvp_create(u8 *p, u16 path, bool v3, u16 tx_port, u16 rx_port)
{
	u32 n = APR_HDR_SIZE + 44;
	mzero(p, n);
	apr_put_hdr(p, n, APR_SVC_CVP, path, 0, 0,
		v3 ? VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V3 : VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2);
	u8 *c = p + 20;
	put16(c, VSS_IVOCPROC_DIRECTION_RX_TX);
	put16(c + 2, tx_port);
	put32(c + 4, VSS_IVOCPROC_TOPOLOGY_ID_NONE);
	put16(c + 8, rx_port);
	put32(c + 10, VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT);
	put32(c + 14, VSS_ICOMMON_CAL_NETWORK_ID_NONE);
	put32(c + 18, VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING);
	put16(c + 22, VSS_IVOCPROC_PORT_ID_NONE);  /* the echo reference */
	/* name[20]: none */
	return n;
}

/* VSS_ICOMMON_CMD_SET_PARAM_V2 in band: mem_handle, mem_address,
 * mem_size, then the parameter: module, id, size, reserved, data */
static u32 cvp_set_param(u8 *p, u16 path, u16 handle, u32 param_id, u32 dsize)
{
	u32 n = APR_HDR_SIZE + 16 + 12 + dsize;
	mzero(p, n);
	apr_put_hdr(p, n, APR_SVC_CVP, path, handle, 0, VSS_ICOMMON_CMD_SET_PARAM_V2);
	put32(p + 32, 12 + dsize);                 /* mem_size */
	put32(p + 40, param_id);
	put16(p + 44, dsize);
	return n;
}

/* the channels: 1 for the microphone, 2 to the codec */
u32 cvp_channel_info(u8 *p, u16 path, u16 handle, bool tx)
{
	u32 n = cvp_set_param(p, path, handle,
		tx ? VSS_PARAM_VOCPROC_TX_CHANNEL_INFO : VSS_PARAM_VOCPROC_RX_CHANNEL_INFO, 16);
	u8 *d = p + 48;
	u32 ch = tx ? 1 : 2;
	put32(d, ch);
	put32(d + 4, 16);
	for (u32 i = 0; i < ch; i++) d[8 + i] = 1 + i;   /* PCM_CHANNEL_FL, FR */
	return n;
}

u32 cvp_media_format(u8 *p, u16 path, u16 handle, u16 port, bool tx)
{
	u32 n = cvp_set_param(p, path, handle,
		tx ? VSS_PARAM_TX_PORT_ENDPOINT_MEDIA_INFO : VSS_PARAM_RX_PORT_ENDPOINT_MEDIA_INFO, 20);
	u8 *d = p + 48;
	u32 ch = tx ? 1 : 2;
	put32(d, port);
	put16(d + 4, ch);
	put16(d + 6, 16);
	put32(d + 8, 48000);
	/* channel_mapping[8] (Linux writes it through the union's other
	 * member, onto sample_rate; only with CVD 2.3, which the Pro1X's
	 * ADSP does not have) */
	for (u32 i = 0; i < ch; i++) d[12 + i] = 1 + i;
	return n;
}

static u32 voice_simple(u8 *p, u8 svc, u16 path, u16 handle, u32 op)
{
	apr_put_hdr(p, APR_HDR_SIZE, svc, path, handle, 0, op);
	return APR_HDR_SIZE;
}

u32 cvp_topology_commit(u8 *p, u16 path, u16 handle)
{
	return voice_simple(p, APR_SVC_CVP, path, handle, VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT);
}

u32 cvp_enable(u8 *p, u16 path, u16 handle, bool enable)
{
	return voice_simple(p, APR_SVC_CVP, path, handle, enable ? VSS_IVOCPROC_CMD_ENABLE : VSS_IVOCPROC_CMD_DISABLE);
}

u32 voice_destroy(u8 *p, u8 svc, u16 path, u16 handle)
{
	return voice_simple(p, svc, path, handle, APRV2_IBASIC_CMD_DESTROY_SESSION);
}

/* --- the sequences ----------------------------------------------------- */

enum {
	S_VOTE_MACRO, S_VOTE_DCODEC, S_CLK_RX, S_CLK_RX_NPL, S_CLK_TX, S_CLK_TX_NPL,
	S_MVM_CREATE, S_MVM_DUAL, S_CVP_CREATE, S_CVP_CH_RX, S_CVP_CH_TX, S_CVP_MF_RX, S_CVP_MF_TX,
	S_CVP_COMMIT, S_CVP_ENABLE, S_MVM_ATTACH, S_MVM_START,
	S_RX_CFG, S_RX_START, S_TX_CFG, S_TX_START,
	S_TX_STOP, S_RX_STOP, S_MVM_STOP, S_MVM_DETACH, S_CVP_DISABLE, S_CVP_DESTROY, S_MVM_DESTROY,
	S_CLK_TX_NPL_OFF, S_CLK_TX_OFF, S_CLK_RX_NPL_OFF, S_CLK_RX_OFF, S_DEVOTE_DCODEC, S_DEVOTE_MACRO,
	S_END,
};

/* Linux's order: the macros' clocks (their runtime resume), the voice
 * session (the VoiceMMode1 front end's startup), the codec DMA ports (the
 * back ends' prepare); and the other way round */
static const u8 seq_up[] = {
	S_VOTE_MACRO, S_VOTE_DCODEC, S_CLK_RX, S_CLK_RX_NPL, S_CLK_TX, S_CLK_TX_NPL,
	S_MVM_CREATE, S_MVM_DUAL, S_CVP_CREATE, S_CVP_CH_RX, S_CVP_CH_TX, S_CVP_MF_RX, S_CVP_MF_TX,
	S_CVP_COMMIT, S_CVP_ENABLE, S_MVM_ATTACH, S_MVM_START,
	S_RX_CFG, S_RX_START, S_TX_CFG, S_TX_START, S_END,
};
static const u8 seq_down[] = {
	S_TX_STOP, S_RX_STOP, S_MVM_STOP, S_MVM_DETACH, S_CVP_DISABLE, S_CVP_DESTROY, S_MVM_DESTROY,
	S_CLK_TX_NPL_OFF, S_CLK_TX_OFF, S_CLK_RX_NPL_OFF, S_CLK_RX_OFF, S_DEVOTE_DCODEC, S_DEVOTE_MACRO,
	S_END,
};

static int cur_step(const struct voicepath *v)
{
	return (v->seq ? seq_down : seq_up)[v->step];
}

static bool afe_step(int s)
{
	return (s >= S_VOTE_MACRO && s <= S_CLK_TX_NPL) || (s >= S_RX_CFG && s <= S_RX_STOP) ||
	       s >= S_CLK_TX_NPL_OFF;
}

/* the parameters of CVD 2.3 only; the AFE's steps when the ports run already */
static bool skipped(const struct voicepath *v, int s)
{
	if (s == S_END) return false;
	if (!v->ports && afe_step(s)) return true;
	if (s >= S_CVP_CH_RX && s <= S_CVP_COMMIT) return !v->cvd_v2_3;
	if (s == S_MVM_STOP || s == S_MVM_DETACH || s == S_MVM_DESTROY) return !v->mvm;
	if (s == S_CVP_DISABLE || s == S_CVP_DESTROY) return !v->cvp;
	if (s == S_DEVOTE_MACRO) return !v->vote_macro;
	if (s == S_DEVOTE_DCODEC) return !v->vote_dcodec;
	return false;
}

static void send_step(struct voicepath *v, u32 ms)
{
	const u16 path = VOICE_PATH_VOICEMMODE1;
	const u16 rx = AFE_PORT_ID_RX_CODEC_DMA_RX_0, tx = AFE_PORT_ID_VA_CODEC_DMA_TX_0;
	u8 p[128] = { 0 };
	u32 n = 0;
	int s;

	while (skipped(v, s = cur_step(v))) v->step++;
	if (s == S_END) {
		bool failed = v->unwinding && !v->down_wanted;
		v->state = !v->seq ? VP_RUNNING : failed ? VP_FAILED : VP_IDLE;
		if (v->seq) { v->mvm = v->cvp = 0; v->vote_macro = v->vote_dcodec = 0; }
		if (!failed) mstrcpy(v->err, v->seq ? "voice: down" : "voice: up", sizeof(v->err));
		v->unwinding = v->down_wanted = false;
		return;
	}
	v->expect_svc = APR_SVC_AFE;
	switch (s) {
	case S_VOTE_MACRO: n = afe_hw_vote(p, Q6AFE_LPASS_CORE_HW_MACRO_BLOCK, "LPASS_HW_MACRO"); break;
	case S_VOTE_DCODEC: n = afe_hw_vote(p, Q6AFE_LPASS_CORE_HW_DCODEC_BLOCK, "LPASS_HW_DCODEC"); break;
	case S_CLK_RX: n = afe_clock_set(p, Q6AFE_LPASS_CLK_ID_RX_CORE_MCLK, LPASS_MCLK_HZ); break;
	case S_CLK_RX_NPL: n = afe_clock_set(p, Q6AFE_LPASS_CLK_ID_RX_CORE_NPL_MCLK, LPASS_MCLK_HZ); break;
	case S_CLK_TX: n = afe_clock_set(p, Q6AFE_LPASS_CLK_ID_TX_CORE_MCLK, LPASS_MCLK_HZ); break;
	case S_CLK_TX_NPL: n = afe_clock_set(p, Q6AFE_LPASS_CLK_ID_TX_CORE_NPL_MCLK, LPASS_MCLK_HZ); break;
	case S_CLK_TX_NPL_OFF: n = afe_clock_set(p, Q6AFE_LPASS_CLK_ID_TX_CORE_NPL_MCLK, 0); break;
	case S_CLK_TX_OFF: n = afe_clock_set(p, Q6AFE_LPASS_CLK_ID_TX_CORE_MCLK, 0); break;
	case S_CLK_RX_NPL_OFF: n = afe_clock_set(p, Q6AFE_LPASS_CLK_ID_RX_CORE_NPL_MCLK, 0); break;
	case S_CLK_RX_OFF: n = afe_clock_set(p, Q6AFE_LPASS_CLK_ID_RX_CORE_MCLK, 0); break;
	case S_DEVOTE_DCODEC: n = afe_hw_devote(p, Q6AFE_LPASS_CORE_HW_DCODEC_BLOCK, v->vote_dcodec); break;
	case S_DEVOTE_MACRO: n = afe_hw_devote(p, Q6AFE_LPASS_CORE_HW_MACRO_BLOCK, v->vote_macro); break;
	case S_RX_CFG: n = afe_cdc_dma_config(p, rx, TOKEN_RX_CODEC_DMA_RX_0, 48000, 16, 2); break;
	case S_TX_CFG: n = afe_cdc_dma_config(p, tx, TOKEN_VA_CODEC_DMA_TX_0, 48000, 16, 2); break;
	case S_RX_START: n = afe_port_start(p, rx, TOKEN_RX_CODEC_DMA_RX_0); break;
	case S_TX_START: n = afe_port_start(p, tx, TOKEN_VA_CODEC_DMA_TX_0); break;
	case S_RX_STOP: n = afe_port_stop(p, rx, TOKEN_RX_CODEC_DMA_RX_0); break;
	case S_TX_STOP: n = afe_port_stop(p, tx, TOKEN_VA_CODEC_DMA_TX_0); break;
	default:
		v->expect_svc = s == S_MVM_CREATE || s == S_MVM_DUAL || s == S_MVM_ATTACH || s == S_MVM_START ||
			s == S_MVM_STOP || s == S_MVM_DETACH || s == S_MVM_DESTROY ? APR_SVC_MVM : APR_SVC_CVP;
		switch (s) {
		case S_MVM_CREATE: n = mvm_create_passive(p, path, VOICE_SESSION_VOICEMMODE1); break;
		case S_MVM_DUAL: n = mvm_dual_control(p, path, v->mvm); break;
		case S_CVP_CREATE: n = cvp_create(p, path, v->cvd_v2_3, tx, rx); break;
		case S_CVP_CH_RX: n = cvp_channel_info(p, path, v->cvp, false); break;
		case S_CVP_CH_TX: n = cvp_channel_info(p, path, v->cvp, true); break;
		case S_CVP_MF_RX: n = cvp_media_format(p, path, v->cvp, rx, false); break;
		case S_CVP_MF_TX: n = cvp_media_format(p, path, v->cvp, tx, true); break;
		case S_CVP_COMMIT: n = cvp_topology_commit(p, path, v->cvp); break;
		case S_CVP_ENABLE: n = cvp_enable(p, path, v->cvp, true); break;
		case S_CVP_DISABLE: n = cvp_enable(p, path, v->cvp, false); break;
		case S_MVM_ATTACH: n = mvm_attach(p, path, v->mvm, v->cvp, true); break;
		case S_MVM_DETACH: n = mvm_attach(p, path, v->mvm, v->cvp, false); break;
		case S_MVM_START: n = mvm_start(p, path, v->mvm, true); break;
		case S_MVM_STOP: n = mvm_start(p, path, v->mvm, false); break;
		case S_CVP_DESTROY: n = voice_destroy(p, APR_SVC_CVP, path, v->cvp); break;
		case S_MVM_DESTROY: n = voice_destroy(p, APR_SVC_MVM, path, v->mvm); break;
		}
	}
	u32 op = get32(p + 16);
	v->expect_op = s == S_VOTE_MACRO || s == S_VOTE_DCODEC ? AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST : op;
	v->sent_ms = ms;
	v->cmds++;
	if (v->send(v->arg, p, n) < 0) {
		v->chan_err = true;
		v->state = VP_FAILED;
		mstrcpy(v->err, "voice: the ADSP channel refused a command", sizeof(v->err));
	}
}

void voicepath_init(struct voicepath *v, int (*send)(void *, const u8 *, u32), void *arg, bool cvd_v2_3,
		    bool ports)
{
	mzero(v, sizeof(*v));
	v->send = send;
	v->arg = arg;
	v->cvd_v2_3 = cvd_v2_3;
	v->ports = ports;
	mstrcpy(v->err, "voice: idle", sizeof(v->err));
}

static int begin(struct voicepath *v, int seq, int state, u32 ms)
{
	v->seq = seq;
	v->step = 0;
	v->state = state;
	v->chan_err = false;
	send_step(v, ms);
	return v->chan_err ? -1 : 0;
}

int voicepath_up(struct voicepath *v, u32 ms)
{
	if (v->state != VP_IDLE && v->state != VP_FAILED) return -1;
	v->status = 0;
	return begin(v, 0, VP_STARTING, ms);
}

int voicepath_down(struct voicepath *v, u32 ms)
{
	if (v->state == VP_STARTING) {            /* its reply first: the handles */
		v->down_wanted = true;
		return 0;
	}
	if (v->state == VP_IDLE || v->state == VP_STOPPING) return -1;
	v->status = 0;
	return begin(v, 1, VP_STOPPING, ms);
}

/* what failed on the way up is undone as far as it got */
static void fail(struct voicepath *v, u32 ms, const char *why)
{
	mstrcpy(v->err, why, sizeof(v->err));
	if (!v->status) v->status = 1;
	if (!v->seq) {
		v->unwinding = true;
		begin(v, 1, VP_STOPPING, ms);
		return;
	}
	v->step++;                                 /* down: on with the rest */
	send_step(v, ms);
}

void voicepath_rx(struct voicepath *v, const u8 *pkt, u32 len, u32 ms)
{
	struct apr_hdr h;
	if (!apr_get_hdr(pkt, len, &h) || h.src_domain != APR_DOMAIN_ADSP) return;
	if (v->state != VP_STARTING && v->state != VP_STOPPING) return;
	if (h.src_svc != v->expect_svc) return;
	const u8 *pl = pkt + APR_HDR_SIZE;
	u32 pn = h.pkt_size - APR_HDR_SIZE;
	int s = cur_step(v);
	if (h.opcode == AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST &&
	    v->expect_op == AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST) {
		if (pn < 4) return;
		if (s == S_VOTE_MACRO) v->vote_macro = get32(pl);
		else v->vote_dcodec = get32(pl);
	} else if (h.opcode == APR_BASIC_RSP_RESULT) {
		if (pn < 8 || get32(pl) != v->expect_op) return;
		u32 st = get32(pl + 4);
		if (s == S_MVM_CREATE && !v->mvm) v->mvm = h.src_port;
		if (s == S_CVP_CREATE && !v->cvp) v->cvp = h.src_port;
		if (st) {
			v->replies++;
			v->status = st;
			fail(v, ms, "voice: the ADSP refused a command");
			return;
		}
	} else {
		return;                            /* events: not ours */
	}
	v->replies++;
	v->step++;
	if (v->down_wanted && !v->seq) {
		begin(v, 1, VP_STOPPING, ms);
		return;
	}
	send_step(v, ms);
}

void voicepath_poll(struct voicepath *v, u32 ms)
{
	if (v->state != VP_STARTING && v->state != VP_STOPPING) return;
	if (ms - v->sent_ms < TIMEOUT_MS) return;
	fail(v, ms, "voice: no answer from the ADSP");
}
