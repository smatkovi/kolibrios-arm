/* KolibriOS for ARM: the WCN3990's copy engines, HTC and WMI-TLV
 * (kernel/modem/ce.c, htc.c, wmi.c) on the PC against a simulated target
 * (make -C tests/wifi; docs/pro1x-network.md, stages 3 and 4).
 *
 * The target: the engines' register window in RAM, read and written as
 * the hardware does (a source ring's write index through its shadow
 * register, the read indices in the engine's registers and in the RRI
 * word in the host's memory, a destination ring's descriptors filled and
 * their lengths set), the host's memory reached by its addresses (35 bits,
 * above 16 GiB here, so that the high words count). On top of it the
 * firmware's side of HTC (READY, CONNECT_SERVICE_RESP, credits in a
 * trailer) and of WMI-TLV (SERVICE_READY, INIT decoded and checked,
 * READY with a MAC address). The host's side is what ath10k_snoc and
 * ath10k_core do with them, in a small loop here. */
#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/modem/ce.h"
#include "../../kernel/modem/htc.h"
#include "../../kernel/modem/wmi.h"

static int failed, passed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* --- the target's side: registers and memory ---------------------------- */

static u32 regs[0x250000 / 4];
static u32 shadow[CE_COUNT];            /* the source rings' write indices, as the shadow registers had them */
static int wrong_writes;

static u32 rd(u32 off)
{
	if (off >= sizeof(regs)) { wrong_writes++; return 0; }
	return regs[off / 4];
}

static void wr(u32 off, u32 v)
{
	if (off >= 0x32000 && off < 0x32000 + 4 * CE_COUNT) {
		shadow[(off - 0x32000) / 4] = v;
		return;
	}
	if (off >= sizeof(regs)) { wrong_writes++; return; }
	u32 id = (off - 0x240000) / 0x1000, r = off & 0xfff;
	if (off >= 0x240000 && off < CE_WRAPPER && r == CE_HOST_IS) {
		regs[off / 4] &= ~v;              /* write 1 to clear */
		return;
	}
	if (off >= 0x240000 && off < CE_WRAPPER && r == CE_SR_WR_INDEX && id < CE_COUNT) wrong_writes++;
	regs[off / 4] = v;
}

#define ARENA (4u << 20)
#define ARENA_PA 0x400000000ull          /* 16 GiB: the high word 4 */
static u8 *arena;
static u32 arena_used;

static void *dma_alloc(u32 size, u64 *pa)
{
	arena_used = (arena_used + 63) & ~63u;
	if (arena_used + size > ARENA) return NULL;
	void *p = arena + arena_used;
	*pa = ARENA_PA + arena_used;
	memset(p, 0, size);
	arena_used += size;
	return p;
}

static u8 *va(u64 pa, u32 len)
{
	if (pa < ARENA_PA || pa + len > ARENA_PA + ARENA) { wrong_writes++; return arena; }
	return arena + (pa - ARENA_PA);
}

static u32 creg(u32 id, u32 r) { return regs[(CE_BASE(id) + r) / 4]; }
static void cset(u32 id, u32 r, u32 v) { regs[(CE_BASE(id) + r) / 4] = v; }
static u64 ring_pa(u32 id, bool dst)
{
	return dst ? creg(id, CE_DR_BA_LO) | (u64)(creg(id, CE_DR_BA_HI) & 0x1f) << 32
		   : creg(id, CE_SR_BA_LO) | (u64)(creg(id, CE_SR_BA_HI) & 0x1f) << 32;
}

static u32 *rri_word(u32 id)
{
	u64 pa = regs[CE_RRI_LOW / 4] | (u64)(regs[CE_RRI_HIGH / 4] & 0x1f) << 32;
	return (u32 *)va(pa + 4 * id, 4);
}

/* the target's read index of a ring, in its register and in the RRI word */
static void set_srri(u32 id, u32 v)
{
	cset(id, CE_CURRENT_SRRI, v);
	u32 *w = rri_word(id);
	*w = (*w & 0xffff0000u) | v;
}

static void set_drri(u32 id, u32 v)
{
	cset(id, CE_CURRENT_DRRI, v);
	u32 *w = rri_word(id);
	*w = (*w & 0xffffu) | v << 16;
}

/* what the host sent: taken from a source ring (its descriptors, the
 * data they point at) */
struct sent { u32 ce, eid, len; u8 data[2048]; };
static struct sent inbox[64];
static int ninbox;

