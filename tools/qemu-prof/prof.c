/* A QEMU TCG plugin for KolibriOS for ARM's profiles: executed guest
 * instructions per translation block, written out when QEMU ends.
 *
 *   qemu-system-aarch64 ... -plugin tools/qemu-prof/prof.so,out=FILE
 *
 * FILE gets one line per block: its start address (the guest's virtual
 * address), its instruction count and how often it ran. from=S counts
 * only what runs S seconds (host time) after QEMU's start: the steady
 * state after the boot. kbase=ADDR (the kernel's load address) counts
 * the kernel's instructions (from ADDR up) and the rest apart at each
 * mark: an AArch32 "orr r7, r7, r7" (0xe1877007) that a program runs
 * between the parts of a measurement (apps/sysbench); FILE.marks gets a
 * line per mark with the totals so far. report.py adds
 * up the kernel's blocks by symbol. Instruction counts do not depend on
 * how fast the host is (unlike TCG's wall clock); they do not see the
 * memory system (caches, the bus), which the device's kolibri.bench
 * measures. Built by tools/qemu-prof/Makefile. */
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

struct block {
	uint64_t vaddr;
	uint32_t ninsn;
	uint32_t used;
	struct qemu_plugin_scoreboard *count;
	uint64_t before;          /* the count at from=S */
};

static struct block *tab;
static size_t cap = 1 << 16, nused;
static char out_path[512] = "prof.out";
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned from_s;
static uint64_t kbase;
static FILE *marks;
static unsigned nmark;

static uint64_t hash(uint64_t v, uint32_t n)
{
	v ^= (uint64_t)n << 48;
	v *= 0x9e3779b97f4a7c15ull;
	return v ^ (v >> 29);
}

static struct block *lookup(uint64_t vaddr, uint32_t n);

static void grow(void)
{
	struct block *old = tab;
	size_t oc = cap;
	cap *= 2;
	tab = calloc(cap, sizeof(*tab));
	nused = 0;
	for (size_t i = 0; i < oc; i++)
		if (old[i].used) {
			struct block *b = lookup(old[i].vaddr, old[i].ninsn);
			b->count = old[i].count;
			b->before = old[i].before;
		}
	free(old);
}

static struct block *lookup(uint64_t vaddr, uint32_t n)
{
	if (nused * 2 >= cap) grow();
	for (size_t i = hash(vaddr, n) & (cap - 1);; i = (i + 1) & (cap - 1)) {
		struct block *b = &tab[i];
		if (!b->used) {
			b->used = 1;
			b->vaddr = vaddr;
			b->ninsn = n;
			b->count = NULL;
			b->before = 0;
			nused++;
			return b;
		}
		if (b->vaddr == vaddr && b->ninsn == n) return b;
	}
}

static void mark(unsigned int vcpu, void *u);

static void tb_trans(struct qemu_plugin_tb *tb, void *u)
{
	uint64_t va = qemu_plugin_tb_vaddr(tb);
	uint32_t n = qemu_plugin_tb_n_insns(tb);
	pthread_mutex_lock(&lock);
	struct block *b = lookup(va, n);
	if (!b->count) b->count = qemu_plugin_scoreboard_new(sizeof(uint64_t));
	struct qemu_plugin_scoreboard *c = b->count;
	pthread_mutex_unlock(&lock);
	qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(tb, QEMU_PLUGIN_INLINE_ADD_U64,
							  qemu_plugin_scoreboard_u64(c), 1);
	if (marks)
		for (uint32_t i = 0; i < n; i++) {
			struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, i);
			uint32_t w;
			if (qemu_plugin_insn_size(in) == 4 && qemu_plugin_insn_data(in, &w, 4) == 4 &&
			    w == 0xe1877007u)
				qemu_plugin_register_vcpu_insn_exec_cb(in, mark, QEMU_PLUGIN_CB_NO_REGS, NULL);
		}
}

/* a mark: the totals so far, the kernel's and the rest */
static void mark(unsigned int vcpu, void *u)
{
	uint64_t k = 0, o = 0;
	pthread_mutex_lock(&lock);
	for (size_t i = 0; i < cap; i++) {
		struct block *b = &tab[i];
		if (!b->used || !b->count) continue;
		uint64_t c = qemu_plugin_u64_sum(qemu_plugin_scoreboard_u64(b->count)) * b->ninsn;
		if (kbase && b->vaddr >= kbase) k += c; else o += c;
	}
	pthread_mutex_unlock(&lock);
	fprintf(marks, "mark %u: kernel %" PRIu64 " other %" PRIu64 "\n", nmark++, k, o);
	fflush(marks);
}

/* from=S: the counts so far, subtracted at the end */
static void *snapshot(void *arg)
{
	sleep(from_s);
	pthread_mutex_lock(&lock);
	for (size_t i = 0; i < cap; i++)
		if (tab[i].used && tab[i].count)
			tab[i].before = qemu_plugin_u64_sum(qemu_plugin_scoreboard_u64(tab[i].count));
	pthread_mutex_unlock(&lock);
	return NULL;
}

static void at_exit(void *p)
{
	FILE *f = fopen(out_path, "w");
	if (!f) return;
	for (size_t i = 0; i < cap; i++) {
		struct block *b = &tab[i];
		if (!b->used || !b->count) continue;
		uint64_t c = qemu_plugin_u64_sum(qemu_plugin_scoreboard_u64(b->count)) - b->before;
		if (c) fprintf(f, "%" PRIx64 " %u %" PRIu64 "\n", b->vaddr, b->ninsn, c);
	}
	fclose(f);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
					   int argc, char **argv)
{
	for (int i = 0; i < argc; i++)
		if (!strncmp(argv[i], "out=", 4))
			snprintf(out_path, sizeof(out_path), "%s", argv[i] + 4);
		else if (!strncmp(argv[i], "from=", 5))
			from_s = atoi(argv[i] + 5);
		else if (!strncmp(argv[i], "kbase=", 6))
			kbase = strtoull(argv[i] + 6, NULL, 16);
	if (kbase) {
		char m[600];
		snprintf(m, sizeof(m), "%s.marks", out_path);
		marks = fopen(m, "w");
	}
	tab = calloc(cap, sizeof(*tab));
	if (from_s) {
		pthread_t t;
		pthread_create(&t, NULL, snapshot, NULL);
		pthread_detach(t);
	}
	qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans, NULL);
	qemu_plugin_register_atexit_cb(id, at_exit, NULL);
	return 0;
}
