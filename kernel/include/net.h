/* KolibriOS for ARM: the network (kernel/net, AArch64 kernel): lwIP and
 * the network devices. lwIP runs in the kernel's context only. */
#ifndef NET_H
#define NET_H

#include "kernel.h"

struct netif;

void net_init(void);            /* lwIP, before the devices */
void net_tick(void);            /* the timer tick: the devices, lwIP's timeouts */

/* a device: its MAC address, its MTU, how it sends a frame (false: no
 * room now) and how net_tick polls it (or NULL) */
struct net_dev {
	const char *name;
	u8 mac[6];
	u16 mtu;
	bool (*send)(void *dev, const void *frame, u32 len);
	void (*poll)(void *dev);
	void *dev;
};
struct netif *net_attach(const struct net_dev *d);
void net_link(struct netif *n, bool up);
/* polled from the tick before and besides the interface; next_ticks for
 * the tickless idle (0xffffffff: nothing) */
void net_add_poller(void (*poll)(void), u32 (*next_ticks)(void));
void net_input(struct netif *n, const void *frame, u32 len);

/* the interface's IPv4 configuration (network byte order), 0 if none;
 * lwIP's random numbers */
u32 net_rand(void);             /* not for keys */
u32 net_addr(void);
u32 net_dns(int i);

/* kernel/main.c: a kernel command line argument: its value ("" for a
 * flag without one), NULL if not given */
const char *boot_arg(const char *name);

#endif
