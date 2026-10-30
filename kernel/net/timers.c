/* KolibriOS for ARM: lwIP's timers on demand (LWIP_TIMERS_CUSTOM).
 *
 * lwIP's own timeouts.c runs its cyclic timers whenever the stack is up:
 * TCP's every 250 ms while any connection exists (an idle one too),
 * DHCP's fine timer every 500 ms, ARP's, IP reassembly's and DNS's every
 * second, DHCP's coarse timer every minute: two to six wake-ups a second
 * on an idle network. Here each runs only while it has something to do,
 * on a common grid from lwIP's start (so that those due together come
 * in one wake-up), and the time an idle module did not count is counted
 * at the next way into the kernel (net_enter), before any of lwIP runs:
 *
 *  - TCP: tcp_fasttmr every 250 ms and tcp_slowtmr every 500 ms (as
 *    tcp_tmr) while a connection has something in flight or pending: an
 *    ACK delayed, data refused, a FIN pending, unacknowledged or unsent
 *    data, the persist timer, segments out of order, a poll callback, or
 *    a state other than ESTABLISHED, CLOSE_WAIT and FIN_WAIT_2. A
 *    connection without any of it needs the slow timer only at its next
 *    time-out: the keepalive's probe (or its end), FIN_WAIT_2's 20 s,
 *    TIME_WAIT's 2 MSL; tcp_slowtmr then finds nothing else to do for it
 *    (its retransmission timer is off, tcp_output has nothing to send).
 *    tcp_ticks, the slow timer's count that those time-outs are measured
 *    in (and the time of a segment's arrival, pcb->tmr), follows the
 *    clock: it is set from the time at every way into the kernel
 *    (net_enter), as if tcp_slowtmr had run every 500 ms;
 *  - DHCP: the fine timer at the 500 ms point a request's time-out is
 *    due (request_timeout), the coarse timer at the minute a lease's
 *    renewal, rebinding or end is due, the points between counted then
 *    (the counters are read only by those timers themselves);
 *  - ARP, IP reassembly, DNS: every second while their state changes by
 *    the second (an ARP request pending or just repeated, a datagram in
 *    pieces, a DNS query unanswered: each began with a frame, so their
 *    time is known from the frames: net_frame); otherwise their only
 *    change is ageing (the ARP table's entries, the DNS cache's TTL, the
 *    pieces' age), counted at the next way into the kernel by calling the
 *    timer once for each second missed, before lwIP looks at its tables.
 *
 * sys_timeouts_sleeptime() is then the time to the next of them, or
 * "never". kolibri.nettimers=periodic runs every timer on its grid
 * always, as lwIP does (a comparison, a fallback). The kernel calls
 * net_enter() at every trap (arch/arm64/trap.c) and net_frame() for
 * every frame in and out (net.c); tests/net runs it on the PC against
 * lwIP's own timeouts.c. */
#include "lwip/opt.h"
#include "lwip/timeouts.h"
#include "lwip/sys.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/etharp.h"
#include "lwip/ip4_frag.h"
#include "lwip/dns.h"
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#include "nettimers.h"

#if !LWIP_TIMERS_CUSTOM
#error "kernel/net/timers.c replaces lwIP's timeouts.c: LWIP_TIMERS_CUSTOM 1"
#endif

#define FOREVER 0xffffffffu

struct net_timer_stats net_timer_stats;
static int on, periodic;
static u32_t t0;                        /* the grid's origin: lwIP's start */

static u32_t since(u32_t now) { return now - t0; }
static int reached(u32_t now, u32_t at) { return (s32_t)(now - at) >= 0; }

/* --- the one-shot timeouts (sys_timeout): lwIP's API, unused by lwIP
 * itself with LWIP_TIMERS_CUSTOM --------------------------------------- */

#define NTIMEO 8
static struct { u32_t at; sys_timeout_handler h; void *arg; } timeo[NTIMEO];

void sys_timeout(u32_t msecs, sys_timeout_handler handler, void *arg)
{
	for (int i = 0; i < NTIMEO; i++)
		if (!timeo[i].h) {
			timeo[i].at = sys_now() + msecs;
			timeo[i].h = handler;
			timeo[i].arg = arg;
			return;
		}
	LWIP_ASSERT("sys_timeout: no room", 0);
}

void sys_untimeout(sys_timeout_handler handler, void *arg)
{
	for (int i = 0; i < NTIMEO; i++)
		if (timeo[i].h == handler && timeo[i].arg == arg) timeo[i].h = NULL;
}

void sys_restart_timeouts(void)
{
}

/* --- TCP ------------------------------------------------------------------- */

enum { TCP_QUIET, TCP_SLOW, TCP_FAST };

