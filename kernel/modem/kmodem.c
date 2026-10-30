/* KolibriOS for ARM: the modem in the kernel: started by the platform
 * (the Pro1X's real one, kernel/platform/pro1x.c) or a fake one (QEMU),
 * its work from the timer tick, function 100 for the programs
 * (lib/kos_modem.h) */
#include "task.h"
#include "phone.h"
#include "fake.h"
#include "callaudio.h"

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

/* QEMU: amplifiers that answer, no ADSP, so that the programs' speaker
 * switch works */
static bool fake_i2c_write(u8 addr, u8 reg, u8 val) { return true; }
static bool fake_i2c_read(u8 addr, u8 reg, u8 *val) { *val = 0x39; return true; }
static void fake_gpio(int pin, bool high) {}
static void fake_delay(u32 us) {}

static bool fake_wlfw;

void kmodem_fake_wlfw(void)
{
	fake_wlfw = true;
}

void kmodem_fake(void)
{
	if (modem_present()) return;
	u8 *smem = kmalloc_aligned(FAKE_SMEM_SIZE, 4096);
	u8 *rm = kmalloc_aligned(FAKE_RMTFS_SIZE, 4096);
	fake_init(&hw, smem, rm, clock_now, rtc_set);
	hw.wlfw = fake_wlfw;                    /* its WLFW service asked (the Wi-Fi in QEMU) */
	fake = true;
	kprintf("modem: a fake one in RAM (QEMU)\n");
	modem_start(&hw);
	struct callaudio_hw ah = {
		.i2c_write = fake_i2c_write, .i2c_read = fake_i2c_read, .gpio_set = fake_gpio,
		.delay_us = fake_delay, .prepared = true,
	};
	callaudio_init(&ah);
}

void kcallaudio_start(const struct callaudio_hw *h)
{
	callaudio_init(h);
}

/* the call, for its sound: being set up (dialling, ringing either way),
 * talking (or held), or none */
static int call_now(u32 *id)
{
	struct kos_call c;
	modem_call(&c);
	*id = c.id;
	switch (c.state) {
	case KOS_CALL_DIALING: case KOS_CALL_ALERTING: case KOS_CALL_INCOMING: return CALLAUDIO_CALL_SETUP;
	case KOS_CALL_ACTIVE: case KOS_CALL_HELD: return CALLAUDIO_CALL_ACTIVE;
	default: return CALLAUDIO_CALL_NONE;
	}
}

void kmodem_tick(void)
{
	if (!modem_present()) return;
	u32 ms = ticks * 10;
	if (fake) fake_poll(ms);
	modem_poll(ms);
	u32 id;
	int call = call_now(&id);
	callaudio_poll(ms, call, id);
}

/* the modem's interrupt (its GLINK edge): from now on what it sends
 * wakes the kernel, its idle poll can wait for its own timeouts */
static bool irq_wired;

void kmodem_irq(void)
{
	irq_wired = true;
	kmodem_tick();
}

/* tickless idle: the next tick kmodem_tick needs. Without the modem's
 * interrupt, what it sends is seen by a poll at least every second (a
 * call rings, rmtfs answers within it); the fake modem answers when
 * polled, as the real one's messages would interrupt */
#define POLL_IDLE_MS 1000

u32 kmodem_next(void)
{
	if (!modem_present()) return 0xffffffffu;
	u32 ms = ticks * 10, id;
	if (call_now(&id) != CALLAUDIO_CALL_NONE) return 0;
	u32 n = modem_next_ms(ms);
	if (fake) n = MIN(n, fake_next_ms(ms));
	else if (!irq_wired) n = MIN(n, (u32)POLL_IDLE_MS);
	return n == 0xffffffffu ? n : (n + 9) / 10;
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
		m->audio = callaudio_state() == CALLAUDIO_PREPARED ? KOS_AUDIO_PREPARED : KOS_AUDIO_NONE;
		m->audio_route = callaudio_route() == CALLAUDIO_SPEAKER ? KOS_ROUTE_SPEAKER : KOS_ROUTE_EARPIECE;
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
	case KOS_MODEM_ROUTE:
		EAX = callaudio_set_route(ECX == KOS_ROUTE_SPEAKER ? CALLAUDIO_SPEAKER : CALLAUDIO_EARPIECE);
		return;
	default: EAX = -1; return;
	}
}
