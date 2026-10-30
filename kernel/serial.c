/* KolibriOS for ARM: serial ports for programs, as KolibriOS's serial
 * driver offers them (drivers/serial: the "SERIAL" service and its
 * ioctls, common.inc): function 68.16 "SERIAL" gives the service's
 * handle, 68.17 its requests (version, open, close, set up, read, write,
 * the list of ports). terminal (apps/terminal) uses it.
 *
 * The ports, polled (no interrupts):
 *   0      the kernel's console UART (its debug output goes there too):
 *          PL011 UART0 on realview, the PL011 of QEMU's virt; on the
 *          N950 (UART3) and the Pro1X (no UART, a RAM console) only
 *          sending, through the platform's putc
 *   1-3    realview's PL011 UART1-UART3 (QEMU: the second to fourth
 *          -serial)
 * Speed, word size, parity and stop bits are checked, not set: QEMU's
 * UARTs don't need them, and the console UART keeps what the boot loader
 * set. */
#include "gui.h"

/* a program's buffers need not be aligned: words stored and read so the
 * compiler cannot merge them into strd/ldrd (which fault on ARMv7) */
static inline void put32(void *p, u32 v) { struct __attribute__((packed)) w { u32 v; } *q = p; q->v = v; }
static inline u32 get32(const void *p) { const struct __attribute__((packed)) w { u32 v; } *q = p; return q->v; }

#define SERIAL_HANDLE 0x53524c01u
#define SERIAL_API_VERSION 0x00010000u        /* SERIAL_COMPATIBLE_API_VER 1 */

enum { GET_VERSION = 0, OPEN_PORT = 4, CLOSE_PORT = 5, SETUP_PORT = 6, READ = 7, WRITE = 8, ENUM_PORTS = 9 };
enum { ERR_PORT_INVALID = 1, ERR_PORT_BUSY = 2, ERR_CONF = 3 };

#define NPORTS 4
static struct port {
	addr_t pl011;          /* the UART's registers, or 0 */
	bool console, busy;
	int owner_slot;        /* the program that opened it */
	u32 owner_pid;
	const char *driver, *descr;
} ports[NPORTS];
static int nports;

static void probe(void)
{
	if (nports) return;
	struct port *p = &ports[nports++];
	p->console = true;
	p->driver = "console";
	p->descr = "the kernel's console";
#ifdef __aarch64__
	if (plat == &platform_virt) {
		p->pl011 = mmu_io_va(0x09000000);
		p->driver = "pl011";
		p->descr = "the console UART (PL011)";
	}
#else
	if (plat == &platform_realview) {
		static const char *const names[NPORTS] = { "the console UART0 (PL011)", "UART1 (PL011)",
							   "UART2 (PL011)", "UART3 (PL011)" };
		for (int i = 0; i < NPORTS; i++) {
			if (i) p = &ports[nports++];
			p->pl011 = mmu_io_va(0x10009000u + 0x1000u * i);
			p->driver = "pl011";
			p->descr = names[i];
		}
		ports[0].console = true;
	}
#endif
}

/* open, and by a program still running: one that ended without closing
 * it (terminal's console closed by its window's button) left it free */
static bool in_use(struct port *p)
{
	if (p->busy && (tasks[p->owner_slot].state == T_FREE || tasks[p->owner_slot].pid != p->owner_pid))
		p->busy = false;
	return p->busy;
}

static void tx(struct port *p, u8 c)
{
	if (p->pl011) {
		while (rd32(p->pl011 + 0x18) & (1 << 5))    /* TXFF */
			;
		wr32(p->pl011, c);
	} else if (plat->putc) {
		plat->putc(c);
	}
}

static int rx(struct port *p)
{
	if (!p->pl011 || (rd32(p->pl011 + 0x18) & (1 << 4)))   /* RXFE */
		return -1;
	return rd32(p->pl011) & 0xff;
}

