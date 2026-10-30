/* KolibriOS for ARM: UFS, a UFS host controller (UFSHCI 2.0 and later)
 * and its logical units as disks "ufs0" ... "ufs7" (by LUN: ufs0 is
 * Linux's sda).
 *
 * Two ways in:
 *  - the Pro1X (keep_link): Linux, starting KolibriOS with kexec, leaves
 *    the controller running with the link up and the device initialised
 *    (pro1x-maemo-leste's kernel patch 0004 keeps UFS powered across
 *    kexec); the controller's PHY, clocks and regulators need Qualcomm
 *    drivers this kernel does not have. As FreeBSD's port does it
 *    (freebsd/patches/0005, UFSHCI_QUIRK_KEEP_LINK): the controller must
 *    be enabled with a device present, its interrupts off, the request
 *    lists stopped and given our memory, what is pending cleared, the
 *    link and its power mode (and Auto-Hibernate) left as they are; no UIC
 *    command, no reset;
 *  - QEMU's UFS model (-device ufs, ufs-lu; on virt's PCIe): enabled and
 *    the link started here (DME_LINKSTARTUP), the device by fDeviceInit.
 *
 * One transfer request at a time (slot 0), polled: the UTRD, the UCD with
 * the command UPIU, the response UPIU and one PRDT entry for a bounce
 * buffer, all in the DMA memory the platform gives (uncached, the bus's
 * addresses: on the Pro1X the stream's SMMU context with its translation
 * off). SCSI: TEST UNIT READY, INQUIRY, READ CAPACITY(16), READ(16),
 * WRITE(16) (the fence asked first: blk.c), REQUEST SENSE through the
 * response's sense data. Nothing else is sent: no START STOP UNIT, no
 * FORMAT, no UNMAP, no query that writes the device's configuration
 * (fDeviceInit only on QEMU's model). */
#include "storage.h"
#include "ufs.h"

/* registers */
#define CAP 0x00
#define VER 0x08
#define AHIT 0x18
#define IS 0x20
#define IE 0x24
#define HCS 0x30
#define HCE 0x34
#define UTRLBA 0x50
#define UTRLBAU 0x54
#define UTRLDBR 0x58
#define UTRLCLR 0x5c
#define UTRLRSR 0x60
#define UTMRLBA 0x70
#define UTMRLBAU 0x74
#define UTMRLDBR 0x78
#define UTMRLRSR 0x80
#define UICCMD 0x90
#define UICCMDARG1 0x94
#define UICCMDARG2 0x98
#define UICCMDARG3 0x9c

#define CAP_64AS (1u << 24)
#define HCS_DP (1u << 0)
#define HCS_UTRLRDY (1u << 1)
#define HCS_UTMRLRDY (1u << 2)
#define HCS_UCRDY (1u << 3)
#define IS_UTRCS (1u << 0)
#define IS_UCCS (1u << 10)
/* the fatal ones: device, UTP, host controller, system bus, crypto engine
 * (the link's power mode and hibernate bits come with Auto-Hibernate) */
#define IS_FATAL 0x00071800u

/* the UPIUs */
#define UPIU_NOP_OUT 0x00
#define UPIU_COMMAND 0x01
#define UPIU_QUERY 0x16
#define UPIU_NOP_IN 0x20
#define UPIU_RESPONSE 0x21
#define UPIU_QUERY_RSP 0x36
#define F_READ 0x40
#define F_WRITE 0x20

/* the DMA memory: the UTRL (32 x 32 bytes), the UTMRL, the UCD, the bounce */
#define O_UTRL 0x0000
#define O_UTMRL 0x0400
#define O_UCD 0x0800                    /* command UPIU 512, response 512, PRDT */
#define UCD_RSP 512
#define UCD_PRDT 1024
#define O_BOUNCE 0x2000
#define BOUNCE (256u << 10)             /* one PRDT entry at most */
#define DMA_NEED (O_BOUNCE + BOUNCE)

#define MAX_LUNS 8

struct lun {
	struct blkdev dev;
	struct ufs *u;
	u8 lun;
};

struct ufs {
	const struct ufs_hw *hw;
	u8 *utrl, *ucd, *bounce;
	u64 utrl_pa, utmrl_pa, ucd_pa, bounce_pa;
	u8 tag;
	bool quiet;                     /* probing: errors are an answer */
	struct lun luns[MAX_LUNS];
};

static struct ufs ufs0;

static u32 rd(struct ufs *u, u32 off) { return u->hw->rd(u->hw->ctx, off); }
static void wr(struct ufs *u, u32 off, u32 v) { u->hw->wr(u->hw->ctx, off, v); }

