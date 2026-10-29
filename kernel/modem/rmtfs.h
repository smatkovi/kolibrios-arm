/* KolibriOS for ARM: the remote file system service (QMI 14), what
 * linux-msm/rmtfs is on Linux: the modem's EFS partitions from RAM */
#ifndef RMTFS_H
#define RMTFS_H
#include "qmi.h"

#define RMTFS_PORT 14
#define RMTFS_SECTOR 512

struct rmtfs {
	struct qrtr *q;
	volatile u8 *mem;              /* the shared buffer (qcom,rmtfs-mem) */
	u64 mem_pa;
	u32 mem_size;
	struct { char name[16]; u8 *data; u32 size; } img[6];
	int nimg;
	struct { bool open; u32 node; int img; } caller[8];
	u32 opens, reads, writes, errors;
};

/* bundle: tools/mkefs.py's images (KOSEFS01), may be NULL */
void rmtfs_init(struct rmtfs *r, struct qrtr *q, volatile u8 *mem, u64 mem_pa, u32 mem_size,
		u8 *bundle, u32 bundle_size);
/* a client went away: its files closed */
void rmtfs_client_gone(struct rmtfs *r, u32 node, u32 port);
/* the bundle's images (the fake modem and the tests use it too) */
int rmtfs_parse_bundle(struct rmtfs *r, u8 *bundle, u32 size);

#endif
