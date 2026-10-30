/* KolibriOS for ARM: kernel/storage/ufs.c on the PC against a model of a
 * UFS host controller (UFSHCI registers, UTP transfer requests, the
 * UPIUs, SCSI on its logical units; make -C tests/storage ufs):
 *
 *   ufs_sim LUN0.img [ranges]
 *
 * The model starts as Linux leaves the Pro1X's controller for kexec
 * (enabled, the link up, its own request list running, interrupts on,
 * Auto-Hibernate set) and counts what the driver must not do there: write
 * HCE, send a UIC command, touch Auto-Hibernate, move a list's base while
 * it runs, ring the doorbell of a stopped list, send a SCSI command other
 * than TEST UNIT READY, INQUIRY, READ CAPACITY(16), READ(16), WRITE(16),
 * misalign a descriptor. LUN 0 is the image (4096-byte blocks, mkdisk.py
 * pro1x), LUNs 1-5 small empty ones, each answers its first command with
 * a UNIT ATTENTION. Then the block layer and the container on top: the
 * writes that reach the model's LUNs must all be inside the container's
 * ranges. Before that, a run that finds the link down must give up
 * without a single request. (The way up from a reset, DME_LINKSTARTUP
 * and fDeviceInit, is QEMU's UFS model's test: run.py ufs-qemu.) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "../../kernel/storage/storage.h"
#include "../../kernel/storage/ufs.h"

static int failed, passed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

u32 rtc_seconds(void) { return (u32)time(NULL); }
static u64 now_us(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (u64)tv.tv_sec * 1000000 + tv.tv_usec;
}
static void barrier(void) { __sync_synchronize(); }

/* --- the model ------------------------------------------------------------ */

#define NLUN 6
static struct {
	u32 cap, ver, hce, hcs, is, ie, ahit, utrlba, utrlbau, utrldbr, utrlrsr, utmrlba, utmrlbau, utmrlrsr;
	u32 uicarg[3];
	bool kept;                     /* started as Linux left it */
	int violations, requests, uic;
	FILE *lun[NLUN];
	u64 nblocks[NLUN];
	bool ua[NLUN];
	struct { int lun; u64 lba; u32 n; } wlog[4096];
	int nw;
} m;

static void violation(const char *what)
{
	m.violations++;
	printf("model: VIOLATION: %s\n", what);
}

static void put_rsp(u8 *rsp, u8 type, u8 tag, u8 status)
{
	memset(rsp, 0, 64);
	rsp[0] = type;
	rsp[3] = tag;
	rsp[7] = status;
}

static void sense(u8 *rsp, u8 key, u8 asc)
{
	put_be16(rsp + 10, 20);
	put_be16(rsp + 32, 18);
	rsp[34] = 0x70;
	rsp[36] = key;
	rsp[46] = asc;
}

/* the PRDT's buffers, copied from or to data (len bytes) */
static bool prdt_copy(u8 *ucd, u32 prdto, u32 prdtl, u8 *data, u32 len, bool to_host)
{
	u8 *prd = ucd + prdto * 4;
	u32 at = 0;
	for (u32 i = 0; i < prdtl && at < len; i++, prd += 16) {
		u64 a = le64(prd);
		u32 n = (le32(prd + 12) & 0x3ffff) + 1;
		if (a & 3) violation("a PRDT buffer not dword aligned");
		n = MIN(n, len - at);
		if (to_host) memcpy((void *)(addr_t)a, data + at, n);
		else memcpy(data + at, (void *)(addr_t)a, n);
		at += n;
	}
	return at == len;
}

static void scsi(u8 *ucd, u8 *rsp, u32 dw7)
{
	u8 lun = ucd[2], tag = ucd[3], *cdb = ucd + 16;
	u32 prdto = dw7 >> 16, prdtl = dw7 & 0xffff, xfer = be32_at(ucd + 12);
	static u8 data[1 << 20];
	put_rsp(rsp, 0x21, tag, 0);
	if (lun >= NLUN || !m.lun[lun]) {
		rsp[7] = 2;
		sense(rsp, 0x05, 0x25);
		return;
	}
	if (m.ua[lun]) {
		m.ua[lun] = false;
		rsp[7] = 2;
		sense(rsp, 0x06, 0x29);
		return;
	}
	switch (cdb[0]) {
	case 0x00:
		return;
	case 0x12:
		memset(data, 0, 36);
		memcpy(data + 8, "SAMSUNG KLUDG4UHDB-B2D1  ", 24);
		prdt_copy(ucd, prdto, prdtl, data, MIN(xfer, 36), true);
		return;
	case 0x9e:
		if ((cdb[1] & 0x1f) != 0x10) break;
		memset(data, 0, 32);
		put_be64(data, m.nblocks[lun] - 1);
		put_be32(data + 8, 4096);
		prdt_copy(ucd, prdto, prdtl, data, MIN(xfer, 32), true);
		return;
	case 0x88: case 0x8a: {
		u64 lba = be64_at(cdb + 2);
		u32 n = be32_at(cdb + 10);
		if (lba + n > m.nblocks[lun] || n * 4096 > sizeof(data) || xfer != n * 4096) {
			rsp[7] = 2;
			sense(rsp, 0x05, 0x21);
			return;
		}
		fseeko(m.lun[lun], (off_t)lba * 4096, SEEK_SET);
		if (cdb[0] == 0x88) {
			if (fread(data, 4096, n, m.lun[lun]) != n) memset(data, 0, n * 4096);
			prdt_copy(ucd, prdto, prdtl, data, n * 4096, true);
		} else {
			prdt_copy(ucd, prdto, prdtl, data, n * 4096, false);
			fwrite(data, 4096, n, m.lun[lun]);
			if (m.nw < (int)ARRAY_SIZE(m.wlog)) m.wlog[m.nw++] = (typeof(m.wlog[0])){ lun, lba, n };
		}
		return;
	}
	}
	char w[64];
	snprintf(w, sizeof(w), "SCSI command 0x%02x", cdb[0]);
	violation(w);
	rsp[7] = 2;
	sense(rsp, 0x05, 0x20);
}

