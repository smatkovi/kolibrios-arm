/* KolibriOS for ARM: the WCN3990's copy engines (ce.h), after Linux's
 * ath10k ce.c (its _64 functions, rri_on_ddr and shadow_reg_support as
 * the WCN3990's hw_params have them) and snoc.c's tables. */
#include "ce.h"
#include "htc.h"

/* snoc.c, host_ce_config_wlan: the host's rings (CE_ATTR_FLAGS 0) */
const struct ce_attr ce_attr_wlan[CE_COUNT] = {
	{ 0, 16, 2048, 0, true },                  /* CE0: host -> target HTC control */
	{ 0, 0, 2048, 512, true },                 /* CE1: target -> host HTT + HTC control */
	{ 0, 0, 2048, 64, true },                  /* CE2: target -> host WMI */
	{ 0, 32, 2048, 0, true },                  /* CE3: host -> target WMI */
	{ CE_ATTR_DIS_INTR, 2048, 256, 0, true },  /* CE4: host -> target HTT */
	{ 0, 0, 512, 512, true },                  /* CE5: target -> host HTT (ipa_uc) */
	{ 0, 0, 0, 0, false },                     /* CE6: the target's own memcpy */
	{ 0, 2, 2048, 2, false },                  /* CE7: the diagnostic window */
	{ 0, 0, 2048, 128, false },                /* CE8: target -> uMC */
	{ 0, 0, 2048, 512, true },                 /* CE9: target -> host HTT */
	{ 0, 0, 2048, 512, true },                 /* CE10: target -> host HTT */
	{ 0, 0, 2048, 512, true },                 /* CE11: target -> host pktlog */
};

/* snoc.c, target_ce_config_wlan */
const struct ce_pipe_cfg ce_target_cfg[CE_COUNT] = {
	{ 0, PIPEDIR_OUT, 32, 2048, 0, 0 },
	{ 1, PIPEDIR_IN, 32, 2048, 0, 0 },
	{ 2, PIPEDIR_IN, 64, 2048, 0, 0 },
	{ 3, PIPEDIR_OUT, 32, 2048, 0, 0 },
	{ 4, PIPEDIR_OUT, 256, 256, CE_ATTR_DIS_INTR, 0 },
	{ 5, PIPEDIR_OUT, 1024, 64, CE_ATTR_DIS_INTR, 0 },
	{ 6, PIPEDIR_INOUT, 32, 16384, 0, 0 },
	{ 7, 4, 0, 0, CE_ATTR_DIS_INTR, 0 },
	{ 8, PIPEDIR_IN, 32, 2048, 0, 0 },
	{ 9, PIPEDIR_IN, 32, 2048, 0, 0 },
	{ 10, PIPEDIR_IN, 32, 2048, 0, 0 },
	{ 11, PIPEDIR_IN, 32, 2048, 0, 0 },
};

/* snoc.c, target_service_to_ce_map_wlan, with its terminating zeros:
 * ath10k_snoc_wlan_enable counts them in (num_ce_svc_pipe_cfg is the
 * table's size), so WLAN_CFG carries 21 entries */
const struct ce_svc_pipe ce_svc_map[] = {
	{ HTC_SVC_WMI_DATA_VO, PIPEDIR_OUT, 3 }, { HTC_SVC_WMI_DATA_VO, PIPEDIR_IN, 2 },
	{ HTC_SVC_WMI_DATA_BK, PIPEDIR_OUT, 3 }, { HTC_SVC_WMI_DATA_BK, PIPEDIR_IN, 2 },
	{ HTC_SVC_WMI_DATA_BE, PIPEDIR_OUT, 3 }, { HTC_SVC_WMI_DATA_BE, PIPEDIR_IN, 2 },
	{ HTC_SVC_WMI_DATA_VI, PIPEDIR_OUT, 3 }, { HTC_SVC_WMI_DATA_VI, PIPEDIR_IN, 2 },
	{ HTC_SVC_WMI_CONTROL, PIPEDIR_OUT, 3 }, { HTC_SVC_WMI_CONTROL, PIPEDIR_IN, 2 },
	{ HTC_SVC_RSVD_CTRL, PIPEDIR_OUT, 0 }, { HTC_SVC_RSVD_CTRL, PIPEDIR_IN, 2 },
	{ HTC_SVC_TEST_RAW_STREAMS, PIPEDIR_OUT, 0 }, { HTC_SVC_TEST_RAW_STREAMS, PIPEDIR_IN, 2 },
	{ HTC_SVC_HTT_DATA_MSG, PIPEDIR_OUT, 4 }, { HTC_SVC_HTT_DATA_MSG, PIPEDIR_IN, 1 },
	{ HTC_SVC_TEST_RAW_STREAMS, PIPEDIR_OUT, 5 },
	{ HTC_SVC_HTT_DATA2_MSG, PIPEDIR_IN, 9 },
	{ HTC_SVC_HTT_DATA3_MSG, PIPEDIR_IN, 10 },
	{ HTC_SVC_HTT_LOG_MSG, PIPEDIR_IN, 11 },
	{ 0, 0, 0 },
};
const u32 ce_svc_map_n = sizeof(ce_svc_map) / sizeof(ce_svc_map[0]);

