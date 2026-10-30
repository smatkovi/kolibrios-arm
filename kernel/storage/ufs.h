/* KolibriOS for ARM: the UFS host controller's driver (ufs.c), what the
 * platform hands it */
#ifndef UFS_H
#define UFS_H

struct ufs_hw {
	void *ctx;
	u32 (*rd)(void *ctx, u32 off);            /* its registers */
	void (*wr)(void *ctx, u32 off, u32 v);
	u64 (*now_us)(void);                      /* a clock that runs with interrupts off */
	void (*barrier)(void);                    /* memory written before the doorbell, read after */
	struct dma_mem dma;                       /* at least 264 KiB, uncached */
	bool keep_link;                           /* the Pro1X: the link Linux left up */
	bool writable;                            /* the write entry given to the block layer (its fence) */
};

/* the controller taken over (or brought up), its LUNs added as disks;
 * the number of them */
int ufs_start(const struct ufs_hw *hw);
/* tests: the controller left running as Linux leaves it (QEMU's model) */
bool ufs_leave_running(const struct ufs_hw *hw);

#endif
