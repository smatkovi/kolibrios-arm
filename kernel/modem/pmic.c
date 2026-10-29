/* KolibriOS for ARM: the SPMI PMIC arbiter, version 5, and the Pro1X's
 * battery (pmic.h).
 *
 * After Linux's spmi-pmic-arb.c and FreeBSD's qcom_spmi (pro1x-maemo-leste,
 * freebsd/patches/0011): every PMIC peripheral (its PPID: the slave id and
 * the address's high byte) has a channel (APID); the map is in the core
 * region (APID_MAP at 0x900, the PPID in bits 8-19), each channel's owner
 * (the EE that may write it) in cnfg (0x700). Reads go through this EE's
 * observer channel (obsrvr + 0x10000 * ee + 0x80 * apid), writes through
 * the channel itself (chnls + 0x10000 * apid): a command word (the
 * opcode in bits 27-31, the address's low byte in 4-11, the length - 1
 * in 0-2), the data in WDATA0/1 or RDATA0/1, done when STATUS says so.
 *
 * The battery (Linux's pro1x-adc-battery.c, FreeBSD's
 * freebsd/patches/0015): the current from the PMI632's fuel gauge (QG at
 * 0x4800 of slave 2): LAST_BURST_AVG_I (0xc6, 16 bits, signed, 152588 nA a
 * step on the 5 A part, subtype 3, else 305176) held (DATA_CTL2 bit 0)
 * while it is read; the voltage from the ADC5 (0x3100), channel VBAT_SNS
 * (0x84, pre-scaled 1/3): DIG_PARAM to CONV_REQ (0x42-0x47) written,
 * STATUS1 (0x08) polled for the end of the conversion, DATA0/1 (0x50)
 * read, the code times 1875 mV over 0x70e4 (qcom-vadc-common.h). */
#include "pmic.h"

#define ARB_VERSION 0x0000
#define ARB_V5_MIN 0x50000000u
#define ARB_V7_MIN 0x70000000u
#define ARB_FEATURES 0x0004
#define ARB_APID_MAP(n) (0x900 + 4 * (n))
#define ARB_APID_OWNER(n) (0x700 + 4 * (n))
#define CH_CMD 0x00
#define CH_STATUS 0x08
#define ST_DONE 1u
#define ST_FAILURE 2u
#define ST_DENIED 4u
#define ST_DROPPED 8u
#define CH_WDATA0 0x10
#define CH_WDATA1 0x14
#define CH_RDATA0 0x18
#define CH_RDATA1 0x1c
#define OP_WRITEL 0
#define OP_READL 1
#define CMD(op, addr, len) ((u32)(op) << 27 | ((addr) & 0xffu) << 4 | (((len) - 1) & 7u))
#define MAX_APID 512
#define APID_VALID 0x8000

static struct spmi_hw hw;
static u16 ppid_apid[4096];
static u8 owner[MAX_APID];
static u32 napids, nours;

u32 spmi_init(const struct spmi_hw *h)
{
	hw = *h;
	mzero(ppid_apid, sizeof(ppid_apid));
	u32 ver = hw.rd(hw.core + ARB_VERSION);
	if (ver < ARB_V5_MIN || ver >= ARB_V7_MIN) return 0;
	napids = MIN(hw.rd(hw.core + ARB_FEATURES) & 0x7ffu, (u32)MAX_APID);
	nours = 0;
	for (u32 a = 0; a < napids; a++) {
		owner[a] = 0xff;
		if (ARB_APID_MAP(a) + 4 > hw.core_size) break;
		u32 v = hw.rd(hw.core + ARB_APID_MAP(a));
		if (!v) continue;
		u32 ppid = (v >> 8) & 0xfff;
		owner[a] = hw.rd(hw.cnfg + ARB_APID_OWNER(a)) & 7;
		if (owner[a] == hw.ee) nours++;
		u16 prev = ppid_apid[ppid];
		/* the first mapping, or the one this EE owns */
		if (!(prev & APID_VALID) || owner[a] == hw.ee) ppid_apid[ppid] = (u16)(a | APID_VALID);
	}
	return ver;
}

u32 spmi_channels(u32 *ours)
{
	if (ours) *ours = nours;
	return napids;
}

static int apid_of(u32 sid, u32 addr)
{
	u16 a = ppid_apid[(sid & 0xf) << 8 | (addr >> 8 & 0xff)];
	return (a & APID_VALID) ? (int)(a & ~APID_VALID) : -1;
}

static int wait_done(addr_t ch)
{
	for (int i = 0; i < 1000; i++) {
		u32 st = hw.rd(ch + CH_STATUS);
		if (st & ST_DONE) {
			if (st & ST_DENIED) return -3;
			return (st & (ST_FAILURE | ST_DROPPED)) ? -3 : 0;
		}
		if (hw.delay_us) hw.delay_us(1);
	}
	return -4;
}

int spmi_read(u32 sid, u32 addr, u8 *buf, u32 len)
{
	int a = apid_of(sid, addr);
	if (a < 0 || !len || len > 8 || !hw.rd) return -1;
	addr_t ch = hw.obsrvr + 0x10000u * hw.ee + 0x80u * (u32)a;
	hw.wr(ch + CH_CMD, CMD(OP_READL, addr, len));
	int r = wait_done(ch);
	if (r) return r;
	u32 d[2] = { hw.rd(ch + CH_RDATA0), len > 4 ? hw.rd(ch + CH_RDATA1) : 0 };
	for (u32 i = 0; i < len; i++) buf[i] = (u8)(d[i / 4] >> 8 * (i % 4));
	return 0;
}

