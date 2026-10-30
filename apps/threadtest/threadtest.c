/* KolibriOS for ARM: a test of threads (function 51), IPC (60), named
 * shared memory (68.22, 68.23) and libraries (68.19) from a program.
 * Each check writes "threadtest: ok ..." or "threadtest: FAIL ..." to the
 * debug board, "threadtest: done N failed" at the end (tests/threads
 * runs it in QEMU on both kernels). It starts itself again as a second
 * process for the checks between processes:
 *
 *   threadtest                 all of it
 *   threadtest echo PID        (the second process) IPC to PID's echo, then shared memory
 *   threadtest orphan          (the second process) its main thread ends, a thread stays
 *   threadtest shellapi        started by the shell: its console through "PID-SHELL"
 *                              (ktcc's shell_api.h, the shell's module_program_console.c) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <kos.h>

static int failed;

static void check(int ok, const char *what, int a, int b)
{
	printf("threadtest: %s %s (%d %d)\n", ok ? "ok" : "FAIL", what, a, b);
	if (!ok) failed++;
}

static u8 info[1024];

/* function 9's state of a slot: 9 = free */
static int slot_state(int slot)
{
	kos_thread_info(info, slot);
	return *(u16 *)(info + 50);
}

static u32 pid_of(int slot)
{
	kos_thread_info(info, slot);
	return *(u32 *)(info + 30);
}

static u32 my_pid(void) { return pid_of(-1); }

/* until the thread with that TID is gone (or 3 s) */
static int wait_gone(u32 tid)
{
	for (int i = 0; i < 300; i++) {
		if (!kos_slot_of_pid(tid)) return 1;
		kos_delay(1);
	}
	return 0;
}

#define NSTACK 48
static u8 stacks[NSTACK][2048] __attribute__((aligned(16)));
#define STACK(i) (stacks[i] + sizeof(stacks[i]))

/* --- create, exit, kill ---------------------------------------------- */

static volatile u32 shared_word, thread_slot, thread_pid_seen;

static void t_simple(void)
{
	thread_slot = kos_thread_slot();
	thread_pid_seen = my_pid();
	shared_word = 0x1234;
	kos_exit();
}

static volatile u32 spin_count;
static void t_spin(void)
{
	for (;;) {
		spin_count++;
		kos_delay(1);
	}
}

static void test_create(void)
{
	shared_word = 0;
	int tid = kos_thread_create(t_simple, STACK(0));
	check(tid > 0 && (u32)tid != my_pid(), "51.1 gives a TID of its own", tid, my_pid());
	int slot = kos_slot_of_pid(tid);
	check(wait_gone(tid), "the thread ended with -1", tid, 0);
	check(shared_word == 0x1234, "its write is in the same address space", shared_word, 0x1234);
	check(thread_pid_seen == (u32)tid, "function 9 -1 in the thread: its own TID", thread_pid_seen, tid);
	check(thread_slot == (u32)slot && slot != kos_thread_slot(), "51.2: its own slot", thread_slot, slot);
	check(slot_state(slot) == 9, "its slot is free again", slot_state(slot), 9);

	/* the name and memory are the process's (function 9's +10, +26) */
	spin_count = 0;
	tid = kos_thread_create(t_spin, STACK(1));
	slot = kos_slot_of_pid(tid);
	char myname[12] = { 0 }, its[12] = { 0 };
	kos_thread_info(info, -1);
	memcpy(myname, info + 10, 11);
	u32 mymem = *(u32 *)(info + 26);
	kos_thread_info(info, slot);
	memcpy(its, info + 10, 11);
	check(!strcmp(myname, its) && *(u32 *)(info + 26) == mymem, "function 9: the thread's name and memory are the program's", mymem, *(u32 *)(info + 26));
	kos_delay(10);
	check(spin_count > 2, "the thread runs beside the main one", spin_count, 0);
	check(kos_kill_pid(tid) == 0 && wait_gone(tid), "18.18 ends the thread by its TID", tid, 0);
	u32 c = spin_count;
	kos_delay(5);
	check(spin_count == c, "and it does not run any more", spin_count, c);

	tid = kos_thread_create(t_spin, STACK(1));
	slot = kos_slot_of_pid(tid);
	kos_kill_slot(slot);
	check(wait_gone(tid), "18.2 ends the thread by its slot", slot, tid);
	check(slot_state(kos_thread_slot()) != 9, "the main thread still runs", kos_thread_slot(), 0);

	/* a thread's fault ends the thread only */
	tid = kos_thread_create((void (*)(void))0x30, STACK(1));    /* the header: not code */
	check(wait_gone(tid), "a thread that faults ends", tid, 0);
	check(slot_state(kos_thread_slot()) != 9, "and the program goes on", 0, 0);
}