/* polls until (reg & mask) == want, at most ms; false: timed out */
static bool wait_reg(struct ufs *u, u32 off, u32 mask, u32 want, u32 ms)
{
	u64 t0 = u->hw->now_us();
	for (;;) {
		if ((rd(u, off) & mask) == want) return true;
		if (u->hw->now_us() - t0 > (u64)ms * 1000) return (rd(u, off) & mask) == want;
	}
}

/* --- a transfer request --------------------------------------------------- */

/* the UTRD in slot 0 for the UPIU in the UCD; dd: 0 none, 1 to the
 * device, 2 from it; len: the bounce buffer's bytes */
static int transfer(struct ufs *u, u32 dd, u32 len, u32 ms)
{
	u8 *d = u->utrl;
	memset(d, 0, 32);
	put_le32(d + 0, 1u << 28 | dd << 25);           /* CT 1: UFS storage */
	put_le32(d + 8, 0x0f);                          /* OCS: invalid until done */
	put_le32(d + 16, (u32)u->ucd_pa);
	put_le32(d + 20, (u32)(u->ucd_pa >> 32));
	put_le32(d + 24, (UCD_RSP / 4) << 16 | (512 / 4));   /* response offset, length (dwords) */
	put_le32(d + 28, (UCD_PRDT / 4) << 16 | (len ? 1 : 0));
	u8 *prd = u->ucd + UCD_PRDT;
	memset(prd, 0, 16);
	if (len) {
		put_le32(prd + 0, (u32)u->bounce_pa);
		put_le32(prd + 4, (u32)(u->bounce_pa >> 32));
		put_le32(prd + 12, len - 1);
	}
	memset(u->ucd + UCD_RSP, 0, 512);
	u->hw->barrier();
	wr(u, IS, rd(u, IS));
	wr(u, UTRLDBR, 1);
	bool done = wait_reg(u, UTRLDBR, 1, 0, ms);
	u->hw->barrier();
	u32 is = rd(u, IS);
	wr(u, IS, is);
	if (!done) {
		slog("ufs: request %u: no answer in %u ms (IS 0x%x)\n", u->tag, ms, is);
		wr(u, UTRLCLR, ~1u);                        /* slot 0 cleared */
		return -1;
	}
	u32 ocs = le32(d + 8) & 0xff;
	if (ocs || (is & IS_FATAL)) {
		if (!u->quiet || (is & IS_FATAL)) slog("ufs: request %u: OCS 0x%x, IS 0x%x\n", u->tag, ocs, is);
		return -1;
	}
	return 0;
}

static void upiu_head(struct ufs *u, u8 type, u8 flags, u8 lun)
{
	u8 *c = u->ucd;
	memset(c, 0, 512);
	c[0] = type;
	c[1] = flags;
	c[2] = lun;
	c[3] = u->tag = (u->tag + 1) & 0x7f;
}

static bool nop(struct ufs *u)
{
	upiu_head(u, UPIU_NOP_OUT, 0, 0);
	if (transfer(u, 0, 0, 1500)) return false;
	return u->ucd[UCD_RSP] == UPIU_NOP_IN && u->ucd[UCD_RSP + 3] == u->tag;
}

/* sense key, ASC, ASCQ of the last CHECK CONDITION */
static u8 sense_key, sense_asc;

/* a SCSI command to a LUN; dd and len as transfer's; 0, -1 (failed),
 * 2 (CHECK CONDITION: sense_key, sense_asc) */
static int scsi(struct ufs *u, u8 lun, const u8 *cdb, u32 cdblen, u32 dd, u32 len)
{
	upiu_head(u, UPIU_COMMAND, dd == 2 ? F_READ : dd == 1 ? F_WRITE : 0, lun);
	put_be32(u->ucd + 12, len);                     /* expected data transfer length */
	memcpy(u->ucd + 16, cdb, cdblen);
	if (transfer(u, dd, len, 10000)) return -1;
	const u8 *r = u->ucd + UCD_RSP;
	if (r[0] != UPIU_RESPONSE || r[3] != u->tag) {
		slog("ufs: LUN %u: not a response (0x%x, tag %u)\n", lun, r[0], r[3]);
		return -1;
	}
	if (r[7] == 0x02) {                             /* CHECK CONDITION (whatever the response says) */
		sense_key = be16_at(r + 10) >= 16 ? r[32 + 2 + 2] & 0x0f : 0;
		sense_asc = be16_at(r + 10) >= 16 ? r[32 + 2 + 12] : 0;
		return 2;
	}
	if (r[6]) {                                     /* the response: target failure */
		slog("ufs: LUN %u: response 0x%x, status 0x%x\n", lun, r[6], r[7]);
		return -1;
	}
	if (r[7]) {
		slog("ufs: LUN %u: SCSI status 0x%x\n", lun, r[7]);
		return -1;
	}
	return 0;
}