/* snoc.c, target_shadow_reg_cfg_map: WCN3990_SRC_WR_IDX_OFFSET 0x3c,
 * WCN3990_DST_WR_IDX_OFFSET 0x40 */
const struct ce_shadow_cfg ce_shadow_map[] = {
	{ 0, CE_SR_WR_INDEX }, { 3, CE_SR_WR_INDEX }, { 4, CE_SR_WR_INDEX }, { 5, CE_SR_WR_INDEX },
	{ 7, CE_SR_WR_INDEX }, { 1, CE_DST_WR_INDEX }, { 2, CE_DST_WR_INDEX }, { 7, CE_DST_WR_INDEX },
	{ 8, CE_DST_WR_INDEX }, { 9, CE_DST_WR_INDEX }, { 10, CE_DST_WR_INDEX }, { 11, CE_DST_WR_INDEX },
};
const u32 ce_shadow_map_n = sizeof(ce_shadow_map) / sizeof(ce_shadow_map[0]);

int ce_service_pipes(u32 service_id, u32 *ul, u32 *dl)
{
	bool ul_set = false, dl_set = false;
	for (u32 i = 0; i < ce_svc_map_n; i++) {
		if (ce_svc_map[i].service_id != service_id) continue;
		if (ce_svc_map[i].pipedir == PIPEDIR_IN) {
			*dl = ce_svc_map[i].pipenum;
			dl_set = true;
		} else if (ce_svc_map[i].pipedir == PIPEDIR_OUT) {
			*ul = ce_svc_map[i].pipenum;
			ul_set = true;
		}
	}
	return ul_set && dl_set ? 0 : -1;
}

/* --- the rings --------------------------------------------------------- */

struct ring {
	u32 n, mask, sw, wr, hw;
	volatile u8 *d;                /* n descriptors of 16 bytes */
	u64 pa;
	void **ctx;
};

static struct {
	struct ring src, dst;
	u32 flags, src_max;
} ce[CE_COUNT];

static struct ce_hw hw;
static volatile u32 *rri;              /* the read indices, CE_COUNT words */
static u64 rri_pa;
/* the transfers' contexts: as many as the rings have entries */
static void *ctx_pool[16 + 512 + 64 + 32 + 2048 + 512 + 2 + 2 + 128 + 512 * 3];

static u32 pow2(u32 n)
{
	u32 p = 1;
	while (p < n) p <<= 1;
	return p;
}

#define DELTA(mask, from, to) ((u32)((int)(to) - (int)(from)) & (mask))

static u32 creg(u32 id, u32 off) { return hw.rd(CE_BASE(id) + off); }
static void cwreg(u32 id, u32 off, u32 v) { hw.wr(CE_BASE(id) + off, v); }
static void cmask(u32 id, u32 off, u32 clear, u32 set) { cwreg(id, off, (creg(id, off) & ~clear) | set); }

static void desc_put(struct ring *r, u32 i, u32 lo, u32 hi, u16 nbytes, u16 flags)
{
	volatile u8 *d = r->d + 16 * i;
	sm_wr32(d, lo);
	sm_wr32(d + 4, hi);
	sm_wr32(d + 8, (u32)nbytes | (u32)flags << 16);
	sm_wr32(d + 12, 0);
}

static u16 desc_nbytes(struct ring *r, u32 i)
{
	return (u16)sm_rd32(r->d + 16 * i + 8);
}

static void desc_clear_nbytes(struct ring *r, u32 i)
{
	volatile u8 *d = r->d + 16 * i + 8;
	sm_wr32(d, sm_rd32(d) & 0xffff0000u);
}

