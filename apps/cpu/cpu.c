/* cpu: KolibriOS's process manager: the threads with their slot, PID,
 * memory and state; one ended with a tap on its button */
#include "kos.h"

#define ROW 20
#define BTN_KILL 100

static char info[1024];
static int slots[64], n;
static u32 free_kb, total_kb;

static void scan(void)
{
	n = 0;
	int max = kos_thread_info(info, -1);
	for (int s = 1; s <= max && n < 64; s++) {
		kos_thread_info(info, s);
		if (*(u16 *)(info + 50) != 9) slots[n++] = s;
	}
	free_kb = kos_free_ram_kb();
	total_kb = kos_total_ram_kb();
}

static void draw(void)
{
	char buf[96], name[12];
	int h = 60 + 16 * ROW + 30;   /* the size is fixed by the first call */
	kos_begin_draw();
	kos_window(200, 50, 420, h, KOS_WIN_SKINNED | 0xf4f4f4, "Processes");
	kos_text(8, 6, "slot  name         PID    memory  state", 0x404040, 0);
	for (int i = 0; i < n; i++) {
		kos_thread_info(info, slots[i]);
		memcpy(name, info + 10, 11);
		name[11] = 0;
		u16 st = *(u16 *)(info + 50);
		const char *state = st == 5 ? "waits" : st == 1 ? "sleeps" : "runs";
		ksprintf(buf, "%4d  %s", slots[i], name);
		kos_text(8, 24 + i * ROW, buf, 0x101820, 0);
		ksprintf(buf, "%4u  %5u KiB  %s", *(u32 *)(info + 30), (*(u32 *)(info + 26) + 1) / 1024, state);
		kos_text(104, 24 + i * ROW, buf, 0x101820, 0);
		kos_button(320, 20 + i * ROW, 70, ROW - 4, BTN_KILL + slots[i], 0xd88080);
		kos_text(338, 24 + i * ROW, "end", 0xffffff, 0);
	}
	ksprintf(buf, "memory: %u of %u MiB free", free_kb / 1024, total_kb / 1024);
	kos_text(8, 30 + n * ROW, buf, 0x404040, 0);
	kos_end_draw();
}

int main(void)
{
	scan();
	draw();
	kos_event_mask(EVM_REDRAW | EVM_KEY | EVM_BUTTON | EVM_SYSTEM);
	for (;;) {
		/* a second's look only while the display is on (EV_SYSTEM
		 * when it goes off or on, or a window comes or goes) */
		int ev = kos_system_state() & KOS_SYS_DISPLAY_OFF ? kos_wait_event() : kos_wait_event_timeout(100);
		if (ev == KOS_EV_KEY) {
			kos_get_key();                 /* taken, else the event stays */
		} else if (ev == KOS_EV_BUTTON) {
			u32 id = kos_get_button() >> 8;
			if (id == 1) kos_exit();
			if (id >= BTN_KILL) { kos_kill_slot(id - BTN_KILL); scan(); draw(); }
		} else if (ev == KOS_EV_REDRAW || ev == 0 || ev == KOS_EV_SYSTEM) {
			int old = n;
			scan();
			if (ev == KOS_EV_REDRAW || old != n) draw();
		}
	}
}