static void target_take(u32 id)
{
	u32 n = creg(id, CE_SR_SIZE), rdi = creg(id, CE_CURRENT_SRRI);
	if (!n) return;
	u8 *ring = va(ring_pa(id, false), n * 16);
	while (rdi != shadow[id]) {
		u8 *d = ring + 16 * rdi;
		u64 pa = get32(d) | (u64)(get32(d + 4) & 0x1f) << 32;
		u32 len = get16(d + 8), flags = get16(d + 10);
		if (ninbox < 64 && len <= 2048) {
			inbox[ninbox].ce = id;
			inbox[ninbox].eid = flags >> 4;
			inbox[ninbox].len = len;
			memcpy(inbox[ninbox].data, va(pa, len), len);
			ninbox++;
		}
		rdi = (rdi + 1) & (n - 1);
		set_srri(id, rdi);
	}
}

/* the target to the host: the next posted buffer of a destination ring */
static bool target_give(u32 id, const u8 *msg, u32 len)
{
	u32 n = creg(id, CE_DR_SIZE), drri = creg(id, CE_CURRENT_DRRI);
	if (!n || drri == creg(id, CE_DST_WR_INDEX)) return false;     /* no buffer posted */
	u8 *d = va(ring_pa(id, true), n * 16) + 16 * drri;
	u64 pa = get32(d) | (u64)(get32(d + 4) & 0x1f) << 32;
	memcpy(va(pa, len), msg, len);
	put16(d + 8, (u16)len);
	set_drri(id, (drri + 1) & (n - 1));
	cset(id, CE_HOST_IS, creg(id, CE_HOST_IS) | CE_IE_COPY_COMPLETE);
	return true;
}

/* --- the firmware's HTC and WMI ------------------------------------------- */

#define FW_CREDIT_SIZE 1792
#define FW_WMI_EID 1
#define FW_HTT_EID 2
static const u8 fw_mac[6] = { 0x00, 0x03, 0x7f, 0x12, 0x34, 0x56 };
static int fw_inits;
static u32 fw_init_chunks;

/* an HTC message from the firmware: on the pipe of the service's DL */
static void fw_send(u32 pipe, u8 eid, const u8 *payload, u16 len, const u8 *trailer, u8 tlen)
{
	u8 m[2048];
	htc_put_hdr(m, eid, (u16)(len + tlen), tlen ? HTC_FLAG_TRAILER_PRESENT : 0, 0);
	m[4] = tlen;
	m[5] = 0;
	memcpy(m + HTC_HDR_LEN, payload, len);
	if (tlen) memcpy(m + HTC_HDR_LEN + len, trailer, tlen);
	CHECK(target_give(pipe, m, HTC_HDR_LEN + len + tlen), "the target finds a buffer on CE%u", pipe);
}

static void fw_ready(void)
{
	u8 p[12] = { 0 };
	put16(p, HTC_MSG_READY);
	put16(p + 2, 3);                        /* credits */
	put16(p + 4, FW_CREDIT_SIZE);
	p[6] = 8;                               /* endpoints */
	p[8] = 1;                               /* HTC 2.1 */
	p[9] = 0;
	fw_send(2, 0, p, sizeof(p), NULL, 0);
}

