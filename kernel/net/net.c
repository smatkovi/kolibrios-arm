/* KolibriOS for ARM: the network: lwIP (NO_SYS, the raw API) with one
 * Ethernet interface, configured by DHCP. With kolibri.nettest on the
 * kernel command line a self test on the serial console: the lease, a DNS
 * lookup, an HTTP request to the gateway (QEMU's host: make run-net
 * serves it). */
#include "net.h"
#include "rng.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

static struct netif nif;
static struct net_dev ndev;
static bool started;
static u8 txbuf[1600];

u32 sys_now(void)
{
	return ticks * 10;
}

/* lwIP's random numbers (DHCP transaction ids, TCP sequence numbers,
 * DNS ids): the counter mixed */
u32 net_rand(void)
{
	static u64 s;
	static bool seeded;
	if (!seeded) {
		rng_hw(&s, sizeof(s));
		seeded = true;
	}
	s += SYSREG_R(cntvct_el0) | 1;
	u64 z = s * 0x9e3779b97f4a7c15ull;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return (u32)(z ^ (z >> 31));
}

static void print_ip(const char *what, const ip4_addr_t *a)
{
	u32 v = ip4_addr_get_u32(a);
	kprintf("%s%u.%u.%u.%u", what, v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24);
}

static err_t link_out(struct netif *n, struct pbuf *p)
{
	(void)n;
	if (p->tot_len > sizeof(txbuf)) return ERR_BUF;
	pbuf_copy_partial(p, txbuf, p->tot_len, 0);
	return ndev.send(ndev.dev, txbuf, p->tot_len) ? ERR_OK : ERR_MEM;
}

static err_t if_init(struct netif *n)
{
	n->name[0] = 'e';
	n->name[1] = 'n';
	n->hwaddr_len = 6;
	memcpy(n->hwaddr, ndev.mac, 6);
	n->mtu = ndev.mtu;
	n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
	n->output = etharp_output;
	n->linkoutput = link_out;
	return ERR_OK;
}

static void nettest_start(void);

static void status_changed(struct netif *n)
{
	if (!netif_is_up(n) || ip4_addr_isany_val(*netif_ip4_addr(n))) return;
	print_ip("net: ", netif_ip4_addr(n));
	print_ip(", mask ", netif_ip4_netmask(n));
	print_ip(", gateway ", netif_ip4_gw(n));
	print_ip(", DNS ", ip_2_ip4(dns_getserver(0)));
	kprintf("\n");
	if (boot_arg("kolibri.nettest")) nettest_start();
}

void net_init(void)
{
	lwip_init();
}

struct netif *net_attach(const struct net_dev *d)
{
	if (started) return NULL;               /* one interface for now */
	ndev = *d;
	if (!netif_add(&nif, NULL, NULL, NULL, NULL, if_init, ethernet_input)) return NULL;
	netif_set_hostname(&nif, "kolibrios");
	netif_set_default(&nif);
	netif_set_status_callback(&nif, status_changed);
	netif_set_up(&nif);
	dhcp_start(&nif);
	started = true;
	kprintf("net: %s, MAC %02x:%02x:%02x:%02x:%02x:%02x, DHCP\n", d->name, d->mac[0], d->mac[1],
		d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
	return &nif;
}

void net_input(struct netif *n, const void *frame, u32 len)
{
	struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
	if (!p) return;
	pbuf_take(p, frame, len);
	if (n->input(p, n) != ERR_OK) pbuf_free(p);
}

void net_tick(void)
{
	if (!started) return;
	if (ndev.poll) ndev.poll(ndev.dev);
	sys_check_timeouts();
}

/* tickless idle: the ticks until lwIP's next timeout (its cyclic timers
 * included); frames come with the device's interrupt */
u32 net_next(void)
{
	if (!started) return 0xffffffffu;
	u32 ms = sys_timeouts_sleeptime();
	return ms == SYS_TIMEOUTS_SLEEPTIME_INFINITE ? 0xffffffffu : (ms + 9) / 10;
}

u32 net_addr(void)
{
	return started ? ip4_addr_get_u32(netif_ip4_addr(&nif)) : 0;
}

u32 net_dns(int i)
{
	const ip_addr_t *a = dns_getserver(i);
	return a ? ip4_addr_get_u32(ip_2_ip4(a)) : 0;
}

/* the self test: kolibri.nettest or kolibri.nettest=<the host's port> */
#define TEST_PORT 8765
static bool tested;
static struct tcp_pcb *tpcb;
static char reply[128];
static u32 nreply;

static err_t t_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	(void)arg;
	if (!p) {
		reply[nreply] = 0;
		for (u32 i = 0; i < nreply; i++)
			if (reply[i] == '\r' || reply[i] == '\n') { reply[i] = 0; break; }
		kprintf("nettest: the host answered \"%s\"\nnettest: done\n", reply);
		tcp_close(pcb);
		tpcb = NULL;
		return ERR_OK;
	}
	u32 n = MIN((u32)p->tot_len, sizeof(reply) - 1 - nreply);
	pbuf_copy_partial(p, reply + nreply, n, 0);
	nreply += n;
	tcp_recved(pcb, p->tot_len);
	pbuf_free(p);
	return err;
}

static err_t t_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
	(void)arg;
	static const char req[] = "GET / HTTP/1.0\r\nHost: kolibrios\r\n\r\n";
	kprintf("nettest: connected\n");
	tcp_write(pcb, req, sizeof(req) - 1, 0);
	tcp_output(pcb);
	return err;
}

static void t_err(void *arg, err_t err)
{
	(void)arg;
	kprintf("nettest: TCP error %d\n", err);
	tpcb = NULL;
}

static void t_dns(const char *name, const ip_addr_t *a, void *arg)
{
	(void)arg;
	if (a) {
		kprintf("nettest: %s is ", name);
		print_ip("", ip_2_ip4(a));
		kprintf("\n");
	} else {
		kprintf("nettest: %s not found\n", name);
	}
	ip_addr_t host;
	ip_addr_copy(host, *netif_ip_gw4(&nif));
	const char *v = boot_arg("kolibri.nettest");
	u16 port = TEST_PORT;
	if (v && *v >= '1' && *v <= '9')
		for (port = 0; *v >= '0' && *v <= '9'; v++) port = port * 10 + *v - '0';
	tpcb = tcp_new();
	tcp_err(tpcb, t_err);
	tcp_recv(tpcb, t_recv);
	print_ip("nettest: TCP to ", ip_2_ip4(&host));
	kprintf(":%d\n", port);
	tcp_connect(tpcb, &host, port, t_connected);
}

static void nettest_start(void)
{
	if (tested) return;
	tested = true;
	const char *name = "example.com";
	ip_addr_t a;
	err_t e = dns_gethostbyname(name, &a, t_dns, NULL);
	if (e == ERR_OK) t_dns(name, &a, NULL);
	else if (e != ERR_INPROGRESS) kprintf("nettest: DNS error %d\n", e);
}
