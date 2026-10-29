/* KolibriOS for ARM: the sound of a call on the Pro1X (callaudio.h) */
#include "callaudio.h"
#include "apr.h"

#define AW87339_REG_CHIPID 0x00
#define AW87339_REG_SYSCTRL 0x01
#define AW87339_CHIPID 0x39
#define DRAIN_MS 20
#define APR_CHANNEL "apr_audio_svc"

static struct callaudio_hw hw;
static bool started, amp_on;
static int route = CALLAUDIO_EARPIECE, last_call;
static u32 drained, last_drain, now;
/* the voice session on the adopted channel */
static struct glink_chan *chan;
static struct voicepath vp;
static u32 vp_call;                    /* the call it was started for, 0: none */
static int vp_tries, vp_shown = -1;

/* aw87339_music_seq: the gain 0x0c, enabled last */
static const u8 aw_seq[][2] = {
	{ 0x01, 0x06 }, { 0x02, 0xa3 }, { 0x03, 0x06 }, { 0x04, 0x05 },
	{ 0x05, 0x0c }, { 0x06, 0x07 }, { 0x07, 0x52 }, { 0x08, 0x03 },
	{ 0x09, 0x08 }, { 0x0a, 0x96 }, { 0x62, 0x01 }, { 0x63, 0x00 },
	{ 0x64, 0x00 }, { 0x01, 0x0e },
};

bool aw87339_on(const struct callaudio_hw *h, u8 addr, int gpio)
{
	u8 id = 0;
	h->gpio_set(gpio, true);
	h->delay_us(2500);
	if (!h->i2c_read(addr, AW87339_REG_CHIPID, &id) || id != AW87339_CHIPID) {
		mlog("callaudio: no AW87339 at 0x%x (%x)\n", addr, id);
		h->gpio_set(gpio, false);
		return false;
	}
	for (u32 i = 0; i < sizeof(aw_seq) / sizeof(aw_seq[0]); i++)
		if (!h->i2c_write(addr, aw_seq[i][0], aw_seq[i][1])) {
			mlog("callaudio: AW87339 at 0x%x: write %x failed\n", addr, aw_seq[i][0]);
			aw87339_off(h, addr, gpio);
			return false;
		}
	return true;
}

void aw87339_off(const struct callaudio_hw *h, u8 addr, int gpio)
{
	h->i2c_write(addr, AW87339_REG_SYSCTRL, 0x00);
	h->gpio_set(gpio, false);
}

/* --- finding Linux's channel ------------------------------------------- */

/* the services a packet may come from or go to (Linux's APR devices) */
static bool apr_svc(u8 s)
{
	return s == 0x03 || s == APR_SVC_AFE || s == 0x07 || s == 0x08 || s == APR_SVC_MVM || s == 0x0a ||
	       s == APR_SVC_CVP;
}

/* a whole APR packet: header length 5 words, type event, response or
 * command, the length the GLINK chunk's (padding at most), the service on
 * both ends the same, from one domain to the other */
static bool apr_packet(const u8 *p, u32 len, u8 from, u8 to)
{
	if (len < APR_HDR_SIZE) return false;
	u16 f = get16(p), size = get16(p + 2);
	if (f >= 0x300 || (f & 0xf0) != 0x50 || size < APR_HDR_SIZE || size > len || len - size >= 8)
		return false;
	return p[5] == from && p[9] == to && p[4] == p[8] && apr_svc(p[4]);
}

bool callaudio_apr_from_apps(const u8 *p, u32 len) { return apr_packet(p, len, APR_DOMAIN_APPS, APR_DOMAIN_ADSP); }
bool callaudio_apr_from_adsp(const u8 *p, u32 len) { return apr_packet(p, len, APR_DOMAIN_ADSP, APR_DOMAIN_APPS); }

static void apr_rx(struct glink_chan *c, const u8 *data, u32 len, void *arg)
{
	voicepath_rx(&vp, data, len, now);
}

static int apr_send(void *arg, const u8 *pkt, u32 len)
{
	return glink_send(chan, pkt, len);
}

/* Linux's ids for apr_audio_svc: its data, newest first (the streams of
 * pro1x-kexec.sh -a talk APR until the jump), else the OPENs of boot */
static void adopt(struct glink *g)
{
	s32 lcid = glink_scan_data(g, true, callaudio_apr_from_apps);
	s32 rcid = glink_scan_data(g, false, callaudio_apr_from_adsp);
	const char *how = "its data";
	if (lcid < 0 || rcid < 0) {
		lcid = glink_scan_open(g, true, APR_CHANNEL);
		rcid = glink_scan_open(g, false, APR_CHANNEL);
		how = "its OPENs";
	}
	if (lcid < 0 || rcid < 0) {
		mlog("callaudio: Linux's APR channel not found in the ADSP's rings: calls without sound\n");
		return;
	}
	chan = glink_adopt(g, APR_CHANNEL, lcid, rcid, apr_rx, 0);
	if (!chan) return;
	mlog("callaudio: Linux's APR channel taken over (lcid %d, rcid %d, from %s)\n", lcid, rcid, how);
	/* what Linux left unread: received and given back, before any command */
	glink_poll(g);
	voicepath_init(&vp, apr_send, 0, false, false);
}