int spmi_write(u32 sid, u32 addr, const u8 *buf, u32 len)
{
	int a = apid_of(sid, addr);
	if (a < 0 || !len || len > 8 || !hw.wr) return -1;
	if (owner[a] != hw.ee) return -2;
	addr_t ch = hw.chnls + 0x10000u * (u32)a;
	u32 d[2] = { 0, 0 };
	for (u32 i = 0; i < len; i++) d[i / 4] |= (u32)buf[i] << 8 * (i % 4);
	hw.wr(ch + CH_WDATA0, d[0]);
	if (len > 4) hw.wr(ch + CH_WDATA1, d[1]);
	hw.wr(ch + CH_CMD, CMD(OP_WRITEL, addr, len));
	return wait_done(ch);
}

/* --- the battery ----------------------------------------------------- */

#define QG_SUBTYPE 0x05
#define QG_SUBTYPE_5A 0x03
#define QG_DATA_CTL2 0x42
#define QG_HOLD 0x01
#define QG_LAST_BURST_AVG_I 0xc6
#define QG_I_RESET 0x8000
#define ADC5_STATUS1 0x08
#define ADC5_EOC_MASK 0x03
#define ADC5_EOC 0x01
#define ADC5_DIG_PARAM 0x42
#define ADC5_DATA0 0x50
#define ADC5_DATA_CHECK 0x8000
#define ADC5_MAX_CODE 0x7fff
#define ADC5_FULL_SCALE 0x70e4
#define ADC5_VREF_MV 1875
#define ADC5_DECIMATION 2        /* Linux's adc5 defaults */
#define ADC5_ABSOLUTE_CAL 2

int battery_init(struct battery *b)
{
	u8 st;
	b->ok = false;
	int r = spmi_read(b->sid, b->qg + QG_SUBTYPE, &st, 1);
	if (r) return r;
	b->na_per_lsb = st == QG_SUBTYPE_5A ? 152588 : 305176;
	if (!b->pre_num || !b->pre_den) b->pre_num = b->pre_den = 1;
	b->ok = true;
	return 0;
}

int battery_current(struct battery *b, s32 *ma)
{
	u8 ctl, raw[2];
	if (!b->ok) return -1;
	int r = spmi_read(b->sid, b->qg + QG_DATA_CTL2, &ctl, 1);
	if (r) return r;
	ctl |= QG_HOLD;
	if ((r = spmi_write(b->sid, b->qg + QG_DATA_CTL2, &ctl, 1))) return r;
	r = spmi_read(b->sid, b->qg + QG_LAST_BURST_AVG_I, raw, 2);
	ctl &= ~QG_HOLD;
	int r2 = spmi_write(b->sid, b->qg + QG_DATA_CTL2, &ctl, 1);
	if (r || r2) return r ? r : r2;
	u16 u = raw[1] << 8 | raw[0];
	if (u == QG_I_RESET) return -4;           /* no sample yet */
	*ma = (s32)((s64)(s16)u * b->na_per_lsb / 1000000);
	return 0;
}

/* one conversion of the battery's channel, as FreeBSD's adc_convert (the
 * registers DIG_PARAM, FAST_AVG_CTL, CH_SEL_CTL, DELAY_CTL, EN_CTL1 and
 * CONV_REQ in one write) */
int battery_voltage(struct battery *b, u32 *mv)
{
	u8 p[6], st = 0, d[2];
	if (!b->ok) return -1;
	int r = spmi_read(b->sid, b->adc + ADC5_DIG_PARAM, p, 6);
	if (r) return r;
	p[0] = (u8)((p[0] & ~0x7cu) | ADC5_ABSOLUTE_CAL << 4 | ADC5_DECIMATION << 2);
	p[1] &= ~0x07u;                           /* one sample */
	p[2] = (u8)b->chan;
	p[3] &= ~0x0fu;                           /* no settling delay */
	p[4] |= 0x80;                             /* enabled */
	p[5] |= 0x80;                             /* the conversion asked for */
	if ((r = spmi_write(b->sid, b->adc + ADC5_DIG_PARAM, p, 6))) return r;
	int i;
	for (i = 0; i < 400; i++) {
		if ((r = spmi_read(b->sid, b->adc + ADC5_STATUS1, &st, 1))) return r;
		if ((st & ADC5_EOC_MASK) == ADC5_EOC) break;
		if (hw.delay_us) hw.delay_us(264);
	}
	if (i == 400) return -4;
	if ((r = spmi_read(b->sid, b->adc + ADC5_DATA0, d, 2))) return r;
	u32 code = (u32)d[1] << 8 | d[0];
	if (code == ADC5_DATA_CHECK) return -3;
	if (code > ADC5_MAX_CODE) code = 0;
	u64 uv = (u64)code * ADC5_VREF_MV * 1000 / ADC5_FULL_SCALE;
	*mv = (u32)(uv * b->pre_den / b->pre_num / 1000);
	return 0;
}