static void fw_service_ready(void)
{
	u8 m[512], *p = m;
	memset(m, 0, sizeof(m));
	wmi_put_hdr(p, WMI_TLV_SERVICE_READY_EVENTID);
	p += WMI_HDR_LEN;
	wmi_put_tlv(p, WMI_TLV_TAG_STRUCT_SERVICE_READY_EVENT, WMI_SVC_RDY_LEN);
	u8 *e = p + 4;
	put32(e, 0x324103d6);                  /* fw_build_vers */
	put32(e + 4, WMI_TLV_ABI_VER0);
	put32(e + 8, WMI_TLV_ABI_VER1);
	put32(e + 12, WMI_TLV_ABI_VER_NS0);
	put32(e + 16, WMI_TLV_ABI_VER_NS1);
	put32(e + 28 + 8, 2);                  /* num_rf_chains */
	put32(e + 28 + 44, 2);                 /* num_mem_reqs */
	put32(e + 28 + 48, 64);                /* max_num_scan_chans */
	p += 4 + WMI_SVC_RDY_LEN;
	wmi_put_tlv(p, WMI_TLV_TAG_STRUCT_HAL_REG_CAPABILITIES, WMI_HAL_REG_LEN);
	put32(p + 4, 0x6a);                    /* eeprom_rd */
	put32(p + 4 + 20, 2312), put32(p + 4 + 24, 2732);
	put32(p + 4 + 28, 4900), put32(p + 4 + 32, 6100);
	p += 4 + WMI_HAL_REG_LEN;
	/* the service bitmap: 4 services a word; WOW (13): word 3 bit 1,
	 * RX_FULL_REORDER (65): word 16 bit 1 */
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_UINT32, 128);
	put32(p + 4 + 4 * 3, 1u << 1);
	put32(p + 4 + 4 * 16, 1u << 1);
	p += 4 + 128;
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_UINT32, 8);   /* the DBS modes: ignored */
	put32(p + 4, 0xffffffffu);
	p += 4 + 8;
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_STRUCT, 2 * (4 + WMI_MEM_REQ_LEN));
	p += 4;
	for (int i = 0; i < 2; i++) {
		wmi_put_tlv(p, WMI_TLV_TAG_STRUCT_WLAN_HOST_MEM_REQ, WMI_MEM_REQ_LEN);
		put32(p + 4, 7 + i);               /* req_id */
		put32(p + 8, 4096);                /* unit_size */
		put32(p + 12, 0);
		put32(p + 16, 2 + i);              /* num_units */
		p += 4 + WMI_MEM_REQ_LEN;
	}
	fw_send(2, FW_WMI_EID, m, (u16)(p - m), NULL, 0);
}

static void fw_wmi_ready(void)
{
	u8 m[64];
	memset(m, 0, sizeof(m));
	wmi_put_hdr(m, WMI_TLV_READY_EVENTID);
	wmi_put_tlv(m + 4, WMI_TLV_TAG_STRUCT_READY_EVENT, WMI_RDY_LEN);
	put32(m + 8, WMI_TLV_ABI_VER0);
	put32(m + 12, WMI_TLV_ABI_VER1);
	memcpy(m + 8 + 24, fw_mac, 6);
	put32(m + 8 + 32, 0);
	/* the INIT's credit back, in a trailer: CREDITS, 4 bytes: eid, 1 */
	u8 t[8] = { HTC_RECORD_CREDITS, 4, 0, 0, FW_WMI_EID, 1, 0, 0 };
	fw_send(2, FW_WMI_EID, m, 4 + 4 + WMI_RDY_LEN, t, sizeof(t));
}

/* INIT as the firmware reads it */
static int init_tlv(u16 tag, const u8 *v, u16 len, void *arg)
{
	int *step = arg;
	if (*step == 0) {
		CHECK(tag == WMI_TLV_TAG_STRUCT_INIT_CMD && len == WMI_INIT_LEN, "INIT: first the command (%u %u)", tag, len);
		CHECK(get32(v) == WMI_TLV_ABI_VER0 && get32(v + 4) == 53 && get32(v + 8) == 0x5f414351u,
		      "INIT: the ABI");
		fw_init_chunks = get32(v + 24);
	} else if (*step == 1) {
		CHECK(tag == WMI_TLV_TAG_STRUCT_RESOURCE_CONFIG && len == WMI_RES_CFG_LEN, "INIT: the resources (%u %u)", tag, len);
		CHECK(get32(v) == 4 && get32(v + 4) == 33 && get32(v + 8) == 4, "INIT: vdevs, peers, offload peers");
		CHECK(get32(v + 4 * 13) == 1 && get32(v + 4 * 9) == 0x64 && get32(v + 4 * 12) == 0x28,
		      "INIT: the decapsulation, the rx timeouts");
		CHECK(get32(v + 4 * 28) == 1056 && get32(v + 4 * 34) == 22 && get32(v + 4 * 43) == 1u << 9,
		      "INIT: msdu descriptors, wow filters, host capabilities (%u %u %#x)", get32(v + 4 * 28),
		      get32(v + 4 * 34), get32(v + 4 * 43));
	} else if (*step == 2) {
		CHECK(tag == WMI_TLV_TAG_ARRAY_STRUCT && len == fw_init_chunks * 20, "INIT: the chunks' array (%u)", len);
		for (u32 i = 0; i < fw_init_chunks && i * 20 + 20 <= len; i++) {
			const u8 *c = v + 20 * i;
			CHECK(get16(c + 2) == WMI_TLV_TAG_STRUCT_WLAN_HOST_MEMORY_CHUNK && get16(c) == 16, "INIT: a chunk's TLV");
			CHECK(get32(c + 4) == 7 + i && get32(c + 12) == 4096 * (2 + i) && get32(c + 16) == 0,
			      "INIT: chunk %u: id, size, below 4 GiB", i);
		}
	}
	(*step)++;
	return 0;
}