void callaudio_init(const struct callaudio_hw *h)
{
	hw = *h;
	started = true;
	amp_on = false;
	route = CALLAUDIO_EARPIECE;
	last_call = CALLAUDIO_CALL_NONE;
	drained = last_drain = 0;
	chan = 0;
	vp_call = 0;
	vp_tries = 0;
	vp_shown = -1;
	mzero(&vp, sizeof(vp));
	if (!hw.prepared) {
		mlog("callaudio: not prepared (pro1x-kexec.sh -a): calls without sound\n");
		return;
	}
	if (hw.adsp) adopt(hw.adsp);
	mlog("callaudio: %s\n", chan ? "the voice session per call, earpiece and microphone from Linux"
		: hw.adsp ? "no voice session" : "no ADSP: the speaker switch only");
}

/* No channel: what the ADSP sends to Linux's channels thrown away, the
 * receive FIFO's tail moved to its head, so that it does not block */
static void drain(void)
{
	struct glink *g = hw.adsp;
	if (!g || !g->rx_fifo || !g->rx_len) return;
	u32 tail = sm_rd32(g->desc + 8), head = sm_rd32(g->desc + 12);
	if (head == tail || head >= g->rx_len || tail >= g->rx_len) return;
	drained += glink_fifo_avail(head, tail, g->rx_len);
	sm_barrier();
	sm_wr32(g->desc + 8, head);
	sm_barrier();
	if (g->kick) g->kick();                  /* it may wait for room */
}

static void amp(bool on)
{
	if (on == amp_on) return;
	if (on) {
		amp_on = aw87339_on(&hw, AW87339_SPK0_ADDR, AW87339_SPK0_GPIO);
		if (!amp_on) route = CALLAUDIO_EARPIECE;   /* not tried again every tick */
	} else {
		aw87339_off(&hw, AW87339_SPK0_ADDR, AW87339_SPK0_GPIO);
		amp_on = false;
	}
	mlog("callaudio: speaker %s\n", amp_on ? "on" : "off");
}

/* As Linux's pro1x-voice-call: up once a call exists, down when it is
 * over, anew for another call; one more try when the call is answered
 * (the modem's own session may come only then) */
static void voice(int call, u32 id)
{
	glink_poll(hw.adsp);
	voicepath_poll(&vp, now);
	int s = vp.state;
	if (call == CALLAUDIO_CALL_NONE) {
		if (s == VP_RUNNING || s == VP_STARTING) voicepath_down(&vp, now);
		vp_call = 0;
		vp_tries = 0;
	} else if (vp_call != id) {
		if (s == VP_RUNNING || s == VP_STARTING) voicepath_down(&vp, now);
		else if (s == VP_IDLE || s == VP_FAILED) {
			vp_call = id;
			vp_tries = 1;
			voicepath_up(&vp, now);
		}
	} else if (s == VP_FAILED && call == CALLAUDIO_CALL_ACTIVE && vp_tries < 2) {
		vp_tries = 2;
		voicepath_up(&vp, now);
	}
	if (vp.state != vp_shown && vp.state != VP_STARTING && vp.state != VP_STOPPING) {
		vp_shown = vp.state;
		if (vp.state == VP_FAILED)
			mlog("callaudio: %s (status %x)\n", vp.err, vp.status);
		else
			mlog("callaudio: %s\n", vp.err);
	}
}

void callaudio_poll(u32 ms, int call, u32 call_id)
{
	if (!started || !hw.prepared) return;
	now = ms;
	if (chan) voice(call, call_id);
	else if (ms - last_drain >= DRAIN_MS) {
		last_drain = ms;
		drain();
	}
	if (last_call != CALLAUDIO_CALL_NONE && call == CALLAUDIO_CALL_NONE)
		route = CALLAUDIO_EARPIECE;              /* the next call: the earpiece */
	last_call = call;
	amp(call != CALLAUDIO_CALL_NONE && route == CALLAUDIO_SPEAKER);
}

int callaudio_set_route(int r)
{
	if (callaudio_state() != CALLAUDIO_PREPARED || (r != CALLAUDIO_EARPIECE && r != CALLAUDIO_SPEAKER))
		return -1;
	route = r;
	return 0;
}

int callaudio_route(void) { return route; }
/* prepared, and the voice session can be had (or there is no ADSP to
 * have it from: QEMU) */
int callaudio_state(void) { return started && hw.prepared && (chan || !hw.adsp) ? CALLAUDIO_PREPARED : CALLAUDIO_NONE; }
int callaudio_voice(void) { return chan ? vp.state : VP_IDLE; }
u32 callaudio_drained(void) { return drained; }