/* --- many threads ---------------------------------------------------- */

static volatile u32 ran[NSTACK];
static volatile u32 counter;
static volatile u32 index_next;

static void t_count(void)
{
	u32 i = __atomic_fetch_add(&index_next, 1, __ATOMIC_SEQ_CST);
	for (int k = 0; k < 100; k++) {
		__atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);
		if (k % 25 == 0) kos_call(68, 1, 0, 0, 0, 0);           /* yield */
	}
	ran[i] = my_pid();
	kos_exit();
}

static void test_many(void)
{
	for (int round = 0; round < 2; round++) {
		int n = 0, tids[NSTACK];
		counter = 0;
		index_next = 0;
		memset((void *)ran, 0, sizeof(ran));
		/* each thread takes a stack by the index it draws: all of them
		 * from stack 2 on, one per thread */
		for (int i = 0; i < NSTACK - 4; i++) {
			int tid = kos_thread_create(t_count, STACK(i + 2));
			if (tid < 0) break;
			tids[n++] = tid;
		}
		int gone = 0;
		for (int i = 0; i < n; i++) gone += wait_gone(tids[i]);
		int distinct = 1;
		for (int i = 0; i < n; i++)
			for (int j = 0; j < i; j++)
				if (ran[i] == ran[j]) distinct = 0;
		char what[64];
		snprintf(what, sizeof(what), "%d threads at once (round %d), all ended", n, round + 1);
		check(n == NSTACK - 4 && gone == n, what, n, gone);
		check(counter == (u32)n * 100 && distinct, "each of them ran, with its own TID", counter, n * 100);
	}
}

/* --- the FP/SIMD registers per thread ---------------------------------- */

static volatile u32 fp_bad[2], fp_done[2];

/* d0-d31 with the thread's own values, then a switch (function 5), then
 * the values compared (the registers are lazily saved per slot) */
static void fp_run(u32 who)
{
	u32 bad = 0;
	for (int round = 0; round < 20; round++) {
		u32 v = (who + 1) * 0x01010101u + round, got;
		asm volatile(".fpu neon\n"
			     "vdup.32 q0, %0\n vdup.32 q1, %0\n vdup.32 q2, %0\n vdup.32 q3, %0\n"
			     "vdup.32 q8, %0\n vdup.32 q15, %0\n vmsr fpscr, %0" :: "r"(v));
		kos_delay(1);
		asm volatile(".fpu neon\n vmov.32 %0, d0[1]" : "=r"(got));
		if (got != v) bad++;
		asm volatile(".fpu neon\n vmov.32 %0, d31[0]" : "=r"(got));
		if (got != v) bad++;
		asm volatile(".fpu neon\n vmrs %0, fpscr" : "=r"(got));
		if ((got & 0xf7c0009f) != (v & 0xf7c0009f)) bad++;
		/* and an arithmetic one */
		u32 sum;
		asm volatile(".fpu neon\n vadd.i32 q4, q0, q1\n vmov.32 %0, d9[1]" : "=r"(sum));
		if (sum != 2 * v) bad++;
	}
	fp_bad[who] = bad;
	fp_done[who] = 1;
}

static void t_fp(void) { fp_run(1); kos_exit(); }

static void test_fp(void)
{
	fp_done[0] = fp_done[1] = 0;
	int tid = kos_thread_create(t_fp, STACK(1));
	fp_run(0);
	wait_gone(tid);
	check(fp_done[0] && fp_done[1] && !fp_bad[0] && !fp_bad[1], "the VFP/NEON registers kept per thread", fp_bad[0], fp_bad[1]);
}

