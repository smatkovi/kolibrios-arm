/* KolibriOS for ARM: the Pro1X's Wi-Fi as a network device
 * (docs/pro1x-network.md, stages 4-6): the driver (kernel/modem/wifi.c)
 * on the WCN3990's register window and DMA memory, its hooks to WLFW, the
 * clock, the random numbers and lwIP. Started by the platform once WLFW
 * says the firmware is ready (kolibri.wifi, the modem's QRTR). The copy
 * engines are polled from the tick (every 10 ms while the station is not
 * up, then by wifi_poll_ms: 20 ms while frames come, 200 ms and 1 s when
 * idle), or by their interrupts with kolibri.wifi=irq (then no SoC sleep
 * while frames come, a look once a second when idle). */
#include "net.h"
#include "rng.h"
#include "power.h"
#include "../modem/wifi.h"
#include "../modem/wlfw.h"

static addr_t membase;                  /* the register window's virtual address */
static u8 *dma;
static u64 dma_pa;
static u32 dma_used, dma_size;
static struct netif *nif;
static bool started, irqs;
static u32 polls, irq_count;

static u32 rd(u32 off) { return *(volatile u32 *)(membase + off); }
static void wr(u32 off, u32 v) { *(volatile u32 *)(membase + off) = v; }

/* one uncached block below 4 GiB for all of it: the rings, the buffers,
 * the firmware's host memory (INIT: below 4 GiB without
 * WMI_SERVICE_SUPPORT_EXTEND_ADDRESS) */
static void *dalloc_aligned(u32 size, u64 *pa, u32 align)
{
	u32 at = (dma_used + align - 1) & ~(align - 1);
	if (at + size > dma_size) return NULL;
	dma_used = at + size;
	*pa = dma_pa + at;
	for (u32 i = 0; i < size; i++) dma[at + i] = 0;
	return dma + at;
}

static void *dalloc(u32 size, u64 *pa) { return dalloc_aligned(size, pa, 64); }
static void *dalloc32(u32 size, u64 *pa) { return dalloc_aligned(size, pa, 4096); }

static void enable(void) { wlfw_wlan_enable(wifi_wlan_enabled); }
static u32 now_ms(void) { return ticks * 10; }

/* the SNonce, SAE's scalars and masks, the random MAC address: the
 * kernel's generator (rng.c), seeded before the Wi-Fi starts
 * (wifi_dev_start) */
static void rnd(void *p, u32 n)
{
	if (!rng_bytes(p, n)) panic("wifi: random numbers without a seed");
}

static void rx(const u8 *eth, u32 len)
{
	if (nif) net_input(nif, eth, len);
}

static bool dev_send(void *d, const void *f, u32 len)
{
	(void)d;
	return wifi_send(f, len) == 0;
}

static void link(bool up, const u8 mac[6])
{
	if (up && !nif) {
		struct net_dev d = { .name = "wcn3990", .mtu = 1500, .send = dev_send, .poll = NULL };
		memcpy(d.mac, mac, 6);
		nif = net_attach(&d);
		return;
	}
	net_link(nif, up);
}

static void poll(void)
{
	if (!started) return;
	polls++;
	wifi_poll();
}

static u32 next_ticks(void)
{
	if (!started) return 0xffffffffu;
	enum wifi_state s = wifi_state();
	if (s == WIFI_FAILED) return 0xffffffffu;
	u32 t = wifi_next_ms();
	t = t == 0xffffffffu ? t : (t + 9) / 10;
	/* without the engines' interrupts: polled, as often as wifi_poll_ms
	 * says (slower when no frames come: the firmware's power save keeps
	 * the radio asleep, the AP keeps the frames); with them a look once a
	 * second when idle, for what came while the SoC slept (its GIC off),
	 * and no SoC sleep while frames come */
	u32 p = (wifi_poll_ms() + 9) / 10;
	if (!irqs) t = MIN(t, p);
	else if (s == WIFI_UP && !wifi_busy()) t = MIN(t, 100u);
	if (irqs && wifi_busy()) power_hold_awake(ticks + 100);
	return t;
}

void wifi_dev_irq(void)
{
	irq_count++;
	if (started) wifi_poll();
}

bool wifi_dev_start(addr_t membase_va, u32 dma_mb, bool use_irqs, const char *conf_text, u32 conf_len)
{
	static struct wifi_conf conf;
	const char *why;
	if (started) return true;
	/* the handshakes' nonces and SAE's scalars: only from a seeded generator */
	u8 probe;
	if (!rng_bytes(&probe, 1)) {
		kprintf("wifi: the random number generator has no seed: no Wi-Fi\n");
		return false;
	}
	if (rng_weak()) kprintf("wifi: WARNING: the random numbers are seeded from timing jitter only\n");
	if (wifi_conf_parse(conf_text, conf_len, &conf, &why)) {
		kprintf("wifi: the network's configuration: %s\n", why);
		return false;
	}
	addr_t pa = mm_alloc_mb_below(dma_mb, 2, 1ull << 32);
	if (!pa) {
		kprintf("wifi: no %u MiB below 4 GiB for DMA\n", dma_mb);
		return false;
	}
	/* uncached; the caches' lines of it written back and dropped first */
	for (addr_t a = pa; a < pa + (addr_t)dma_mb * (1u << 20); a += 64)
		asm volatile("dc civac, %0" :: "r"(a) : "memory");
	asm volatile("dsb sy");
	mmu_set_uncached(pa, dma_mb << 20);
	dma = (u8 *)pa;
	dma_pa = pa;
	dma_size = dma_mb << 20;
	membase = membase_va;
	irqs = use_irqs;
	struct wifi_hw hw = { { rd, wr, dalloc }, dalloc32, enable, now_ms, rnd, rx, link };
	kprintf("wifi: DMA %p (%u MiB), engines by %s, network \"%s\"\n", pa, dma_mb, irqs ? "interrupt" : "polling",
		(const char *)conf.ssid);
	net_add_poller(poll, next_ticks);
	started = true;
	wifi_start(&hw, &conf);
	return true;
}

/* the console's summary (the lifetime's end, the power test) */
void wifi_dev_report(void)
{
	if (!started) return;
	const struct wifi_stats *s = wifi_stats();
	kprintf("wifi: %s; %u WMI commands, %u events, %u HTT messages, %u scans, %u BSSs seen, %u frames out (%u done), "
		"%u in, %u dropped, %u EAPOL, %u restarts, %u polls, %u interrupts\n", wifi_state_name(wifi_state()),
		s->wmi_cmds, s->wmi_events, s->htt_msgs, s->scans, s->bss_seen, s->tx_frames, s->tx_done, s->rx_frames,
		s->rx_dropped, s->eapol, s->restarts, polls, irq_count);
}

/* the network's configuration in the initrd (pro1x-kexec.sh -w): the
 * image "wifi" of the KOSEFS01 bundle (tools/mkefs.py) */
const char *wifi_dev_conf(const u8 *b, u32 size, u32 *len)
{
	if (!b || size < 12 || memcmp(b, "KOSEFS01", 8)) return NULL;
	u32 n = *(const u32 *)(b + 8);
	for (u32 i = 0; i < n && 12 + (i + 1) * 24 <= size; i++) {
		const u8 *e = b + 12 + i * 24;
		u32 off = *(const u32 *)(e + 16), sz = *(const u32 *)(e + 20);
		if (!strncmp((const char *)e, "wifi", 16) && (u64)off + sz <= size) {
			*len = sz;
			return (const char *)b + off;
		}
	}
	return NULL;
}
