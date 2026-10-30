/* lwIP's timers on demand (kernel/net/timers.c) against lwIP's own
 * (timeouts.c), on the PC: make -C tests/net
 *
 * The same program is built twice: net-kos with the kernel's timers,
 * net-lwip with lwIP's cyclic ones; run.py runs every scenario with both
 * and compares what they did. lwIP (the kernel's options) has one
 * Ethernet interface, 10.0.0.2 (or by DHCP), on a simulated wire with a
 * router, 10.0.0.1: it answers ARP for itself, is a DHCP server, and
 * reflects TCP: a segment to 10.0.0.1 comes back with source and
 * destination swapped (the checksums stay right), so that a connection
 * from 10.0.0.2 to 10.0.0.1:80 reaches the listener on 10.0.0.2:80 and
 * both of its ends are this lwIP, under the timers being tested. 5 ms
 * each way; the scenarios drop segments, close the window, cut the wire.
 *
 * The clock is simulated: the program jumps from one event to the next
 * (lwIP's next timeout, sys_timeouts_sleeptime; a frame's arrival; the
 * scenario's next step), and counts those jumps as wake-ups (by a timer,
 * or by a frame or a step), as the kernel's tickless idle would have
 * them. Out (stdout), one line each:
 *
 *   F <ms> <frame>     a frame lwIP sent (ARP, DHCP, TCP with the sequence
 *                      numbers relative to each side's first)
 *   E <ms> <event>     the applications' (connected, accepted, received,
 *                      errors), the connections' lists' sizes
 *   W <ms> <name> <timer wake-ups> <all wake-ups>   a phase's wake-ups
 *
 *   net-kos SCENARIO   (run.py has the list and what is checked) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/udp.h"
#include "lwip/timeouts.h"
#include "lwip/inet_chksum.h"
#include "netif/ethernet.h"
#if LWIP_TIMERS_CUSTOM
#include "nettimers.h"
#else
static void net_enter(void) {}
static void net_frame(const void *f, u32_t len, int out) { (void)f, (void)len, (void)out; }
#endif

#define INF 0xffffffffu
static u32_t now;
u32_t sys_now(void) { return now; }

static const char *scen;
#define IS(s) (!strcmp(scen, s))

static const u8_t our_mac[6] = { 2, 0, 0, 0, 0, 2 }, rt_mac[6] = { 2, 0, 0, 0, 0, 1 };
#define RT_IP 0x0100000au       /* 10.0.0.1 in network order */
#define OUR_IP 0x0200000au
static struct netif nif;

/* --- the wire: frames to lwIP, later ---------------------------------------- */

#define QN 4096
static struct { u32_t at; u16_t len; u8_t b[1514]; } q[QN];
static int nq;

static void to_lwip(const u8_t *f, u32_t len, u32_t delay)
{
	if (nq == QN || len > 1514) return;
	q[nq].at = now + delay;
	q[nq].len = (u16_t)len;
	memcpy(q[nq].b, f, len);
	nq++;
}

static u32_t q_next(void)
{
	u32_t t = INF;
	for (int i = 0; i < nq; i++)
		if (t == INF || (s32_t)(q[i].at - t) < 0) t = q[i].at;
	return t;
}

static void q_deliver(void)
{
	for (;;) {
		int b = -1;
		for (int i = 0; i < nq; i++)
			if ((s32_t)(now - q[i].at) >= 0 && (b < 0 || (s32_t)(q[i].at - q[b].at) < 0)) b = i;
		if (b < 0) return;
		u8_t f[1514];
		u16_t len = q[b].len;
		memcpy(f, q[b].b, len);
		memmove(&q[b], &q[b + 1], (size_t)(nq - b - 1) * sizeof(q[0]));
		nq--;
		net_frame(f, len, 0);
		struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
		if (!p) continue;
		pbuf_take(p, f, len);
		if (nif.input(p, &nif) != ERR_OK) pbuf_free(p);
	}
}

/* --- the router ----------------------------------------------------------------- */

static int dhcp_silent;         /* the DHCP server stops answering */
static u32_t lease = 600;
static int cut;                 /* the reflector drops everything */
static int drop_syn81;          /* no answer to SYNs for port 81 */
static int ndata, drop_every, burst_at, burst_n;   /* tcp-bulk's losses */
static int answered_arp;

static u16_t be16(const u8_t *p) { return (u16_t)(p[0] << 8 | p[1]); }
static u32_t be32(const u8_t *p) { return (u32_t)p[0] << 24 | (u32_t)p[1] << 16 | (u32_t)p[2] << 8 | p[3]; }
static void put16(u8_t *p, u16_t v) { p[0] = (u8_t)(v >> 8), p[1] = (u8_t)v; }