/* --- a window and events per thread ------------------------------------ */

static volatile u32 win_events, win_tid_ready;

static void t_window(void)
{
	kos_event_mask(EVM_REDRAW | EVM_BUTTON | EVM_IPC);
	for (;;) {
		int ev = kos_wait_event();
		if (ev == KOS_EV_REDRAW) {
			kos_begin_draw();
			kos_window(40, 40, 200, 100, KOS_WIN_SKINNED | 0xffffff, "a thread's window");
			kos_end_draw();
			win_events |= 1;
			win_tid_ready = 1;
		} else if (ev == KOS_EV_IPC) {
			win_events |= 2;
			kos_exit();
		}
	}
}

static void test_window(void)
{
	win_events = win_tid_ready = 0;
	int tid = kos_thread_create(t_window, STACK(1));
	for (int i = 0; i < 200 && !win_tid_ready; i++) kos_delay(1);
	int slot = kos_slot_of_pid(tid);
	kos_thread_info(info, slot);
	int w = *(u32 *)(info + 42), h = *(u32 *)(info + 46);
	check((win_events & 1) && w > 0 && h > 0, "a thread has a window of its own (its first event: redraw)", w, h);
	check(kos_ipc_send(tid, "x", 1) == 1, "60.2 to a thread without a buffer: 1", 0, 0);
	kos_kill_pid(tid);
	wait_gone(tid);
	check(slot_state(slot) == 9, "its window gone with it", 0, 0);
}

/* --- IPC ping-pong ----------------------------------------------------- */

static u8 main_ipc[256] __attribute__((aligned(4)));
static u8 thr_ipc[256] __attribute__((aligned(4)));
static volatile u32 main_tid;

/* the next message from buf (its first one) or 0; the buffer emptied */
static int ipc_take(u8 *buf, char *out, u32 *from)
{
	if (*(u32 *)(buf + 4) <= 8) return 0;
	*(u32 *)buf = 1;                        /* locked while it is read */
	u32 len = *(u32 *)(buf + 12);
	if (from) *from = *(u32 *)(buf + 8);
	memcpy(out, buf + 16, len);
	out[len] = 0;
	u32 used = *(u32 *)(buf + 4), next = 16 + len;   /* the second message */
	memmove(buf + 8, buf + next, used - next);
	*(u32 *)(buf + 4) = used - (next - 8);
	*(u32 *)buf = 0;
	return 1;
}

static void t_pong(void)
{
	*(u32 *)thr_ipc = 0;
	*(u32 *)(thr_ipc + 4) = 8;
	kos_ipc_set_area(thr_ipc, sizeof(thr_ipc));
	kos_event_mask(EVM_IPC);
	char m[64], r[64];
	for (;;) {
		u32 from;
		while (!ipc_take(thr_ipc, m, &from))
			if (kos_wait_event() != KOS_EV_IPC) continue;
		if (!strcmp(m, "quit")) kos_exit();
		snprintf(r, sizeof(r), "pong%s", m + 4);
		while (kos_ipc_send(from, r, strlen(r)) != 0) kos_delay(1);
	}
}

static int ipc_wait(u8 *buf, char *out, u32 *from, int ticks)
{
	for (int i = 0; i < ticks; i++) {
		if (ipc_take(buf, out, from)) return 1;
		kos_wait_event_timeout(1);
	}
	return ipc_take(buf, out, from);
}