/* the firmware's turn: what came on CE0 (HTC control) and CE3 (WMI) */
static void target_run(void)
{
	target_take(0);
	target_take(3);
	for (int i = 0; i < ninbox; i++) {
		struct sent *s = &inbox[i];
		const u8 *h = s->data, *p = s->data + HTC_HDR_LEN;
		u16 plen = get16(h + 2);
		CHECK(h[0] == s->eid && plen + HTC_HDR_LEN == s->len, "the descriptor's transfer id is the HTC endpoint (%u %u)",
		      h[0], s->eid);
		if (s->ce == 0 && get16(p) == HTC_MSG_CONNECT_SERVICE) {
			u16 svc = get16(p + 2), flags = get16(p + 4);
			u8 r[8] = { 0 };
			put16(r, HTC_MSG_CONNECT_SERVICE_RESP);
			put16(r + 2, svc);
			r[4] = HTC_CONN_STATUS_SUCCESS;
			r[5] = svc == HTC_SVC_WMI_CONTROL ? FW_WMI_EID : FW_HTT_EID;
			put16(r + 6, FW_CREDIT_SIZE);
			if (svc == HTC_SVC_WMI_CONTROL)
				CHECK(flags == 1 << 8, "WMI control: one credit, credit flow (%#x)", flags);
			else
				CHECK(flags == HTC_CONN_FLAGS_DISABLE_CREDIT_FLOW_CTRL, "HTT: no credit flow (%#x)", flags);
			fw_send(2, 0, r, sizeof(r), NULL, 0);
		} else if (s->ce == 0 && get16(p) == HTC_MSG_SETUP_COMPLETE_EX) {
			CHECK(plen == 12, "SETUP_COMPLETE_EX: 12 bytes");
			fw_service_ready();
		} else if (s->ce == 3 && s->eid == FW_WMI_EID && get32(p) == WMI_TLV_INIT_CMDID) {
			CHECK(h[1] & HTC_FLAG_NEED_CREDIT_UPDATE, "WMI: a credit update asked for");
			int step = 0;
			CHECK(wmi_tlv_each(p + WMI_HDR_LEN, plen - WMI_HDR_LEN, init_tlv, &step) == 0 && step == 3,
			      "INIT: three TLVs");
			fw_inits++;
			fw_wmi_ready();
		} else {
			CHECK(0, "a message the firmware does not know: CE%u eid %u", s->ce, s->eid);
		}
	}
	ninbox = 0;
}

/* --- the host's side --------------------------------------------------- */

static struct htc htc;
static u8 *rxbuf[CE_COUNT][8];
static u64 rxpa[CE_COUNT][8];

static void post_rx(u32 ce, int n)
{
	for (int i = 0; i < n; i++) {
		if (!rxbuf[ce][i]) rxbuf[ce][i] = dma_alloc(2048, &rxpa[ce][i]);
		CHECK(ce_rx_post(ce, rxpa[ce][i], rxbuf[ce][i]) == 0, "a buffer posted on CE%u", ce);
	}
}

/* a message to an endpoint: header and payload in DMA memory, sent on its
 * pipe; 0, or -1 (no credit) */
static int host_send(u8 eid, const u8 *payload, u16 len)
{
	u64 pa;
	u8 *b = dma_alloc(HTC_HDR_LEN + len, &pa);
	memcpy(b + HTC_HDR_LEN, payload, len);
	int n = htc_prepare(&htc, eid, b, len);
	if (n < 0) return -1;
	CHECK(ce_send(htc.ep[eid].ul_pipe, pa, (u32)n, eid, b) == 0, "sent on CE%u", htc.ep[eid].ul_pipe);
	return 0;
}

/* the next message on CE2 (HTC control and WMI) */
static bool host_recv(struct htc_rx *m)
{
	void *ctx;
	u32 n;
	if (ce_recv(2, &ctx, &n)) return false;
	ce_irq_ack(2);
	CHECK(htc_receive(&htc, ctx, n, m) == 0, "an HTC message (%u bytes)", n);
	/* the buffer back to the target */
	for (int i = 0; i < 8; i++)
		if (rxbuf[2][i] == ctx) ce_rx_post(2, rxpa[2][i], ctx);
	return true;
}