/* the trace's sequence numbers: relative to each port's first */
static struct { u16_t port; u32_t isn; } isn[16];
static u32_t rel(u16_t port, u32_t seq, int syn)
{
	for (int i = 0; i < 16; i++)
		if (isn[i].port == port) {
			if (syn) isn[i].isn = seq;
			return seq - isn[i].isn;
		}
	for (int i = 0; i < 16; i++)
		if (!isn[i].port) {
			isn[i].port = port, isn[i].isn = seq;
			return 0;
		}
	return seq;
}

static void dhcp_reply(const u8_t *f, u32_t len, int type)
{
	static u8_t r[342];
	memset(r, 0, sizeof(r));
	memset(r, 0xff, 6);
	memcpy(r + 6, rt_mac, 6);
	put16(r + 12, 0x0800);
	u8_t *ip = r + 14, *udp = ip + 20, *b = udp + 8;
	ip[0] = 0x45, ip[8] = 64, ip[9] = 17;
	put16(ip + 2, (u16_t)(sizeof(r) - 14));
	memcpy(ip + 12, "\x0a\x00\x00\x01", 4);
	memset(ip + 16, 0xff, 4);
	u16_t ck = inet_chksum(ip, 20);         /* as it is stored */
	memcpy(ip + 10, &ck, 2);
	put16(udp, 67), put16(udp + 2, 68), put16(udp + 4, (u16_t)(sizeof(r) - 34));
	const u8_t *rq = f + 42;
	b[0] = 2, b[1] = 1, b[2] = 6;
	memcpy(b + 4, rq + 4, 4);               /* xid */
	memcpy(b + 16, "\x0a\x00\x00\x02", 4);  /* yiaddr */
	memcpy(b + 28, rq + 28, 16);            /* chaddr */
	memcpy(b + 236, "\x63\x82\x53\x63", 4);
	u8_t *o = b + 240;
	*o++ = 53, *o++ = 1, *o++ = (u8_t)type;
	*o++ = 54, *o++ = 4, memcpy(o, "\x0a\x00\x00\x01", 4), o += 4;
	*o++ = 51, *o++ = 4, o[0] = (u8_t)(lease >> 24), o[1] = (u8_t)(lease >> 16), o[2] = (u8_t)(lease >> 8), o[3] = (u8_t)lease, o += 4;
	*o++ = 1, *o++ = 4, memcpy(o, "\xff\xff\xff\x00", 4), o += 4;
	*o++ = 3, *o++ = 4, memcpy(o, "\x0a\x00\x00\x01", 4), o += 4;
	*o++ = 255;
	(void)len;
	to_lwip(r, sizeof(r), 2);
}