static void test_ipc(void)
{
	*(u32 *)main_ipc = 0;
	*(u32 *)(main_ipc + 4) = 8;
	kos_ipc_set_area(main_ipc, sizeof(main_ipc));
	u32 old = kos_event_mask(EVM_IPC | EVM_REDRAW);
	int tid = kos_thread_create(t_pong, STACK(1));
	kos_delay(2);
	int good = 0;
	for (int i = 0; i < 200; i++) {
		char m[32], r[64], want[32];
		snprintf(m, sizeof(m), "ping%d", i);
		snprintf(want, sizeof(want), "pong%d", i);
		u32 from = 0;
		if (kos_ipc_send(tid, m, strlen(m))) break;
		if (!ipc_wait(main_ipc, r, &from, 300)) break;
		if (!strcmp(r, want) && from == (u32)tid) good++;
	}
	check(good == 200, "IPC ping-pong between threads, 200 rounds, the sender's TID", good, 200);
	/* the original's answers: no such PID 4, locked 2, full 3 */
	check(kos_ipc_send(0x7fffffff, "x", 1) == 4, "60.2 to no such PID: 4", 0, 0);
	*(u32 *)main_ipc = 1;
	check(kos_ipc_send(my_pid(), "x", 1) == 2, "60.2 to a locked buffer: 2", 0, 0);
	*(u32 *)main_ipc = 0;
	char big[300];
	memset(big, 'a', sizeof(big));
	check(kos_ipc_send(my_pid(), big, 241) == 3, "60.2 more than the buffer holds: 3", 0, 0);
	check(kos_ipc_send(my_pid(), big, 100) == 0 && kos_ipc_send(my_pid(), big, 100) == 0 &&
	      kos_ipc_send(my_pid(), big, 100) == 3 && *(u32 *)(main_ipc + 4) == 8 + 2 * 108,
	      "60.2 appends until the buffer is full", *(u32 *)(main_ipc + 4), 8 + 2 * 108);
	int ev = kos_check_event();
	check(ev == KOS_EV_IPC, "event 7 after a message", ev, 7);
	*(u32 *)(main_ipc + 4) = 8;
	kos_ipc_send(tid, "quit", 4);
	check(wait_gone(tid), "the echo thread ended", 0, 0);
	kos_event_mask(old);
}

/* --- between processes: IPC and shared memory -------------------------- */

#define SHM_NAME "threadtest-area"

static void test_processes(void)
{
	*(u32 *)main_ipc = 0;
	*(u32 *)(main_ipc + 4) = 8;
	kos_ipc_set_area(main_ipc, sizeof(main_ipc));
	kos_event_mask(EVM_IPC | EVM_REDRAW);
	u32 edx = 0;
	u8 *a = kos_shm_open(SHM_NAME, 10000, KOS_SHM_CREATE | KOS_SHM_WRITE, &edx);
	check(a && edx == 0, "68.22 creates an area (edx 0)", (int)a, edx);
	if (!a) return;
	for (int i = 0; i < 10000; i++) a[i] = i * 7;
	u32 e2 = 0;
	void *b = kos_shm_open(SHM_NAME, 10000, KOS_SHM_CREATE | KOS_SHM_WRITE, &e2);
	check(!b && e2 == 10, "68.22 SHM_CREATE of one that is there: E_ACCESS", (int)b, e2);
	b = kos_shm_open("threadtest-none", 0, KOS_SHM_OPEN, &e2);
	check(!b && e2 == 5, "68.22 SHM_OPEN of none: E_NOTFOUND", (int)b, e2);
	u8 *c = kos_shm_open(SHM_NAME, 0, KOS_SHM_OPEN | KOS_SHM_WRITE, &e2);
	check(c && c != a && e2 == 12288 && c[1] == 7 && c[9999] == (u8)(9999 * 7),
	      "68.22 again: a second mapping of the same memory, edx its size in pages", (int)c, e2);
	if (c) {
		c[5] = 0x55;
		check(a[5] == 0x55, "written through one, read through the other", a[5], 0x55);
		kos_shm_close(SHM_NAME);  /* the latest of the two (KolibriOS's order) */
	}

	char params[32];
	snprintf(params, sizeof(params), "echo %u", my_pid());
	int child = kos_run("/sys/threadtest", params);
	check(child > 0, "a second process started", child, 0);
	char m[64];
	u32 from = 0;
	int got = ipc_wait(main_ipc, m, &from, 500);
	check(got && !strcmp(m, "hello") && from == (u32)child, "IPC from the other process", got, from);
	kos_ipc_send(child, "ping", 4);
	got = ipc_wait(main_ipc, m, &from, 500);
	check(got && !strcmp(m, "pong"), "IPC to it and back", got, 0);
	/* it reads the area and writes into it */
	got = ipc_wait(main_ipc, m, &from, 500);
	check(got && !strcmp(m, "shm ok"), "the other process sees the area's contents", got, 0);
	check(a[0] == 0xaa && a[9999] == 0xbb, "and its writes arrive here", a[0], a[9999]);
	check(wait_gone(child), "the other process ended", child, 0);

	/* a read-only area: opening it for writing is refused */
	void *ro = kos_shm_open("threadtest-ro", 4096, KOS_SHM_CREATE | KOS_SHM_READ, &e2);
	b = kos_shm_open("threadtest-ro", 0, KOS_SHM_OPEN | KOS_SHM_WRITE, &e2);
	check(ro && !b && e2 == 10, "68.22 SHM_WRITE on a read-only area: E_ACCESS", (int)b, e2);
	kos_shm_close("threadtest-ro");
	kos_shm_close(SHM_NAME);
	b = kos_shm_open(SHM_NAME, 0, KOS_SHM_OPEN, &e2);
	check(!b && e2 == 5, "68.23 by all of them: the area is gone", (int)b, e2);

	/* a process whose main thread ended: its thread goes on, the
	 * address space with it, until that ends too */
	child = kos_run("/sys/threadtest", "orphan");
	got = ipc_wait(main_ipc, m, &from, 500);
	u32 ttid = got ? (u32)atoi(m) : 0;
	check(got && ttid && wait_gone(child), "a process's main thread ended", child, ttid);
	volatile u32 *cnt = kos_shm_open("threadtest-orphan", 0, KOS_SHM_OPEN, &e2);
	u32 c0 = cnt ? *cnt : 0;
	kos_delay(20);
	check(cnt && *cnt > c0 && kos_slot_of_pid(ttid), "its other thread still runs in its memory", cnt ? *cnt : 0, c0);
	kos_kill_pid(ttid);
	check(wait_gone(ttid), "the thread ended by 18.18: the process with it", ttid, 0);
	kos_shm_close("threadtest-orphan");
	b = kos_shm_open("threadtest-orphan", 0, KOS_SHM_OPEN, &e2);
	check(!b && e2 == 5, "its shared memory closed with it", (int)b, e2);
}

