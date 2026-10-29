/* KolibriOS for ARM: the modem in the kernel: started by the platform
 * (the Pro1X's real one, kernel/platform/pro1x.c) or a fake one (QEMU),
 * its work from the timer tick, function 100 for the programs
 * (lib/kos_modem.h) */
#include "task.h"
#include "phone.h"
#include "fake.h"

static struct modem_hw hw;
static bool fake;

static u32 clock_now(void)
{
	u32 t = rtc_seconds();
	return t > 1600000000u ? t : 0;     /* before 2020: not set */
}

void kmodem_start(const struct modem_hw *h)
{
	hw = *h;
	hw.clock = clock_now;
	hw.set_clock = rtc_set;
	modem_start(&hw);
}

void kmodem_fake(void)
{
	if (modem_present()) return;
	u8 *smem = kmalloc_aligned(FAKE_SMEM_SIZE, 4096);
	u8 *rm = kmalloc_aligned(FAKE_RMTFS_SIZE, 4096);
	fake_init(&hw, smem, rm, clock_now, rtc_set);
	fake = true;
	kprintf("modem: a fake one in RAM (QEMU)\n");
	modem_start(&hw);
}

void kmodem_tick(void)
{
	if (!modem_present()) return;
	u32 ms = ticks * 10;
	if (fake) fake_poll(ms);
	modem_poll(ms);
}

#define EAX r[0]
#define EBX r[1]
#define ECX r[2]
#define EDX r[3]

void sys_modem(struct task *t, u32 *r)
{
	switch (EBX) {
	case KOS_MODEM_STATUS: {
		struct kos_modem *m = uptr(t, ECX, sizeof(*m));
		if (!m) { EAX = -1; return; }
		modem_status(m);
		EAX = 0;
		return;
	}
	case KOS_MODEM_SMS: {
		struct kos_sms *s = uptr(t, EDX, sizeof(*s));
		EAX = s ? modem_sms(ECX, s) : -1;
		return;
	}
	case KOS_MODEM_SEND: {
		const char *n = ustr(t, ECX), *s = ustr(t, EDX);
		EAX = n && s ? modem_send(n, s) : -1;
		return;
	}
	case KOS_MODEM_DIAL: {
		const char *n = ustr(t, ECX);
		EAX = n ? modem_dial(n) : -1;
		return;
	}
	case KOS_MODEM_ANSWER: EAX = modem_answer(); return;
	case KOS_MODEM_HANGUP: EAX = modem_hangup(); return;
	case KOS_MODEM_CALL: {
		struct kos_call *c = uptr(t, ECX, sizeof(*c));
		if (!c) { EAX = -1; return; }
		modem_call(c);
		EAX = 0;
		return;
	}
	case KOS_MODEM_DELETE: EAX = modem_delete(ECX); return;
	case KOS_MODEM_READ: EAX = modem_mark_read(ECX); return;
	default: EAX = -1; return;
	}
}