/* a command with a retry after a UNIT ATTENTION (a reset, a change) */
static int scsi_retry(struct ufs *u, u8 lun, const u8 *cdb, u32 cdblen, u32 dd, u32 len)
{
	int r = -1;
	for (int i = 0; i < 4; i++) {
		r = scsi(u, lun, cdb, cdblen, dd, len);
		if (r != 2 || sense_key != 0x06) return r;
	}
	return r;
}

/* --- the disks -------------------------------------------------------------- */

static int rw16(struct lun *l, u8 op, u64 lba, u32 n)
{
	u8 cdb[16] = { op };
	put_be64(cdb + 2, lba);
	put_be32(cdb + 10, n);
	int r = scsi_retry(l->u, l->lun, cdb, 16, op == 0x88 ? 2 : 1, n * l->dev.bsize);
	if (r == 2) slog("ufs: LUN %u: %s at %u: sense %x/%x\n", l->lun, op == 0x88 ? "read" : "write",
			 (u32)lba, sense_key, sense_asc);
	return r ? -1 : 0;
}

static int lread(struct blkdev *d, u64 lba, u32 n, void *buf)
{
	struct lun *l = d->priv;
	if (rw16(l, 0x88, lba, n)) return -1;           /* READ(16) */
	memcpy(buf, l->u->bounce, n * d->bsize);
	return 0;
}

/* only the fence calls it (blk.c), and the fence is asked again */
static int lwrite(struct blkdev *d, u64 lba, u32 n, const void *buf)
{
	struct lun *l = d->priv;
	if (!blk_fence_allows(d, lba, n)) {
		slog("ufs: LUN %u: write at %u refused (the fence)\n", l->lun, (u32)lba);
		return -1;
	}
	memcpy(l->u->bounce, buf, n * d->bsize);
	return rw16(l, 0x8a, lba, n);                   /* WRITE(16) */
}

static void lun_probe(struct ufs *u, u8 lun, bool writable)
{
	u8 tur[6] = { 0x00 };
	u->quiet = true;                                /* a LUN that is not there: an error */
	int r = scsi_retry(u, lun, tur, 6, 0, 0);
	u->quiet = false;
	if (r == -1 || (r == 2 && sense_asc == 0x25)) return;   /* not there, LOGICAL UNIT NOT SUPPORTED */
	if (r == 2 && sense_key == 0x02) {              /* NOT READY: once more, a moment later */
		u64 t0 = u->hw->now_us();
		while (u->hw->now_us() - t0 < 200000)
			;
		r = scsi_retry(u, lun, tur, 6, 0, 0);
	}
	if (r) {
		if (r == 2) slog("ufs: LUN %u: not ready (sense %x/%x)\n", lun, sense_key, sense_asc);
		return;
	}
	u8 inq[6] = { 0x12, 0, 0, 0, 36, 0 };
	char what[48] = "UFS LUN 0";
	what[8] = '0' + lun;
	if (!scsi_retry(u, lun, inq, 6, 2, 36)) {
		u32 o = 9;
		what[o++] = ',';
		what[o++] = ' ';
		for (int i = 8; i < 32 && o + 2 < sizeof(what); i++) {
			char c = u->bounce[i];
			if (c < 32 || c > 126) c = ' ';
			if (c == ' ' && what[o - 1] == ' ') continue;
			what[o++] = c;
		}
		while (o && what[o - 1] == ' ') o--;
		what[o] = 0;
	}
	u8 cap[16] = { 0x9e, 0x10 };                    /* READ CAPACITY(16) */
	put_be32(cap + 10, 32);
	if (scsi_retry(u, lun, cap, 16, 2, 32)) {
		slog("ufs: LUN %u: no capacity\n", lun);
		return;
	}
	u64 last = be64_at(u->bounce);
	u32 bs = be32_at(u->bounce + 8);
	if (bs != 512 && bs != 4096) {
		slog("ufs: LUN %u: blocks of %u bytes\n", lun, bs);
		return;
	}
	struct lun *l = &u->luns[lun];
	l->u = u;
	l->lun = lun;
	struct blkdev *d = &l->dev;
	d->name[0] = 'u', d->name[1] = 'f', d->name[2] = 's', d->name[3] = '0' + lun, d->name[4] = 0;
	memcpy(d->what, what, sizeof(d->what));
	d->bsize = bs;
	d->nblocks = last + 1;
	d->max_blocks = BOUNCE / bs;
	d->read = lread;
	d->priv = l;
	blk_add(d, writable ? lwrite : NULL);
}