int main(void)
{
	arena = calloc(1, ARENA);
	/* registers as Linux left them after its shutdown: the indices at 5 */
	for (u32 id = 0; id < CE_COUNT; id++) {
		cset(id, CE_SR_WR_INDEX, 5), cset(id, CE_CURRENT_SRRI, 5);
		cset(id, CE_DST_WR_INDEX, 5), cset(id, CE_CURRENT_DRRI, 5);
		cset(id, CE_CTRL1, 0x00060000u);   /* the byte swaps on */
		cset(id, CE_HOST_IE, CE_IE_WATERMARKS);
		shadow[id] = 5;
	}
	struct ce_hw hw = { rd, wr, dma_alloc };
	CHECK(ce_init(&hw) == 0, "the engines set up");
	/* the RRI words start at 5 too (the target writes them) */
	for (u32 id = 0; id < CE_COUNT; id++) *rri_word(id) = 5 | 5 << 16;
	CHECK(ce_rri_pa() >= ARENA_PA && regs[CE_RRI_LOW / 4] == (u32)ce_rri_pa() && regs[CE_RRI_HIGH / 4] == 4,
	      "the RRI word's address (%#x %#x)", regs[CE_RRI_LOW / 4], regs[CE_RRI_HIGH / 4]);
	CHECK(creg(0, CE_SR_SIZE) == 16 && creg(3, CE_SR_SIZE) == 32 && creg(4, CE_SR_SIZE) == 2048 &&
	      creg(7, CE_SR_SIZE) == 2, "the source rings' sizes");
	CHECK(creg(1, CE_DR_SIZE) == 512 && creg(2, CE_DR_SIZE) == 64 && creg(5, CE_DR_SIZE) == 512 &&
	      creg(8, CE_DR_SIZE) == 128 && creg(11, CE_DR_SIZE) == 512 && creg(6, CE_DR_SIZE) == 0,
	      "the destination rings' sizes");
	CHECK(creg(0, CE_SR_BA_HI) == 4 && creg(2, CE_DR_BA_HI) == 4 && (creg(0, CE_SR_BA_LO) & 63) == 0,
	      "the rings above 16 GiB: the high words");
	CHECK((creg(0, CE_CTRL1) & 0xffff) == 2048 && (creg(4, CE_CTRL1) & 0xffff) == 256 &&
	      (creg(0, CE_CTRL1) & CE_CTRL1_SRC_SWAP) == 0 && (creg(2, CE_CTRL1) & CE_CTRL1_DST_SWAP) == 0,
	      "CTRL1: the transfers' maximum, no byte swap (%#x)", creg(0, CE_CTRL1));
	CHECK((creg(6, CE_CTRL1) & CE_CTRL1_RRI_UPD) && (creg(11, CE_CTRL1) & CE_CTRL1_RRI_UPD),
	      "CTRL1: the read indices to RAM, every engine");
	CHECK(creg(0, CE_SRC_WM) == 16 && creg(2, CE_DST_WM) == 64, "the watermarks: low 0, high the size");
	CHECK(creg(0, CE_HOST_IE) == 1 && creg(2, CE_HOST_IE) == 1 && creg(4, CE_HOST_IE) == 0 &&
	      creg(7, CE_HOST_IE) == 0 && creg(8, CE_HOST_IE) == 0, "copy-complete interrupts: not CE4 (polled), CE6-8");
	u32 n;
	const struct ce_desc64 *r0 = ce_ring(0, false, &n);
	CHECK(r0 && n == 16, "CE0's ring");
	CHECK(ce_src_free(0) == 15 && ce_dst_free(2) == 63, "the rings' free entries (%u %u)", ce_src_free(0), ce_dst_free(2));

	u32 ul, dl;
	CHECK(ce_service_pipes(HTC_SVC_WMI_CONTROL, &ul, &dl) == 0 && ul == 3 && dl == 2, "WMI control: CE3 out, CE2 in");
	CHECK(ce_service_pipes(HTC_SVC_HTT_DATA_MSG, &ul, &dl) == 0 && ul == 4 && dl == 1, "HTT: CE4 out, CE1 in");
	CHECK(ce_service_pipes(HTC_SVC_RSVD_CTRL, &ul, &dl) == 0 && ul == 0 && dl == 2, "HTC control: CE0 out, CE2 in");
	CHECK(ce_service_pipes(HTC_SVC_HTT_LOG_MSG, &ul, &dl) == -1, "pktlog: in only");

	/* HTC: the target's READY on endpoint 0 */
	htc_init(&htc);
	post_rx(2, 8);
	post_rx(1, 4);
	fw_ready();
	struct htc_rx m;
	CHECK(host_recv(&m) && m.eid == 0, "READY on endpoint 0");
	CHECK(htc_ready(&htc, m.payload, m.len) == 0 && htc.credit_size == FW_CREDIT_SIZE && htc.credit_count == 3 &&
	      htc.version == 1 && htc.total_credits == 1, "READY: credits (the WCN3990's: one), their size, HTC 2.1");

	/* the services: WMI control, then HTT */
	u8 msg[16];
	u16 svcs[2] = { HTC_SVC_WMI_CONTROL, HTC_SVC_HTT_DATA_MSG };
	int eids[2] = { -1, -1 };
	for (int i = 0; i < 2; i++) {
		u32 l = htc_connect_msg(msg, svcs[i], htc_credit_alloc(&htc, svcs[i]));
		CHECK(host_send(0, msg, (u16)l) == 0, "CONNECT_SERVICE %x", svcs[i]);
		target_run();
		CHECK(host_recv(&m) && m.eid == 0, "its response");
		CHECK(htc_connect_resp(&htc, m.payload, m.len, svcs[i], &eids[i]) == 0, "connected");
	}
	CHECK(eids[0] == FW_WMI_EID && htc.ep[FW_WMI_EID].ul_pipe == 3 && htc.ep[FW_WMI_EID].dl_pipe == 2 &&
	      htc.ep[FW_WMI_EID].credits == 1 && htc.ep[FW_WMI_EID].credit_flow, "WMI: endpoint 1, CE3/CE2, one credit");
	CHECK(eids[1] == FW_HTT_EID && htc.ep[FW_HTT_EID].ul_pipe == 4 && !htc.ep[FW_HTT_EID].credit_flow,
	      "HTT: endpoint 2, CE4, no credit flow");
	/* the source ring's descriptors: CE0 from Linux's index 5 on */
	const u8 *d = (const u8 *)r0 + 16 * 5;
	CHECK(get32(d + 4) == 4 && get16(d + 8) == HTC_HDR_LEN + 8 && get16(d + 10) == 0, "CE0's descriptor 5: the high word, the length, endpoint 0");
	void *done;
	CHECK(ce_send_done(0, &done) == 0 && ce_send_done(0, &done) == 0 && ce_send_done(0, &done) == -1,
	      "both connects done (the read index in RAM)");

	/* SETUP_COMPLETE_EX, then the firmware's SERVICE_READY */
	u32 l = htc_setup_complete_msg(msg);
	CHECK(host_send(0, msg, (u16)l) == 0, "SETUP_COMPLETE_EX");
	target_run();
	CHECK(host_recv(&m) && m.eid == FW_WMI_EID, "an event on WMI's endpoint");
	CHECK(wmi_id(m.payload) == WMI_TLV_SERVICE_READY_EVENTID, "SERVICE_READY");
	struct wmi_svc_ready sr;
	CHECK(wmi_service_ready(m.payload + WMI_HDR_LEN, m.len - WMI_HDR_LEN, &sr) == 0, "SERVICE_READY decoded");
	CHECK(sr.fw_build == 0x324103d6 && sr.num_rf_chains == 2 && sr.num_mem_reqs == 2 && sr.max_num_scan_chans == 64 &&
	      sr.eeprom_rd == 0x6a && sr.low_2ghz_chan == 2312 && sr.high_5ghz_chan == 6100, "its fields");
	CHECK(sr.nreqs == 2 && sr.req[1].req_id == 8 && sr.req[1].unit_size == 4096 && sr.req[1].num_units == 3,
	      "the memory it wants");
	CHECK(sr.svc_map_len == 128 && wmi_service(&sr, WMI_TLV_SERVICE_WOW) && wmi_service(&sr, WMI_TLV_SERVICE_RX_FULL_REORDER) &&
	      !wmi_service(&sr, 0) && !wmi_service(&sr, 12) && !wmi_service(&sr, 600), "the services: WOW, RX_FULL_REORDER");

	/* INIT with that memory, in one credit */
	struct wmi_res_cfg cfg;
	wmi_init_defaults(&cfg, wmi_service(&sr, WMI_TLV_SERVICE_RX_FULL_REORDER),
			  wmi_service(&sr, WMI_TLV_SERVICE_WOW) ? 22 : 0, false);
	struct wmi_mem_chunk ch[WMI_MAX_MEM_REQS];
	for (int i = 0; i < sr.nreqs; i++) {
		ch[i].req_id = sr.req[i].req_id;
		ch[i].size = sr.req[i].unit_size * sr.req[i].num_units;
		ch[i].pa = 0x80000000u + 0x10000u * (u32)i;
	}
	u8 init[512];
	int il = wmi_init_cmd(init, sizeof(init), &cfg, ch, sr.nreqs);
	CHECK(il == 4 + 4 + 28 + 4 + 176 + 4 + 2 * 20, "INIT's length (%d)", il);
	CHECK(wmi_init_cmd(init, 100, &cfg, ch, sr.nreqs) == -1, "INIT: too small a buffer");
	CHECK(host_send(FW_WMI_EID, init, (u16)il) == 0, "INIT sent");
	CHECK(htc.ep[FW_WMI_EID].credits == 0 && host_send(FW_WMI_EID, init, (u16)il) == -1, "no credit left: refused");
	target_run();
	CHECK(fw_inits == 1, "the firmware got INIT once");
	CHECK(host_recv(&m) && m.eid == FW_WMI_EID && wmi_id(m.payload) == WMI_TLV_READY_EVENTID, "READY");
	struct wmi_ready rdy;
	CHECK(wmi_ready(m.payload + WMI_HDR_LEN, m.len - WMI_HDR_LEN, &rdy) == 0 && !memcmp(rdy.mac, fw_mac, 6) &&
	      rdy.status == 0 && rdy.abi_version == 53, "READY: the MAC address %02x:%02x:%02x:%02x:%02x:%02x", rdy.mac[0],
	      rdy.mac[1], rdy.mac[2], rdy.mac[3], rdy.mac[4], rdy.mac[5]);
	CHECK(htc.ep[FW_WMI_EID].credits == 1 && m.len == 4 + 4 + WMI_RDY_LEN, "the trailer's credit back, the trailer cut off");

	/* the TLV walk refuses what does not fit */
	u8 bad[16];
	wmi_put_tlv(bad, WMI_TLV_TAG_STRUCT_READY_EVENT, 8);
	CHECK(wmi_tlv_each(bad, 12, init_tlv, &(int){ 9 }) == -1, "a READY shorter than its structure");
	wmi_put_tlv(bad, WMI_TLV_TAG_ARRAY_BYTE, 40);
	CHECK(wmi_tlv_each(bad, 16, init_tlv, &(int){ 9 }) == -1, "a TLV longer than the message");
	put32(bad, 0);
	CHECK(htc_receive(&htc, (u8[8]){ 7, 0, 4, 0 }, 8, &m) == -1, "HTC: an endpoint not connected");

	/* the rings wrap: CE0 has 16 entries, 15 usable. Its free entries
	 * count from the target's read index (as Linux with rri_on_ddr), so
	 * the completions are taken first (SETUP_COMPLETE_EX's) */
	CHECK(ce_send_done(0, &done) == 0 && ce_send_done(0, &done) == -1, "SETUP_COMPLETE_EX done");
	CHECK(ce_send_done(3, &done) == 0 && ce_send_done(3, &done) == -1, "INIT done");
	int sent = 0;
	for (int i = 0; i < 20; i++) {
		u64 pa;
		u8 *b = dma_alloc(16, &pa);
		if (ce_send(0, pa, 16, 0, b) == 0) sent++;
	}
	CHECK(sent == 15 && ce_src_free(0) == 0, "CE0 full after 15 (%d)", sent);
	target_take(0);
	ninbox = 0;
	int back = 0;
	while (ce_send_done(0, &done) == 0) back++;
	CHECK(back == 15 && ce_src_free(0) == 15, "all 15 back, across the ring's end (%d)", back);
	CHECK(wrong_writes == 0, "no write to a source ring's own write index, none outside (%d)", wrong_writes);

	printf("%d passed, %d failed\n", passed, failed);
	return failed != 0;
}