/* a timer on the grid: the last point it ran for; awake: needed at the
 * last look (from not needed, its first point is the grid's next one
 * after the time it is needed again, not one long past) */
struct grid {
	u32_t interval, done;
	int awake;
};

static u32_t point(const struct grid *g, u32_t now) { return since(now) / g->interval; }

/* needed: from now on the points after done; not: none, done follows */
static void grid_need(struct grid *g, u32_t now, int need)
{
	if (need && !g->awake) g->done = point(g, now);
	g->awake = need;
}

static u32_t grid_at(const struct grid *g, u32_t n) { return t0 + n * g->interval; }

static struct grid tcp_grid = { TCP_FAST_INTERVAL, 0, 0 };

/* what the connections need: TCP_FAST (the fast timer), TCP_SLOW (both,
 * as tcp_tmr), or TCP_QUIET with *slot the tcp_ticks at which the first
 * quiet one's time-out is due (FOREVER: none) */
static int tcp_need(u32_t *slot)
{
	int need = TCP_QUIET;
	u32_t s = FOREVER;
	for (struct tcp_pcb *p = tcp_active_pcbs; p; p = p->next) {
		if (p->refused_data || (p->flags & (TF_ACK_DELAY | TF_CLOSEPEND))) return TCP_FAST;
		if (p->unacked || p->unsent || p->rtime >= 0 || p->persist_backoff || p->poll ||
#if TCP_QUEUE_OOSEQ
		    p->ooseq ||
#endif
		    (p->flags & (TF_ACK_NOW | TF_NAGLEMEMERR)) ||
		    (p->state != ESTABLISHED && p->state != CLOSE_WAIT && p->state != FIN_WAIT_2)) {
			need = TCP_SLOW;
			continue;
		}
		/* tcp_slowtmr's own conditions: the first tick that meets them */
		if (p->state == FIN_WAIT_2 && (p->flags & TF_RXCLOSED)) {
			u32_t at = p->tmr + TCP_FIN_WAIT_TIMEOUT / TCP_SLOW_INTERVAL + 1;
			if (s == FOREVER || (s32_t)(at - s) < 0) s = at;
		}
		if (ip_get_option(p, SOF_KEEPALIVE) && (p->state == ESTABLISHED || p->state == CLOSE_WAIT)) {
			u32_t sent = p->keep_cnt_sent < p->keep_cnt ? p->keep_cnt_sent : p->keep_cnt;
			u32_t at = p->tmr + (p->keep_idle + sent * p->keep_intvl) / TCP_SLOW_INTERVAL + 1;
			if (s == FOREVER || (s32_t)(at - s) < 0) s = at;
		}
	}
	for (struct tcp_pcb *p = tcp_tw_pcbs; p; p = p->next) {
		u32_t at = p->tmr + 2 * TCP_MSL / TCP_SLOW_INTERVAL + 1;
		if (s == FOREVER || (s32_t)(at - s) < 0) s = at;
	}
	*slot = s;
	return need;
}

/* the time TCP's timer is next due at (FOREVER: no connection, or none
 * with a time-out), and what for */
static u32_t tcp_due(u32_t now, int *what)
{
	u32_t slot = FOREVER;
	int need = TCP_QUIET;
	if (tcp_active_pcbs || tcp_tw_pcbs) need = periodic ? TCP_SLOW : tcp_need(&slot);
	*what = need;
	grid_need(&tcp_grid, now, need != TCP_QUIET);
	u32_t next = tcp_grid.done + 1, even = (tcp_grid.done + 2) & ~1u;
	if (need == TCP_FAST) return grid_at(&tcp_grid, next);
	if (need == TCP_SLOW) return grid_at(&tcp_grid, even);
	if (slot == FOREVER) return FOREVER;
	/* the slot's point (an even one: tcp_slowtmr's); one past (its probe
	 * not sent: no memory, no route) again at the next slow point after
	 * the last run, as tcp_tmr would */
	u32_t at = t0 + slot * TCP_SLOW_INTERVAL, e = grid_at(&tcp_grid, even);
	if ((s32_t)(at - e) < 0) at = e;
	return reached(now, at) ? now : at;
}

/* tcp_ticks from the clock: the 500 ms slots since the start; never back */
static void tcp_ticks_now(u32_t now)
{
	u32_t slot = since(now) / TCP_SLOW_INTERVAL;
	if ((s32_t)(slot - tcp_ticks) > 0) tcp_ticks = slot;
}