/* --- the controller ---------------------------------------------------------- */

/* QEMU: a UIC command; 0 or its error */
static int uic(struct ufs *u, u32 cmd, u32 a1, u32 a2, u32 a3)
{
	if (!wait_reg(u, HCS, HCS_UCRDY, HCS_UCRDY, 100)) return -1;
	wr(u, IS, IS_UCCS);
	wr(u, UICCMDARG1, a1);
	wr(u, UICCMDARG2, a2);
	wr(u, UICCMDARG3, a3);
	wr(u, UICCMD, cmd);
	if (!wait_reg(u, IS, IS_UCCS, IS_UCCS, 500)) return -1;
	wr(u, IS, IS_UCCS);
	return rd(u, UICCMDARG2) & 0xff;
}

/* QEMU: the controller enabled, the link started */
static bool bring_up(struct ufs *u)
{
	wr(u, HCE, 0);
	if (!wait_reg(u, HCE, 1, 0, 100)) return false;
	wr(u, HCE, 1);
	if (!wait_reg(u, HCE, 1, 1, 100)) return false;
	for (int i = 0; i < 3; i++) {
		if (uic(u, 0x16, 0, 0, 0) == 0 && wait_reg(u, HCS, HCS_DP, HCS_DP, 100)) return true;   /* DME_LINKSTARTUP */
	}
	return false;
}

/* QEMU: fDeviceInit set, until the device clears it */
static bool device_init(struct ufs *u)
{
	for (int set = 1; set >= 0; set--) {
		for (int i = 0; i < 100; i++) {
			upiu_head(u, UPIU_QUERY, 0, 0);
			u->ucd[5] = set ? 0x81 : 0x01;          /* query function: standard write, read request */
			u->ucd[12] = set ? 0x06 : 0x05;         /* SET FLAG, READ FLAG */
			u->ucd[13] = 0x01;                      /* fDeviceInit */
			if (transfer(u, 0, 0, 1500) || u->ucd[UCD_RSP] != UPIU_QUERY_RSP || u->ucd[UCD_RSP + 6]) return false;
			if (set) break;
			if (!(u->ucd[UCD_RSP + 23] & 1)) return true;     /* the flag's value */
		}
	}
	return false;
}