/* a struct serial_conf: size, speed, word size, stop bits, parity, flow */
static bool conf_ok(struct task *t, u32 va)
{
	const u8 *c = uptr(t, va, 12);
	if (!c || get32(c) < 12) return false;
	u32 baud = get32(c + 4);
	u8 word = c[8], stop = c[9], parity = c[10], flow = c[11];
	return baud && word >= 5 && word <= 8 && stop <= 2 && parity <= 4 && flow == 0;
}

/* function 68.16: a driver by its name */
u32 serial_load_driver(struct task *t, u32 name_va)
{
	const char *n = ustr(t, name_va);
	return n && !strcmp(n, "SERIAL") ? SERIAL_HANDLE : 0;
}

/* function 68.17: { handler, io_code, input, inp_size, output, out_size } */
u32 serial_control(struct task *t, u32 ctl_va)
{
	const u8 *ctl = uptr(t, ctl_va, 24);
	if (!ctl || get32(ctl) != SERIAL_HANDLE) return -1;
	probe();
	u32 code = get32(ctl + 4), in = get32(ctl + 8), insz = get32(ctl + 12), out = get32(ctl + 16), outsz = get32(ctl + 20);
	const u8 *ab = insz ? uptr(t, in, insz) : NULL;
	u8 *o = outsz ? uptr(t, out, outsz) : NULL;
	/* the input's words */
	u32 a[3] = { 0, 0, 0 };
	for (u32 i = 0; ab && i < 3 && 4 * i + 4 <= insz; i++) a[i] = get32(ab + 4 * i);
	switch (code) {
	case GET_VERSION:
		if (!o || outsz < 4) return -1;
		put32(o, SERIAL_API_VERSION);
		return 0;
	case OPEN_PORT: {                      /* port id, conf -> handle */
		if (!ab || insz < 8 || !o || outsz < 4) return -1;
		if (a[0] >= (u32)nports) return ERR_PORT_INVALID;
		struct port *p = &ports[a[0]];
		if (in_use(p)) return ERR_PORT_BUSY;
		if (!conf_ok(t, a[1])) return ERR_CONF;
		p->busy = true;
		p->owner_slot = t->slot;
		p->owner_pid = t->pid;
		while (rx(p) >= 0)                 /* what came before */
			;
		put32(o, a[0] + 1);
		return 0;
	}
	case CLOSE_PORT:
	case SETUP_PORT:
	case READ:
	case WRITE: {
		if (!ab || insz < 4 || !a[0] || a[0] > (u32)nports || !in_use(&ports[a[0] - 1])) return ERR_PORT_INVALID;
		struct port *p = &ports[a[0] - 1];
		if (code == CLOSE_PORT) { p->busy = false; return 0; }
		if (code == SETUP_PORT) {
			if (insz < 8) return -1;
			bool ok = conf_ok(t, a[1]);
			if (o && outsz >= 4) put32(o, ok ? 0 : ERR_CONF);
			return ok ? 0 : ERR_CONF;
		}
		if (insz < 12 || !o || outsz < 4) return -1;
		u8 *buf = a[2] ? uptr(t, a[1], a[2]) : NULL;
		if (a[2] && !buf) return -1;
		u32 n = 0;
		if (code == READ) {
			for (int c; n < a[2] && (c = rx(p)) >= 0;) buf[n++] = c;
		} else {
			for (; n < a[2]; n++) tx(p, buf[n]);
		}
		put32(o, n);
		return 0;
	}
	case ENUM_PORTS: {                     /* buf, size -> total, filled */
		if (!ab || insz < 8 || !o || outsz < 8) return -1;
		u32 room = a[1] / 88, filled = 0;
		u8 *e = room ? uptr(t, a[0], room * 88) : NULL;
		for (int i = 0; i < nports && e && filled < room; i++, filled++, e += 88) {
			memset(e, 0, 88);
			put32(e, i);
			put32(e + 4, in_use(&ports[i]));
			strncpy0((char *)e + 8, ports[i].driver, 16);
			strncpy0((char *)e + 24, ports[i].descr, 64);
		}
		put32(o, nports);
		put32(o + 4, filled);
		return 0;
	}
	}
	return -1;
}