static void tcp_run(u32_t now)
{
	int what;
	u32_t due = tcp_due(now, &what);
	if (due == FOREVER || !reached(now, due)) return;
	u32_t p = point(&tcp_grid, now);
	tcp_grid.done = p;
	tcp_fasttmr();
	net_timer_stats.tcp_fast++;
	/* the slow timer at its points, and for a quiet one's time-out (a
	 * point missed: now); it counts tcp_ticks up itself */
	if (what != TCP_FAST || !(p & 1)) {
		tcp_ticks = since(now) / TCP_SLOW_INTERVAL - 1;
		tcp_slowtmr();
		net_timer_stats.tcp_slow++;
	}
}

/* --- DHCP --------------------------------------------------------------------- */

#if LWIP_DHCP
static struct grid fine = { DHCP_FINE_TIMER_MSECS, 0, 0 }, coarse = { DHCP_COARSE_TIMER_MSECS, 0, 0 };

/* the fine timer's calls until a request's time-out (request_timeout:
 * dhcp_fine_tmr counts it down, read by nothing else) */
static u32_t dhcp_points(void)
{
	u32_t m = FOREVER;
	struct netif *n;
	NETIF_FOREACH(n) {
		struct dhcp *d = netif_dhcp_data(n);
		if (d && d->request_timeout) m = LWIP_MIN(m, (u32_t)d->request_timeout);
	}
	return m;
}

/* the coarse timer's calls until the first of a lease's events (its end,
 * the rebinding, the renewal: dhcp_coarse_tmr's three counters) */
static u32_t dhcp_minutes(void)
{
	u32_t m = FOREVER;
	struct netif *n;
	NETIF_FOREACH(n) {
		struct dhcp *d = netif_dhcp_data(n);
		if (!d || d->state == DHCP_STATE_OFF) continue;
		if (d->t0_timeout) m = LWIP_MIN(m, d->t0_timeout > d->lease_used ? (u32_t)(d->t0_timeout - d->lease_used) : 1u);
		if (d->t2_rebind_time) m = LWIP_MIN(m, (u32_t)d->t2_rebind_time);
		if (d->t1_renew_time) m = LWIP_MIN(m, (u32_t)d->t1_renew_time);
	}
	return m;
}

static u32_t fine_due(u32_t now)
{
	u32_t m = periodic ? 1 : dhcp_points();
	grid_need(&fine, now, m != FOREVER);
	return m == FOREVER ? FOREVER : grid_at(&fine, fine.done + m);
}

static u32_t coarse_due(u32_t now)
{
	u32_t m = periodic ? 1 : dhcp_minutes();
	grid_need(&coarse, now, m != FOREVER);
	return m == FOREVER ? FOREVER : grid_at(&coarse, coarse.done + m);
}

static void dhcp_run(u32_t now)
{
	/* each timer's points since its last call counted, the last one
	 * the request's time-out or the lease's event */
	u32_t due = fine_due(now);
	if (due != FOREVER && reached(now, due)) {
		u32_t p = point(&fine, now);
		for (u32_t k = p - fine.done; k && k < 100000; k--) {
			dhcp_fine_tmr();
			net_timer_stats.dhcp_fine++;
		}
		fine.done = p;
	}
	due = coarse_due(now);
	if (due != FOREVER && reached(now, due)) {
		u32_t minute = point(&coarse, now);
		for (u32_t k = minute - coarse.done; k && k < 100000; k--) {
			dhcp_coarse_tmr();
			net_timer_stats.dhcp_coarse++;
		}
		coarse.done = minute;
	}
}
#endif

/* --- ARP, IP reassembly, DNS: a second's grid, the missed ones counted --- */

struct lazy {
	void (*tmr)(void);
	u32_t done;                     /* the seconds done */
	u32_t busy_until;               /* on the grid until then, then idle */
	int busy;
	u32_t most;                     /* the calls that change anything after a while idle */
	u32_t *runs;
};

enum { L_ARP, L_REASS, L_DNS, NLAZY };
static struct lazy lazy[NLAZY] = {
#if LWIP_ARP
	[L_ARP] = { etharp_tmr, 0, 0, 0, ARP_MAXAGE + 2, &net_timer_stats.arp },
#endif
#if IP_REASSEMBLY
	[L_REASS] = { ip_reass_tmr, 0, 0, 0, IP_REASS_MAXAGE + 2, &net_timer_stats.reass },
#endif
#if LWIP_DNS
	[L_DNS] = { dns_tmr, 0, 0, 0, 604800 + 2, &net_timer_stats.dns },     /* DNS_MAX_TTL */
#endif
};

static int lazy_busy(struct lazy *l, u32_t now)
{
	if (l->busy && !reached(now, l->busy_until)) return 1;
	l->busy = 0;
	return periodic;
}