int ufs_start(const struct ufs_hw *hw)
{
	struct ufs *u = &ufs0;
	if (u->hw) return 0;
	if (hw->dma.size < DMA_NEED) {
		slog("ufs: %u KiB of DMA memory, %u needed\n", hw->dma.size >> 10, DMA_NEED >> 10);
		return 0;
	}
	u->hw = hw;
	u32 cap = rd(u, CAP), ver = rd(u, VER), hce = rd(u, HCE), hcs = rd(u, HCS);
	slog("ufs: UFSHCI %u.%u, CAP 0x%x, HCE %u, HCS 0x%x, AHIT 0x%x, UTRLDBR 0x%x, UTRLRSR %u\n",
	     ver >> 8 & 0xff, ver >> 4 & 0xf, cap, hce & 1, hcs, rd(u, AHIT), rd(u, UTRLDBR), rd(u, UTRLRSR) & 1);
	if ((ver >> 8 & 0xff) < 2) {
		slog("ufs: a controller before UFSHCI 2.0: not driven\n");
		u->hw = NULL;
		return 0;
	}
	if (!(cap & CAP_64AS) && (hw->dma.pa >> 32)) {
		slog("ufs: 32-bit DMA, the memory at 0x%x%08x is above 4 GiB\n", (u32)(hw->dma.pa >> 32), (u32)hw->dma.pa);
		u->hw = NULL;
		return 0;
	}
	/* a previous kernel's interrupts off before anything else */
	wr(u, IE, 0);
	if (hw->keep_link) {
		if (!(hce & 1) || !(hcs & HCS_DP)) {
			slog("ufs: the link is not up (HCE %u, device %s): Linux did not leave UFS running "
			     "(pro1x-maemo-leste's kernel patch 0004)\n", hce & 1, hcs & HCS_DP ? "present" : "absent");
			u->hw = NULL;
			return 0;
		}
		slog("ufs: keeping the link that is up\n");
		wr(u, UTMRLRSR, 0);
		wr(u, UTRLRSR, 0);
		if (rd(u, UTRLDBR) || rd(u, UTMRLDBR)) {
			slog("ufs: requests pending (0x%x, 0x%x): cleared\n", rd(u, UTRLDBR), rd(u, UTMRLDBR));
			wr(u, UTRLCLR, 0);
		}
	} else if (!bring_up(u)) {
		slog("ufs: the link did not come up\n");
		u->hw = NULL;
		return 0;
	}
	wr(u, IS, rd(u, IS));
	if (!wait_reg(u, HCS, HCS_UTRLRDY | HCS_UTMRLRDY, HCS_UTRLRDY | HCS_UTMRLRDY, 100)) {
		slog("ufs: the request lists are not ready (HCS 0x%x)\n", rd(u, HCS));
		u->hw = NULL;
		return 0;
	}
	u8 *m = hw->dma.va;
	memset(m, 0, O_BOUNCE);
	u->utrl = m + O_UTRL;
	u->ucd = m + O_UCD;
	u->bounce = m + O_BOUNCE;
	u->utrl_pa = hw->dma.pa + O_UTRL;
	u->utmrl_pa = hw->dma.pa + O_UTMRL;
	u->ucd_pa = hw->dma.pa + O_UCD;
	u->bounce_pa = hw->dma.pa + O_BOUNCE;
	wr(u, UTRLBA, (u32)u->utrl_pa);
	wr(u, UTRLBAU, (u32)(u->utrl_pa >> 32));
	wr(u, UTMRLBA, (u32)u->utmrl_pa);
	wr(u, UTMRLBAU, (u32)(u->utmrl_pa >> 32));
	wr(u, UTMRLRSR, 1);
	wr(u, UTRLRSR, 1);
	hw->barrier();
	bool ok = false;
	for (int i = 0; i < 10 && !ok; i++) ok = nop(u);
	if (!ok) {
		slog("ufs: the device does not answer NOP OUT\n");
		wr(u, UTRLRSR, 0);
		u->hw = NULL;
		return 0;
	}
	if (!hw->keep_link && !device_init(u)) slog("ufs: fDeviceInit not cleared\n");
	int n = blk_count();
	for (u8 lun = 0; lun < MAX_LUNS; lun++) lun_probe(u, lun, hw->writable);
	n = blk_count() - n;
	slog("ufs: %d logical unit(s)\n", n);
	return n;
}

/* tests (QEMU's model): the controller left as Linux leaves it for kexec
 * (enabled, the link up, the device initialised, its requests from a list
 * of its own elsewhere, interrupts on, Auto-Hibernate set), so that
 * ufs_start with keep_link takes it over as on the Pro1X */
bool ufs_leave_running(const struct ufs_hw *hw)
{
	struct ufs *u = &ufs0;
	if (u->hw || hw->dma.size < 2 * DMA_NEED) return false;
	u->hw = hw;
	u8 *m = hw->dma.va + hw->dma.size - DMA_NEED;
	u64 pa = hw->dma.pa + hw->dma.size - DMA_NEED;
	bool ok = bring_up(u) &&
		  wait_reg(u, HCS, HCS_UTRLRDY | HCS_UTMRLRDY, HCS_UTRLRDY | HCS_UTMRLRDY, 100);
	if (ok) {
		memset(m, 0, O_BOUNCE);
		u->utrl = m + O_UTRL;
		u->ucd = m + O_UCD;
		u->bounce = m + O_BOUNCE;
		u->utrl_pa = pa + O_UTRL;
		u->ucd_pa = pa + O_UCD;
		u->bounce_pa = pa + O_BOUNCE;
		wr(u, UTRLBA, (u32)u->utrl_pa);
		wr(u, UTRLBAU, (u32)(u->utrl_pa >> 32));
		wr(u, UTMRLBA, (u32)(pa + O_UTMRL));
		wr(u, UTMRLBAU, (u32)((pa + O_UTMRL) >> 32));
		wr(u, UTMRLRSR, 1);
		wr(u, UTRLRSR, 1);
		ok = nop(u) && device_init(u);
	}
	if (ok && (rd(u, CAP) & (1u << 23))) wr(u, AHIT, 3u << 10 | 150);   /* 150 ms */
	if (ok) wr(u, IE, IS_UTRCS | IS_FATAL);
	slog("ufs: left running as Linux leaves it: %s\n", ok ? "yes" : "no");
	u->hw = NULL;
	return ok;
}