static void child_echo(u32 parent)
{
	*(u32 *)main_ipc = 0;
	*(u32 *)(main_ipc + 4) = 8;
	kos_ipc_set_area(main_ipc, sizeof(main_ipc));
	kos_event_mask(EVM_IPC);
	kos_ipc_send(parent, "hello", 5);
	char m[64];
	u32 from;
	if (ipc_wait(main_ipc, m, &from, 500) && !strcmp(m, "ping")) kos_ipc_send(from, "pong", 4);
	u32 edx = 0;
	u8 *a = kos_shm_open(SHM_NAME, 0, KOS_SHM_OPEN | KOS_SHM_WRITE, &edx);
	int ok = a && edx == 12288;
	for (int i = 0; ok && i < 10000; i++)
		if (a[i] != (u8)(i == 5 ? 0x55 : i * 7)) ok = 0;
	if (ok) {
		a[0] = 0xaa;
		a[9999] = 0xbb;
	}
	kos_ipc_send(parent, ok ? "shm ok" : "shm bad", ok ? 6 : 7);
	/* its mapping closed when it ends, without 68.23 */
}

static volatile u32 *orphan_cnt;
static void t_orphan(void)
{
	for (;;) {
		(*orphan_cnt)++;
		kos_delay(1);
	}
}

static void child_orphan(void)
{
	/* the area is the main thread's: it stays mapped for the other
	 * thread when the main one ends (the process's then) */
	u32 edx;
	orphan_cnt = kos_shm_open("threadtest-orphan", 4096, KOS_SHM_CREATE | KOS_SHM_WRITE, &edx);
	int tid = kos_thread_create(t_orphan, STACK(1));
	/* the parent: the other threadtest */
	u32 me = my_pid(), parent = 0;
	int max = kos_thread_info(info, -1);
	for (int s = 1; s <= max; s++) {
		if (slot_state(s) == 9) continue;
		kos_thread_info(info, s);
		if (!memcmp(info + 10, "threadtest", 10) && *(u32 *)(info + 30) != me && *(u32 *)(info + 30) != (u32)tid) {
			parent = *(u32 *)(info + 30);
			break;
		}
	}
	char m[16];
	snprintf(m, sizeof(m), "%d", tid);
	kos_ipc_send(parent, m, strlen(m));
	kos_delay(5);
}

