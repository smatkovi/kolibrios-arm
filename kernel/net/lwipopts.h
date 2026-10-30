/* KolibriOS for ARM: lwIP's configuration. The raw API only (NO_SYS):
 * lwIP runs in the kernel's context, with interrupts masked, from the
 * network interrupt, the timer tick and system calls. IPv4 with DHCP and
 * DNS, TCP, UDP. */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
/* the timers on demand: kernel/net/timers.c instead of lwIP's cyclic
 * ones (its timeouts.c then only has tcp_timer_needed's empty stub) */
#ifndef LWIP_TIMERS_CUSTOM
#define LWIP_TIMERS_CUSTOM 1
#endif
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0

#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 0
#define LWIP_UDP 1
#define LWIP_TCP 1
#define LWIP_DHCP 1
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_DNS 1
#define DNS_MAX_SERVERS 2
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_TCP_KEEPALIVE 1
#define SO_REUSE 1
#define LWIP_STATS 0
#define LWIP_CHKSUM_ALGORITHM 3

/* memory: lwIP's own heap and pools, static */
#define MEM_ALIGNMENT 8
#define MEM_SIZE (2 * 1024 * 1024)
#define MEMP_NUM_PBUF 128
#define MEMP_NUM_UDP_PCB 32
#define MEMP_NUM_TCP_PCB 64
#define MEMP_NUM_TCP_PCB_LISTEN 16
#define MEMP_NUM_TCP_SEG 512
#define MEMP_NUM_SYS_TIMEOUT 16
#define PBUF_POOL_SIZE 512
#define PBUF_POOL_BUFSIZE 1600

#define TCP_MSS 1460
#define LWIP_WND_SCALE 1
#define TCP_RCV_SCALE 2
#define TCP_WND (64 * 1024)
#define TCP_SND_BUF (64 * 1024)
#define TCP_SND_QUEUELEN (4 * TCP_SND_BUF / TCP_MSS)
#define TCP_LISTEN_BACKLOG 1

#endif