static bool ring_alloc(struct ring *r, u32 entries, void ***pool)
{
	r->n = pow2(entries);
	r->mask = r->n - 1;
	r->d = hw.dma_alloc(r->n * 16, &r->pa);
	if (!r->d) return false;
	r->ctx = *pool;
	*pool += r->n;
	return true;
}

/* the target's read index: of an engine without interrupts from the RRI
 * word in RAM, else its register (ath10k_ce_*_ring_read_index_get) */
static u32 src_read_index(u32 id)
{
	if (ce[id].flags & CE_ATTR_DIS_INTR) return rri[id] & 0xffff;
	return creg(id, CE_CURRENT_SRRI);
}

static u32 dst_read_index(u32 id)
{
	if (ce[id].flags & CE_ATTR_DIS_INTR) return rri[id] >> 16 & 0xffff;
	return creg(id, CE_CURRENT_DRRI);
}

int ce_init(const struct ce_hw *h)
{
	hw = *h;
	void **pool = ctx_pool;
	/* the read indices in RAM (ath10k_ce_alloc_rri) */
	rri = hw.dma_alloc(4 * CE_COUNT, &rri_pa);
	if (!rri) return -1;
	hw.wr(CE_RRI_LOW, (u32)rri_pa);
	hw.wr(CE_RRI_HIGH, (u32)(rri_pa >> 32) & 0x1f);
	for (u32 id = 0; id < CE_COUNT; id++) cmask(id, CE_CTRL1, 0, CE_CTRL1_RRI_UPD);
	for (u32 id = 0; id < CE_COUNT; id++) {
		const struct ce_attr *a = &ce_attr_wlan[id];
		ce[id].flags = a->flags;
		ce[id].src_max = a->src_max;
		if (a->src_n) {
			/* ath10k_ce_init_src_ring */
			struct ring *r = &ce[id].src;
			if (!ring_alloc(r, a->src_n, &pool)) return -1;
			r->sw = r->hw = src_read_index(id) & r->mask;
			r->wr = creg(id, CE_SR_WR_INDEX) & r->mask;
			cwreg(id, CE_SR_BA_LO, (u32)r->pa);
			cwreg(id, CE_SR_BA_HI, (u32)(r->pa >> 32) & 0x1f);
			cwreg(id, CE_SR_SIZE, r->n);
			cmask(id, CE_CTRL1, CE_CTRL1_DMAX | CE_CTRL1_SRC_SWAP, a->src_max & CE_CTRL1_DMAX);
			cmask(id, CE_SRC_WM, 0xffffffffu, r->n & 0xffff);    /* low mark 0, high mark n */
		}
		if (a->dst_n) {
			/* ath10k_ce_init_dest_ring */
			struct ring *r = &ce[id].dst;
			if (!ring_alloc(r, a->dst_n, &pool)) return -1;
			r->sw = dst_read_index(id) & r->mask;
			r->wr = creg(id, CE_DST_WR_INDEX) & r->mask;
			cwreg(id, CE_DR_BA_LO, (u32)r->pa);
			cmask(id, CE_DR_BA_HI, 0x1f, (u32)(r->pa >> 32) & 0x1f);
			cwreg(id, CE_DR_SIZE, r->n);
			cmask(id, CE_CTRL1, CE_CTRL1_DST_SWAP, 0);
			cmask(id, CE_DST_WM, 0xffffffffu, r->n & 0xffff);
		}
		/* ath10k_ce_per_engine_handler_adjust: copy-complete where
		 * Linux has a callback and the engine interrupts, watermarks off */
		bool cc = a->cb && !(a->flags & CE_ATTR_DIS_INTR);
		cmask(id, CE_HOST_IE, CE_IE_COPY_COMPLETE | CE_IE_WATERMARKS, cc ? CE_IE_COPY_COMPLETE : 0);
	}
	return 0;
}