/* a frame from lwIP: traced, answered or reflected */
static void router(const u8_t *f, u32_t len)
{
	u16_t type = be16(f + 12);
	if (type == 0x0806) {
		int op = be16(f + 20);
		u32_t tip = (u32_t)f[38] | (u32_t)f[39] << 8 | (u32_t)f[40] << 16 | (u32_t)f[41] << 24;
		printf("F %u arp-%s %u.%u.%u.%u\n", now, op == 1 ? "req" : "reply", f[38], f[39], f[40], f[41]);
		if (op == 1 && tip == RT_IP && !cut) {
			u8_t r[42];
			memcpy(r, f + 6, 6), memcpy(r + 6, rt_mac, 6);
			memcpy(r + 12, f + 12, 8);
			put16(r + 20, 2);
			memcpy(r + 22, rt_mac, 6), memcpy(r + 28, f + 38, 4);
			memcpy(r + 32, f + 22, 6), memcpy(r + 38, f + 28, 4);
			answered_arp++;
			to_lwip(r, 42, 1);
		}
		return;
	}
	if (type != 0x0800) return;
	const u8_t *ip = f + 14;
	u32_t ihl = (ip[0] & 15) * 4u;
	const u8_t *l4 = ip + ihl;
	if (ip[9] == 17) {
		u16_t sp = be16(l4), dp = be16(l4 + 2);
		if (sp == 68 && dp == 67) {
			const u8_t *o = l4 + 8 + 240;
			int t = 0;
			for (; o < f + len && *o != 255; o += 2 + o[1])
				if (o[0] == 53) t = o[2];
			static const char *const names[] = { "?", "discover", "offer", "request", "decline", "ack", "nak", "release", "inform" };
			printf("F %u dhcp-%s%s\n", now, t < 9 ? names[t] : "?", ip[16] == 255 ? " broadcast" : "");
			if (!dhcp_silent && (t == 1 || t == 3)) dhcp_reply(f, len, t == 1 ? 2 : 5);
		} else {
			printf("F %u udp %u>%u\n", now, sp, dp);
		}
		return;
	}
	if (ip[9] != 6) return;
	u16_t sp = be16(l4), dp = be16(l4 + 2);
	u8_t fl = l4[13];
	u32_t dlen = be16(ip + 2) - ihl - (l4[12] >> 4) * 4u;
	u32_t s = rel(sp, be32(l4 + 4), fl & 2), a = (fl & 16) ? rel(dp, be32(l4 + 8), 0) : 0;
	char fs[8], *p = fs;
	if (fl & 2) *p++ = 'S';
	if (fl & 1) *p++ = 'F';
	if (fl & 4) *p++ = 'R';
	if (fl & 8) *p++ = 'P';
	if (fl & 16) *p++ = '.';
	*p = 0;
	printf("F %u tcp %u>%u %s s=%u a=%u l=%u w=%u\n", now, sp, dp, fs, s, a, dlen, be16(l4 + 14));
	if (cut || (drop_syn81 && dp == 81 && (fl & 2))) return;
	if (dlen && drop_every && sp != 80) {
		ndata++;
		if (ndata % drop_every == 0 || (ndata >= burst_at && ndata < burst_at + burst_n)) return;
	}
	u8_t r[1514];
	memcpy(r, f, len);
	memcpy(r, our_mac, 6), memcpy(r + 6, rt_mac, 6);
	memcpy(r + 14 + 12, ip + 16, 4), memcpy(r + 14 + 16, ip + 12, 4);
	to_lwip(r, len, 5);
}

static err_t link_out(struct netif *n, struct pbuf *p)
{
	(void)n;
	u8_t f[1514];
	if (p->tot_len > sizeof(f)) return ERR_BUF;
	pbuf_copy_partial(p, f, p->tot_len, 0);
	net_frame(f, p->tot_len, 1);
	router(f, p->tot_len);
	return ERR_OK;
}

static err_t if_init(struct netif *n)
{
	n->name[0] = 'e', n->name[1] = '0';
	n->hwaddr_len = 6;
	memcpy(n->hwaddr, our_mac, 6);
	n->mtu = 1500;
	n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
	n->output = etharp_output;
	n->linkoutput = link_out;
	return ERR_OK;
}

/* --- the applications: a listener on :80, a client, UDP ------------------- */

static struct tcp_pcb *cli, *srv;
static u32_t to_send, sent, srv_got, srv_held, srv_sum, cli_got;
static int srv_reading = 1, srv_bad;

static u8_t data_at(u32_t i) { return (u8_t)(i * 7 + 3); }

