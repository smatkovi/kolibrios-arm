/* KolibriOS for ARM: the WCN3990's copy engines (ce.c): the rings in RAM
 * through which the host and the WLAN firmware pass HTC's messages
 * (docs/pro1x-network.md, stage 3). After Linux's ath10k: ce.c (the 64-bit
 * descriptors, ce_desc_64), hw.c (wcn3990_regs, wcn3990_ce_regs, the
 * engines' register map), snoc.c (host_ce_config_wlan: each engine's
 * rings; target_ce_config_wlan and the service map for WLFW's WLAN_CFG).
 *
 * Twelve engines at 0x240000 + 0x1000 * id of the Wi-Fi block's register
 * window (the device tree's membase, 0xc800000, 8 MiB). An engine has a
 * source ring (host -> target) and/or a destination ring (target ->
 * host); a descriptor is 16 bytes: the buffer's address (35 bits; the
 * high word also carries flags: gather in bit 31), its length, the
 * transfer id (HTC's endpoint) in flags bits 4-15. The host's write index
 * of a source ring goes through a shadow register (0x32000 + 4 * id:
 * shadow_reg_support), of a destination ring to the engine's DST_WR_INDEX;
 * the target's read indices are the engine's CURRENT_SRRI/DRRI, and for
 * every engine in a word of RAM the target writes (rri_on_ddr: the source
 * ring's in bits 0-15, the destination's in 16-31). */
#ifndef CE_H
#define CE_H
#include "modem.h"

#define CE_COUNT 12
#define CE_BASE(id) (0x240000u + 0x1000u * (id))
#define CE_WRAPPER 0x24c000u

/* the engine's registers (wcn3990_ce_regs) */
#define CE_SR_BA_LO 0x00
#define CE_SR_BA_HI 0x04
#define CE_SR_SIZE 0x08
#define CE_DR_BA_LO 0x0c
#define CE_DR_BA_HI 0x10
#define CE_DR_SIZE 0x14
#define CE_CTRL1 0x18
#define CE_HOST_IE 0x2c
#define CE_HOST_IS 0x30
#define CE_MISC_IE 0x34
#define CE_SR_WR_INDEX 0x3c
#define CE_DST_WR_INDEX 0x40
#define CE_CURRENT_SRRI 0x44
#define CE_CURRENT_DRRI 0x48
#define CE_SRC_WM 0x4c
#define CE_DST_WM 0x50
#define CE_RRI_LOW (CE_WRAPPER + 4)
#define CE_RRI_HIGH (CE_WRAPPER + 8)
#define CE_CTRL1_DMAX 0xffffu          /* the source ring's largest transfer */
#define CE_CTRL1_SRC_SWAP (1u << 17)
#define CE_CTRL1_DST_SWAP (1u << 18)
#define CE_CTRL1_RRI_UPD (1u << 19)    /* the read indices to RAM (ce_rri) */
#define CE_IE_COPY_COMPLETE 1u
#define CE_IE_WATERMARKS 0x1eu
#define CE_ATTR_DIS_INTR 8u            /* (ce.h's CE_ATTR_DIS_INTR) */
#define CE_ADDR_MASK ((1ull << 35) - 1)
#define CE_GATHER (1u << 31)
#define CE_SHADOW_SR(id) (0x32000u + 4u * (id))   /* ce.c's shadow_sr_wr_ind_addr */

struct ce_desc64 {
	u64 addr;
	u16 nbytes;
	u16 flags;
	u32 hash;
};

/* the register window and memory the target reaches: rd/wr at an offset
 * of membase; dma_alloc: zeroed memory aligned to 64 bytes, its address
 * for the target in *pa (below 2^35), 0 when there is none */
struct ce_hw {
	u32 (*rd)(u32 off);
	void (*wr)(u32 off, u32 v);
	void *(*dma_alloc)(u32 size, u64 *pa);
};

/* an engine's host-side setup (snoc.c's host_ce_config_wlan) */
struct ce_attr {
	u32 flags, src_n, src_max, dst_n;
	bool cb;                       /* Linux has a callback on it: its copy-complete interrupt on */
};
extern const struct ce_attr ce_attr_wlan[CE_COUNT];

/* the target's (target_ce_config_wlan, sent in WLFW's WLAN_CFG) */
struct ce_pipe_cfg {
	u32 pipenum, pipedir, nentries, nbytes_max, flags, reserved;
};
extern const struct ce_pipe_cfg ce_target_cfg[CE_COUNT];
#define PIPEDIR_IN 1                   /* target -> host */
#define PIPEDIR_OUT 2                  /* host -> target */
#define PIPEDIR_INOUT 3

/* the services' pipes (target_service_to_ce_map_wlan) */
struct ce_svc_pipe {
	u32 service_id, pipedir, pipenum;
};
extern const struct ce_svc_pipe ce_svc_map[];
extern const u32 ce_svc_map_n;
/* the shadow registers the target keeps (target_shadow_reg_cfg_map) */
struct ce_shadow_cfg {
	u16 ce_id, reg_offset;
};
extern const struct ce_shadow_cfg ce_shadow_map[];
extern const u32 ce_shadow_map_n;

/* a service's pipes, as ath10k_snoc_hif_map_service_to_pipe: 0, or -1 */
int ce_service_pipes(u32 service_id, u32 *ul, u32 *dl);

/* the rings allocated and the engines programmed (ath10k_ce_alloc_pipe,
 * ath10k_ce_alloc_rri, ath10k_ce_init_pipe, ath10k_ce_enable_interrupts):
 * 0, or -1 (no memory). The indices start where the engine's registers
 * say (after a kexec: Linux's). On the phone after WLFW's WLAN_CFG and
 * WLAN_MODE, as ath10k_snoc_hif_power_up orders it */
int ce_init(const struct ce_hw *hw);
/* a buffer to the target: 0, -1 (the ring full or none). The free
 * entries count from the target's read index in RAM (rri_on_ddr, as
 * Linux's _ath10k_ce_send_nolock_64), so ce_send_done must have taken the
 * completions before the ring goes round once more */
int ce_send(u32 ce, u64 pa, u32 nbytes, u32 transfer_id, void *ctx);
/* the next sent buffer the target is done with: 0 (*ctx), -1 (none yet) */
int ce_send_done(u32 ce, void **ctx);
/* an empty buffer for the target to fill: 0, -1 (the ring full) */
int ce_rx_post(u32 ce, u64 pa, void *ctx);
/* the next filled one: 0 (*ctx, *nbytes), -1 (none yet) */
int ce_recv(u32 ce, void **ctx, u32 *nbytes);
/* the engine's interrupt status cleared (ath10k_ce_per_engine_service) */
void ce_irq_ack(u32 ce);
/* the free entries of a source ring, of a destination ring */
u32 ce_src_free(u32 ce);
u32 ce_dst_free(u32 ce);
/* the RRI word's address for the target (tests), the rings' (tests) */
u64 ce_rri_pa(void);
const struct ce_desc64 *ce_ring(u32 ce, bool dst, u32 *n);

#endif