int ce_send_sg(u32 id, u64 pa, u32 nbytes, u32 transfer_id, bool gather, void *ctx)
{
	if (id >= CE_COUNT || !ce[id].src.n) return -1;
	struct ring *r = &ce[id].src;
	/* rri_on_ddr: the target's read index from RAM for every engine */
	u32 sw = rri[id] & 0xffff;
	if ((int)DELTA(r->mask, r->wr, sw - 1) <= 0) return -1;
	u32 flags = transfer_id << 4 & 0xfff0;            /* CE_DESC_FLAGS_META_DATA (wcn3990_values) */
	/* _ath10k_ce_send_nolock_64: a gathered piece has CE_DESC_FLAGS_GATHER
	 * in its flags and, in the address's high word, the send flag
	 * (CE_SEND_FLAG_GATHER, 0x10000) and CE_WCN3990_DESC_FLAGS_GATHER */
	u32 hi = (u32)(pa >> 32) & 0x1f;
	if (gather) {
		flags |= 1;
		hi |= 0x00010000u | CE_GATHER;
	}
	desc_put(r, r->wr, (u32)pa, hi, (u16)nbytes, (u16)flags);
	r->ctx[r->wr] = ctx;
	r->wr = (r->wr + 1) & r->mask;
	/* the write index only after the last piece */
	if (!gather) {
		sm_barrier();
		hw.wr(CE_SHADOW_SR(id), r->wr);
	}
	return 0;
}

int ce_send(u32 id, u64 pa, u32 nbytes, u32 transfer_id, void *ctx)
{
	return ce_send_sg(id, pa, nbytes, transfer_id, false, ctx);
}

void ce_send_revert(u32 id)
{
	if (id >= CE_COUNT || !ce[id].src.n) return;
	struct ring *r = &ce[id].src;
	r->wr = (r->wr - 1) & r->mask;
	r->ctx[r->wr] = 0;
}

int ce_send_done(u32 id, void **ctx)
{
	if (id >= CE_COUNT || !ce[id].src.n) return -1;
	struct ring *r = &ce[id].src;
	if (r->hw == r->sw) r->hw = src_read_index(id) & r->mask;
	u32 rd = src_read_index(id) & r->mask;        /* rri_on_ddr: read again */
	if (rd == r->sw) return -1;
	if (ctx) *ctx = r->ctx[r->sw];
	r->ctx[r->sw] = 0;
	desc_clear_nbytes(r, r->sw);
	r->sw = (r->sw + 1) & r->mask;
	return 0;
}

int ce_rx_post(u32 id, u64 pa, void *ctx)
{
	if (id >= CE_COUNT || !ce[id].dst.n) return -1;
	struct ring *r = &ce[id].dst;
	if (DELTA(r->mask, r->wr, r->sw - 1) == 0) return -1;
	pa &= CE_ADDR_MASK;
	desc_put(r, r->wr, (u32)pa, (u32)(pa >> 32), 0, 0);
	r->ctx[r->wr] = ctx;
	r->wr = (r->wr + 1) & r->mask;
	sm_barrier();
	cwreg(id, CE_DST_WR_INDEX, r->wr);
	return 0;
}

int ce_recv(u32 id, void **ctx, u32 *nbytes)
{
	if (id >= CE_COUNT || !ce[id].dst.n) return -1;
	struct ring *r = &ce[id].dst;
	u16 n = desc_nbytes(r, r->sw);
	if (!n) return -1;                       /* not done (or the DRRI ahead of it) */
	sm_barrier();                            /* the buffer read after its length (dma_rmb) */
	desc_clear_nbytes(r, r->sw);
	if (nbytes) *nbytes = n;
	if (ctx) *ctx = r->ctx[r->sw];
	if (id != 5) r->ctx[r->sw] = 0;          /* CE5 reuses its contexts */
	r->sw = (r->sw + 1) & r->mask;
	return 0;
}

void ce_irq_ack(u32 id)
{
	if (id < CE_COUNT) cwreg(id, CE_HOST_IS, CE_IE_COPY_COMPLETE | CE_IE_WATERMARKS);
}

u32 ce_src_free(u32 id)
{
	if (id >= CE_COUNT || !ce[id].src.n) return 0;
	struct ring *r = &ce[id].src;
	return DELTA(r->mask, r->wr, (rri[id] & 0xffff) - 1);
}

u32 ce_dst_free(u32 id)
{
	if (id >= CE_COUNT || !ce[id].dst.n) return 0;
	struct ring *r = &ce[id].dst;
	return DELTA(r->mask, r->wr, r->sw - 1);
}

u64 ce_rri_pa(void)
{
	return rri_pa;
}

const struct ce_desc64 *ce_ring(u32 id, bool dst, u32 *n)
{
	struct ring *r = dst ? &ce[id].dst : &ce[id].src;
	if (n) *n = r->n;
	return (const struct ce_desc64 *)r->d;
}