static void cli_push(void)
{
	while (cli && sent < to_send && tcp_sndbuf(cli) > 0) {
		u8_t b[1024];
		u32_t n = LWIP_MIN(LWIP_MIN((u32_t)sizeof(b), to_send - sent), (u32_t)tcp_sndbuf(cli));
		for (u32_t i = 0; i < n; i++) b[i] = data_at(sent + i);
		if (tcp_write(cli, b, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
		sent += n;
	}
	if (cli) tcp_output(cli);
}

static err_t cli_sent(void *arg, struct tcp_pcb *p, u16_t len)
{
	(void)arg, (void)p, (void)len;
	cli_push();
	return ERR_OK;
}

static err_t cli_recv(void *arg, struct tcp_pcb *p, struct pbuf *pb, err_t err)
{
	(void)arg, (void)err;
	if (!pb) {
		printf("E %u client-eof\n", now);
		return ERR_OK;
	}
	cli_got += pb->tot_len;
	tcp_recved(p, pb->tot_len);
	pbuf_free(pb);
	return ERR_OK;
}

static void cli_err(void *arg, err_t err)
{
	(void)arg;
	printf("E %u client-error %d\n", now, err);
	cli = NULL;
}

static err_t cli_connected(void *arg, struct tcp_pcb *p, err_t err)
{
	(void)arg, (void)p;
	printf("E %u connected %d\n", now, err);
	cli_push();
	return ERR_OK;
}

static err_t srv_recv(void *arg, struct tcp_pcb *p, struct pbuf *pb, err_t err)
{
	(void)arg, (void)err;
	if (!pb) {
		printf("E %u server-eof %u sum %u\n", now, srv_got, srv_sum);
		return ERR_OK;
	}
	u32_t at = srv_got;
	for (struct pbuf *q2 = pb; q2; q2 = q2->next)
		for (u16_t i = 0; i < q2->len; i++, at++) {
			if (((u8_t *)q2->payload)[i] != data_at(at) && !srv_bad) {
				srv_bad = 1;
				printf("E %u corrupt at %u\n", now, at);
			}
			srv_sum = srv_sum * 31 + ((u8_t *)q2->payload)[i];
		}
	srv_got += pb->tot_len;
	if (srv_reading) tcp_recved(p, pb->tot_len);
	else srv_held += pb->tot_len;
	pbuf_free(pb);
	return ERR_OK;
}

static void srv_err(void *arg, err_t err)
{
	(void)arg;
	printf("E %u server-error %d\n", now, err);
	srv = NULL;
}

static err_t srv_accept(void *arg, struct tcp_pcb *p, err_t err)
{
	(void)arg, (void)err;
	printf("E %u accepted\n", now);
	srv = p;
	tcp_recv(p, srv_recv);
	tcp_err(p, srv_err);
	return ERR_OK;
}

static void listen80(void)
{
	struct tcp_pcb *l = tcp_new();
	tcp_bind(l, IP_ADDR_ANY, 80);
	l = tcp_listen(l);
	tcp_accept(l, srv_accept);
}

static void connect_to(u16_t port)
{
	cli = tcp_new();
	tcp_recv(cli, cli_recv);
	tcp_sent(cli, cli_sent);
	tcp_err(cli, cli_err);
	ip_addr_t a;
	IP4_ADDR(ip_2_ip4(&a), 10, 0, 0, 1);
	tcp_connect(cli, &a, port, cli_connected);
}

static void udp_to(u8_t last)
{
	static struct udp_pcb *u;
	if (!u) u = udp_new();
	ip_addr_t a;
	IP4_ADDR(ip_2_ip4(&a), 10, 0, 0, last);
	struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 4, PBUF_RAM);
	memcpy(p->payload, "ping", 4);
	udp_sendto(u, p, &a, 9);
	pbuf_free(p);
}

/* --- the clock -------------------------------------------------------------- */

static u32_t wake_timer, wake_all;

static int count(struct tcp_pcb *p)
{
	int n = 0;
	for (; p; p = p->next) n++;
	return n;
}

/* from one event to the next until end; step: the scenario's work at
 * the times it asks for (*at) */
static void run_until(u32_t end)
{
	static int shown_a = -1, shown_tw = -1;
	static u32_t shown_ip;
	while ((s32_t)(end - now) > 0) {
		u32_t next = end, by_timer = 0;
		u32_t sl = sys_timeouts_sleeptime();
		if (sl != SYS_TIMEOUTS_SLEEPTIME_INFINITE && (s32_t)(now + sl - next) <= 0) next = now + sl, by_timer = 1;
		u32_t qa = q_next();
		if (qa != INF && (s32_t)(qa - next) < 0) next = qa, by_timer = 0;
		if ((s32_t)(next - now) <= 0) next = now + 1;
		now = next;
		if (now == end && !by_timer && qa != now) break;
		wake_all++;
		if (by_timer) wake_timer++;
		net_enter();
		q_deliver();
		sys_check_timeouts();
		int a = count(tcp_active_pcbs), tw = count(tcp_tw_pcbs);
		if (a != shown_a || tw != shown_tw) {
			printf("E %u pcbs active %d time-wait %d\n", now, a, tw);
			shown_a = a, shown_tw = tw;
		}
		u32_t ipa = ip4_addr_get_u32(netif_ip4_addr(&nif));
		if (ipa != shown_ip) {
			printf("E %u address %u.%u.%u.%u\n", now, ipa & 0xff, ipa >> 8 & 0xff, ipa >> 16 & 0xff, ipa >> 24);
			shown_ip = ipa;
		}
	}
	now = end;
	net_enter();
}

/* a phase: its wake-ups */
static u32_t ph_timer, ph_all;
static void phase_start(void) { ph_timer = wake_timer, ph_all = wake_all; }
static void phase_end(const char *name) { printf("W %u %s %u %u\n", now, name, wake_timer - ph_timer, wake_all - ph_all); }

/* --- the scenarios ------------------------------------------------------- */

static void up(int dhcp)
{
	ip4_addr_t a, m, g;
	IP4_ADDR(&a, 10, 0, 0, 2), IP4_ADDR(&m, 255, 255, 255, 0), IP4_ADDR(&g, 10, 0, 0, 1);
	if (dhcp) {
		netif_add(&nif, NULL, NULL, NULL, NULL, if_init, ethernet_input);
	} else {
		netif_add(&nif, &a, &m, &g, NULL, if_init, ethernet_input);
	}
	netif_set_default(&nif);
	netif_set_up(&nif);
	if (dhcp) dhcp_start(&nif);
}

int main(int argc, char **argv)
{
	scen = argc > 1 ? argv[1] : "tcp-idle";
	srand(7);
	now = 100000;
	lwip_init();
	if (IS("dhcp") || IS("dhcp-expire")) {
		up(1);
		phase_start();
		if (IS("dhcp-expire")) {
			run_until(now + 10000);
			dhcp_silent = 1;
		}
		run_until(100000 + 40 * 60000 + 30000);
		phase_end("dhcp");
	} else if (IS("arp")) {
		up(0);
		phase_start();
		/* the router used every 100 s, an address that does not answer at 400 s */
		for (int i = 0; i < 15; i++) {
			udp_to(1);
			if (i == 4) udp_to(9);
			run_until(now + 100000);
		}
		phase_end("arp");
	} else {
		up(0);
		listen80();
		if (IS("tcp-syn")) {
			connect_to(81);
			drop_syn81 = 1;
			phase_start();
			run_until(now + 60000);
			phase_end("syn");
			return 0;
		}
		connect_to(80);
		run_until(now + 1000);
		if (!cli || !srv) {
			printf("E %u not connected\n", now);
			return 1;
		}
		if (IS("tcp-bulk")) {
			drop_every = 23, burst_at = 60, burst_n = 6;
			to_send = 300000;
			cli_push();
			run_until(now + 60000);
			printf("E %u sent %u got %u\n", now, sent, srv_got);
			tcp_close(cli);
			tcp_close(srv);
			cli = srv = NULL;
			phase_start();
			run_until(now + 200000);
			phase_end("closed");
		} else if (IS("tcp-delack")) {
			/* 100 bytes every 2 s, the server only reads: its ACKs delayed */
			for (int i = 0; i < 10; i++) {
				to_send += 100;
				cli_push();
				run_until(now + 2000);
			}
			printf("E %u got %u\n", now, srv_got);
		} else if (IS("tcp-zerowin")) {
			srv_reading = 0;
			to_send = 200000;
			cli_push();
			run_until(now + 30000);
			printf("E %u window closed: got %u held %u\n", now, srv_got, srv_held);
			srv_reading = 1;
			if (srv) tcp_recved(srv, (u16_t)LWIP_MIN(srv_held, 0xffffu));
			srv_held = 0;
			run_until(now + 30000);
			printf("E %u got %u sum %u\n", now, srv_got, srv_sum);
		} else if (IS("tcp-keepalive")) {
			ip_set_option(cli, SOF_KEEPALIVE);
			cli->keep_idle = 20000, cli->keep_intvl = 5000, cli->keep_cnt = 3;
			phase_start();
			run_until(now + 120000);
			phase_end("answered");
			cut = 1;
			printf("E %u cut\n", now);
			phase_start();
			run_until(now + 120000);
			phase_end("cut");
		} else if (IS("tcp-idle")) {
			phase_start();
			run_until(now + 30 * 60000);
			phase_end("idle");
			to_send += 5000;
			cli_push();
			run_until(now + 10000);
			printf("E %u got %u\n", now, srv_got);
		} else if (IS("tcp-finwait2")) {
			tcp_close(cli);
			cli = NULL;
			phase_start();
			run_until(now + 60000);
			phase_end("finwait2");
		} else if (IS("tcp-timewait")) {
			to_send = 1000;
			cli_push();
			run_until(now + 1000);
			tcp_close(srv);
			srv = NULL;
			run_until(now + 1000);
			tcp_close(cli);
			cli = NULL;
			phase_start();
			run_until(now + 200000);
			phase_end("timewait");
		}
	}
	printf("E %u done, %u wake-ups (%u by a timer)\n", now, wake_all, wake_timer);
#if LWIP_TIMERS_CUSTOM
	printf("E %u timers: tcp %u/%u dhcp %u/%u arp %u reass %u dns %u\n", now, net_timer_stats.tcp_fast,
	       net_timer_stats.tcp_slow, net_timer_stats.dhcp_fine, net_timer_stats.dhcp_coarse, net_timer_stats.arp,
	       net_timer_stats.reass, net_timer_stats.dns);
#endif
	return 0;
}