static void request(u32 slot)
{
	u64 base = (u64)m.utrlbau << 32 | m.utrlba;
	if (base & 1023) violation("the UTRL not 1 KiB aligned");
	u8 *d = (u8 *)(addr_t)(base + 32 * slot);
	u32 dw0 = le32(d), dw6 = le32(d + 24), dw7 = le32(d + 28);
	u64 ucdba = le64(d + 16);
	if (ucdba & 127) violation("a UCD not 128-byte aligned");
	if (dw0 >> 28 != 1) violation("command type not UFS storage");
	if ((le32(d + 8) & 0xff) != 0x0f) violation("OCS not set to invalid before the doorbell");
	u8 *ucd = (u8 *)(addr_t)ucdba, *rsp = ucd + (dw6 >> 16) * 4;
	m.requests++;
	switch (ucd[0]) {
	case 0x00:
		put_rsp(rsp, 0x20, ucd[3], 0);
		break;
	case 0x01:
		scsi(ucd, rsp, dw7);
		break;
	case 0x16:
		if (m.kept) violation("a query while keeping the link");
		put_rsp(rsp, 0x36, ucd[3], 0);
		rsp[12] = ucd[12];
		rsp[13] = ucd[13];
		break;
	default:
		violation("an unknown UPIU");
	}
	put_le32(d + 8, 0);                     /* OCS: success */
}

static u32 mrd(void *c, u32 off)
{
	switch (off) {
	case 0x00: return m.cap;
	case 0x08: return m.ver;
	case 0x18: return m.ahit;
	case 0x20: return m.is;
	case 0x24: return m.ie;
	case 0x30: return m.hcs;
	case 0x34: return m.hce;
	case 0x50: return m.utrlba;
	case 0x54: return m.utrlbau;
	case 0x58: return m.utrldbr;
	case 0x60: return m.utrlrsr;
	case 0x70: return m.utmrlba;
	case 0x74: return m.utmrlbau;
	case 0x78: return 0;
	case 0x80: return m.utmrlrsr;
	case 0x98: return m.uicarg[1];
	}
	return 0;
}

static void mwr(void *c, u32 off, u32 v)
{
	switch (off) {
	case 0x18:
		if (m.kept) violation("Auto-Hibernate written while keeping the link");
		m.ahit = v;
		break;
	case 0x20: m.is &= ~v; break;
	case 0x24: m.ie = v; break;
	case 0x34:
		if (m.kept) violation("HCE written while keeping the link");
		m.hce = v & 1;
		if (!m.hce) m.hcs = 0, m.utrlrsr = m.utmrlrsr = 0;
		else m.hcs = 1u << 3;                   /* UCRDY */
		break;
	case 0x50: case 0x54:
		if (m.utrlrsr) violation("the UTRL's base moved while it runs");
		if (off == 0x50) m.utrlba = v; else m.utrlbau = v;
		break;
	case 0x70: case 0x74:
		if (m.utmrlrsr) violation("the UTMRL's base moved while it runs");
		if (off == 0x70) m.utmrlba = v; else m.utmrlbau = v;
		break;
	case 0x58:
		if (!m.utrlrsr) violation("the doorbell of a stopped list");
		for (u32 s = 0; s < 32; s++)
			if (v & 1u << s) request(s);
		m.is |= 1;
		break;
	case 0x5c: m.utrldbr &= v; break;
	case 0x60: m.utrlrsr = v & 1; if (!m.utrlrsr) m.utrldbr = 0; break;
	case 0x80: m.utmrlrsr = v & 1; break;
	case 0x90:
		m.uic++;
		if (m.kept) violation("a UIC command while keeping the link");
		if (v == 0x16 && m.hce) m.hcs = 0xf;   /* DP, the lists ready, UCRDY */
		m.uicarg[1] = 0;
		m.is |= 1u << 10;
		break;
	case 0x94: case 0x98: case 0x9c: m.uicarg[(off - 0x94) / 4] = v; break;
	}
}