/* --- libraries ----------------------------------------------------------- */

struct kexport { const char *name; void *value; };

static void *find_export(struct kexport *e, const char *name)
{
	for (; e && e->name; e++)
		if (!strcmp(e->name, name)) return e->value;
	return NULL;
}

static void test_dll(void)
{
	struct kexport *e = kos_load_dll("/sys/lib/libini.obj");
	check(e != NULL, "68.19 loads /sys/lib/libini.obj", (int)e, 0);
	if (!e) return;
	check(e == kos_load_dll("/sys/lib/libini.obj"), "68.19 again: the same export table", 0, 0);
	check(!strncmp(e[0].name, "lib_", 4), "the first export is lib_init", 0, 0);
	typedef int (*init_fn)(void *, void *, void *, void *);
	((init_fn)e[0].value)(malloc, free, realloc, NULL);
	typedef int (*get_str_fn)(const char *, const char *, const char *, char *, int, const char *);
	get_str_fn get_str = find_export(e, "ini_get_str");
	char buf[64] = "";
	if (get_str) get_str("/sys/kfar.ini", "Panels", "SortMode", buf, sizeof(buf), "none");
	check(get_str != NULL && buf[0], "a function called through the export table", buf[0], 0);
	check(kos_load_dll("/sys/lib/none.obj") == NULL, "68.19 of no file: 0", 0, 0);
	check(kos_load_dll("/sys/threadtest") == NULL, "68.19 of a program, not a library: 0", 0, 0);
}

/* --- the shell's console (shell_api.h's buffer: a command byte, the data,
 * at the end the return values) ----------------------------------------- */

#define SHELL_SHM_MAX (16 * 1024)
enum { SHELL_OK = 0, SHELL_EXIT = 1, SHELL_PUTS = 3, SHELL_PID = 7 };

static int shell_cmd(volatile u8 *b, int cmd)
{
	b[0] = cmd;
	for (int i = 0; i < 300; i++) {
		if (b[0] == SHELL_OK) return 1;
		kos_delay(1);
	}
	return 0;
}

static void shell_api(void)
{
	char name[32];
	snprintf(name, sizeof(name), "%u-SHELL", my_pid());
	u32 edx;
	volatile u8 *b = kos_shm_open(name, SHELL_SHM_MAX, KOS_SHM_OPEN_ALWAYS | KOS_SHM_WRITE, &edx);
	if (!b) {
		printf("threadtest: FAIL shell api: no area (%u)\n", edx);
		return;
	}
	/* the shell opens it within 0.6 s of starting us, then serves it */
	strcpy((char *)b + 1, "threadtest: this line came through the shell's console\n");
	int ok = shell_cmd(b, SHELL_PUTS);
	u32 pid = 0;
	ok = ok && shell_cmd(b, SHELL_PID);
	memcpy(&pid, (const void *)(b + 1), 4);
	int slot = pid ? kos_slot_of_pid(pid) : 0;
	char who[12] = { 0 };
	if (slot) {
		kos_thread_info(info, slot);
		memcpy(who, info + 10, 11);
	}
	ok = ok && !strcmp(who, "shell") && shell_cmd(b, SHELL_EXIT);
	printf("threadtest: %s the shell's console through %s (the shell's PID %u)\n", ok ? "ok" : "FAIL", name, pid);
	kos_shm_close(name);
}

int main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "echo")) {
		child_echo(atoi(argv[2]));
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "shellapi")) {
		shell_api();
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "orphan")) {
		child_orphan();
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "wait")) kos_delay(atoi(argv[2]));
	test_create();
	test_many();
	test_fp();
	test_window();
	test_ipc();
	test_processes();
	test_dll();
	printf("threadtest: done %d failed\n", failed);
	return 0;
}