/* the module's state changes by the second for a while: from that frame on */
static void lazy_wake(int i, u32_t ms)
{
	struct lazy *l = &lazy[i];
	if (!l->tmr) return;
	u32_t until = sys_now() + ms;
	if (!l->busy || (s32_t)(until - l->busy_until) > 0) l->busy_until = until;
	l->busy = 1;
}

/* the seconds missed counted: the timer called once for each (as many
 * as change anything), so that its tables are as they would be */
static void lazy_catch_up(u32_t now)
{
	u32_t sec = since(now) / 1000;
	for (int i = 0; i < NLAZY; i++) {
		struct lazy *l = &lazy[i];
		if (!l->tmr || (s32_t)(sec - l->done) <= 0) continue;
		u32_t k = sec - l->done;
		if (k > l->most) k = l->most;
		while (k--) {
			l->tmr();
			(*l->runs)++;
		}
		l->done = sec;
	}
}

static u32_t lazy_due(u32_t now)
{
	u32_t due = FOREVER;
	for (int i = 0; i < NLAZY; i++) {
		struct lazy *l = &lazy[i];
		if (!l->tmr || !lazy_busy(l, now)) continue;
		u32_t at = t0 + (l->done + 1) * 1000;
		if (due == FOREVER || (s32_t)(at - due) < 0) due = at;
	}
	return due;
}

/* --- lwIP's interface ------------------------------------------------------- */

/* the next time net_enter has something to count: a 500 ms slot's start
 * (tcp_ticks), every second's among them (the idle timers') */
static u32_t next_enter;

void sys_timeouts_init(void)
{
	t0 = next_enter = sys_now();
	on = 1;
}

void net_timers_periodic(int yes)
{
	periodic = yes;
}

void net_enter(void)
{
	if (!on) return;
	u32_t now = sys_now();
	if (!reached(now, next_enter)) return;
	tcp_ticks_now(now);
	lazy_catch_up(now);
	next_enter = t0 + (since(now) / TCP_SLOW_INTERVAL + 1) * TCP_SLOW_INTERVAL;
}

void sys_check_timeouts(void)
{
	if (!on) return;
	u32_t now = sys_now();
	net_enter();
	tcp_run(now);
#if LWIP_DHCP
	dhcp_run(now);
#endif
	for (int i = 0; i < NTIMEO; i++)
		if (timeo[i].h && reached(now, timeo[i].at)) {
			sys_timeout_handler h = timeo[i].h;
			timeo[i].h = NULL;
			h(timeo[i].arg);
		}
}

u32_t sys_timeouts_sleeptime(void)
{
	if (!on) return SYS_TIMEOUTS_SLEEPTIME_INFINITE;
	int what;
	u32_t now = sys_now(), due = tcp_due(now, &what);
#if LWIP_DHCP
	u32_t d = fine_due(now);
	if (d != FOREVER && (due == FOREVER || (s32_t)(d - due) < 0)) due = d;
	d = coarse_due(now);
	if (d != FOREVER && (due == FOREVER || (s32_t)(d - due) < 0)) due = d;
#endif
	u32_t d2 = lazy_due(now);
	if (d2 != FOREVER && (due == FOREVER || (s32_t)(d2 - due) < 0)) due = d2;
	for (int i = 0; i < NTIMEO; i++)
		if (timeo[i].h && (due == FOREVER || (s32_t)(timeo[i].at - due) < 0)) due = timeo[i].at;
	if (due == FOREVER) return SYS_TIMEOUTS_SLEEPTIME_INFINITE;
	return reached(now, due) ? 0 : due - now;
}

/* --- the frames: when ARP, reassembly and DNS have time-outs running ---- */

static u16_t be16(const u8_t *p) { return (u16_t)(p[0] << 8 | p[1]); }

void net_frame(const void *frame, u32_t len, int out)
{
	const u8_t *f = frame;
	if (!on || len < 14) return;
	u16_t type = be16(f + 12);
	/* an ARP request of ours: an entry pending (a second's repeats,
	 * ARP_MAXPENDING of them) or re-requested (two seconds) */
	if (type == 0x0806 && out && len >= 22 && be16(f + 20) == 1) lazy_wake(L_ARP, 3000);
	if (type != 0x0800 || len < 34) return;
	const u8_t *ip = f + 14;
	u32_t ihl = (ip[0] & 15) * 4u;
	/* a piece of a datagram: reassembled within IP_REASS_MAXAGE s */
	if (!out && (be16(ip + 6) & 0x3fff)) lazy_wake(L_REASS, (IP_REASS_MAXAGE + 1) * 1000);
	/* a DNS query: repeated after 1, 1, 2, 3 s, given up 3 s after the last */
	if (out && ip[9] == 17 && len >= 14 + ihl + 4 && be16(ip + ihl + 2) == 53) lazy_wake(L_DNS, 4000);
}