static void model_reset(FILE *lun0, bool kept, bool link)
{
	FILE *keep[NLUN];
	memcpy(keep, m.lun, sizeof(keep));
	memset(&m, 0, sizeof(m));
	memcpy(m.lun, keep, sizeof(keep));
	m.lun[0] = lun0;
	m.cap = 31 | 1u << 23 | 1u << 24;               /* 32 slots, Auto-Hibernate, 64-bit */
	m.ver = 0x300;
	m.kept = kept;
	for (int i = 0; i < NLUN; i++) {
		if (!m.lun[i]) continue;
		fseeko(m.lun[i], 0, SEEK_END);
		m.nblocks[i] = (u64)ftello(m.lun[i]) / 4096;
		m.ua[i] = true;
	}
	if (kept) {
		static u8 linux_list[2048] __attribute__((aligned(1024)));
		m.hce = 1;
		m.hcs = link ? 0xf : 0x0e;
		m.utrlba = (u32)(addr_t)linux_list;
		m.utrlbau = (u32)((u64)(addr_t)linux_list >> 32);
		m.utrlrsr = m.utmrlrsr = 1;
		m.ie = 0x30c5f;
		m.ahit = 3u << 10 | 1;
	}
}

static struct ufs_hw hw = { .rd = mrd, .wr = mwr, .now_us = now_us, .barrier = barrier };

int ufs_start(const struct ufs_hw *hw);

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "ufs_sim LUN0.img\n");
		return 2;
	}
	FILE *lun0 = fopen(argv[1], "r+b");
	if (!lun0) { perror(argv[1]); return 2; }
	static struct { unsigned long long off, len; } rs[FMAP_MAX];
	int nrs = 0;
	char rp[1024];
	snprintf(rp, sizeof(rp), "%s.ranges", argv[1]);
	FILE *rf = fopen(rp, "r");
	while (rf && nrs < FMAP_MAX && fscanf(rf, "%llu %llu", &rs[nrs].off, &rs[nrs].len) == 2) nrs++;
	if (rf) fclose(rf);
	for (int i = 1; i < NLUN; i++) {
		m.lun[i] = tmpfile();
		static u8 z[4096];
		for (int k = 0; k < 64 * i; k++) fwrite(z, 1, sizeof(z), m.lun[i]);
	}
	static u8 dma[2 << 20] __attribute__((aligned(4096)));
	hw.dma = (struct dma_mem){ dma, (u64)(addr_t)dma, sizeof(dma) };

	/* 3: the link down: nothing */
	model_reset(lun0, true, false);
	hw.keep_link = true;
	CHECK(ufs_start(&hw) == 0, "the link down: no logical units");
	CHECK(m.requests == 0 && m.violations == 0, "no request (%d), no violation (%d)", m.requests, m.violations);

	/* 1: as Linux left it */
	model_reset(lun0, true, true);
	hw.writable = true;
	int n = ufs_start(&hw);
	CHECK(n == NLUN, "%d logical units", n);
	CHECK(m.uic == 0, "no UIC command");
	CHECK(m.utrlba == (u32)(addr_t)dma, "the list in our memory");
	CHECK(blk_count() == NLUN && blk_get(0)->bsize == 4096, "ufs0 with 4096-byte blocks");
	blk_scan();
	home_init(HOME_RW);
	struct vol *h = home_vol();
	CHECK(h != NULL, "the container on ufs0/13");
	if (h) {
		u32 done = 0;
		static u8 big[1 << 20];
		memset(big, 0x5a, sizeof(big));
		CHECK(h->fs->write(h, "ufs-sim.bin", 0, sizeof(big), big, true, &done) == E_OK, "a file written");
		static u8 back[1 << 20];
		blk_cache_drop();
		u32 got = 0;
		CHECK(h->fs->read(h, "ufs-sim.bin", 0, sizeof(back), back, &got) == E_OK && got == sizeof(big) &&
		      !memcmp(back, big, sizeof(big)), "read back through the model");
	}
	int outside = 0;
	for (int i = 0; i < m.nw; i++) {
		unsigned long long b0 = m.wlog[i].lba * 4096, b1 = b0 + m.wlog[i].n * 4096ull;
		bool in = false;
		for (int k = 0; k < nrs; k++) in |= m.wlog[i].lun == 0 && b0 >= rs[k].off && b1 <= rs[k].off + rs[k].len;
		if (!in) outside++, printf("FAIL: LUN %d write at %llu+%u outside\n", m.wlog[i].lun, (unsigned long long)m.wlog[i].lba, m.wlog[i].n);
	}
	CHECK(m.nw > 0 && outside == 0, "%d writes, %d outside the container", m.nw, outside);
	CHECK(m.violations == 0, "%d violations", m.violations);
	printf("ufs_sim %s: %d passed, %d failed, %d requests, %d writes\n", argv[1], passed, failed, m.requests, m.nw);
	return failed != 0;
}
