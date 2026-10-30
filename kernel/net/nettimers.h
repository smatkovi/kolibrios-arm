/* KolibriOS for ARM: lwIP's timers on demand (timers.c), the kernel's
 * side: also built on the PC (tests/net), lwIP's types only */
#ifndef NETTIMERS_H
#define NETTIMERS_H

#include "lwip/arch.h"

/* every way into the kernel, before anything of lwIP runs: what its
 * idle timers did not count (tcp_ticks, the ARP table's, the DNS cache's,
 * the reassembly's ages) brought up to the present */
void net_enter(void);
/* every frame in and out: ARP requests, IP fragments, DNS queries start
 * the time-outs that need the second's timer */
void net_frame(const void *frame, u32_t len, int out);
/* kolibri.nettimers=periodic: every timer on its grid always, as lwIP's
 * timeouts.c */
void net_timers_periodic(int yes);

/* how often each timer ran */
struct net_timer_stats {
	u32_t tcp_fast, tcp_slow, dhcp_fine, dhcp_coarse, arp, reass, dns;
};
extern struct net_timer_stats net_timer_stats;

#endif
