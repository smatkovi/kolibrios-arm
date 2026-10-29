/* KolibriOS for ARM: the rmtfs service (linux-msm/rmtfs, qmi_rmtfs.qmi).
 *
 * The modem opens its file systems by path (OPEN: a caller id back), asks
 * for the shared buffer (ALLOC_BUFF: its physical address), and moves
 * 512-byte sectors between them and the buffer (RW_IOVEC: entries of
 * sector, physical address in the buffer, count; direction 0 reads from
 * the storage into the buffer, 1 writes). Here the storage is the images
 * pro1x-kexec.sh passed along; writes change them in RAM only, as Linux's
 * "rmtfs -r" keeps them in memory. */
#include "rmtfs.h"

enum { RM_OPEN = 1, RM_CLOSE = 2, RM_IOVEC = 3, RM_ALLOC = 4, RM_DEV_ERROR = 5 };

static const char *const paths[][2] = {
	{ "/boot/modem_fs1", "modemst1" }, { "/boot/modem_fs2", "modemst2" },
	{ "/boot/modem_fsc", "fsc" }, { "/boot/modem_fsg", "fsg" },
};

int rmtfs_parse_bundle(struct rmtfs *r, u8 *b, u32 size)
{
	r->nimg = 0;
	if (!b || size < 12 || get32(b) != 0x45534f4bu || get32(b + 4) != 0x31305346u) /* KOSEFS01 */
		return -1;
	u32 n = get32(b + 8);
	for (u32 i = 0; i < n && r->nimg < 6; i++) {
		if (12 + (i + 1) * 24 > size) return -1;
		const u8 *e = b + 12 + i * 24;
		u32 off = get32(e + 16), sz = get32(e + 20);
		if ((u64)off + sz > size) return -1;
		mcopy(r->img[r->nimg].name, e, 16);
		r->img[r->nimg].name[15] = 0;
		r->img[r->nimg].data = b + off;
		r->img[r->nimg].size = sz;
		r->nimg++;
	}
	return 0;
}

static int find_img(struct rmtfs *r, const char *path)
{
	for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		if (!mstreq(path, paths[i][0])) continue;
		for (int j = 0; j < r->nimg; j++)
			if (mstreq(r->img[j].name, paths[i][1])) return j;
	}
	return -1;
}

static struct qmi_buf resp;

static void reply(struct rmtfs *r, u32 node, u32 port)
{
	u32 n = qmi_end(&resp);
	qrtr_send(r->q, RMTFS_PORT, node, port, resp.d, n);
}

static bool caller_ok(struct rmtfs *r, u32 id, u32 node)
{
	return id >= 1 && id <= 8 && r->caller[id - 1].open && r->caller[id - 1].node == node;
}

static void rx(void *arg, u32 node, u32 port, const u8 *data, u32 len)
{
	struct rmtfs *r = arg;
	struct qmi_msg m;
	if (!qmi_parse(data, len, &m) || m.type != QMI_REQUEST) return;
	qmi_begin(&resp, QMI_RESPONSE, m.txn, m.id);
	u32 n;
	const u8 *v;
	switch (m.id) {
	case RM_OPEN: {
		char path[64];
		v = qmi_tlv(&m, 0x01, &n);
		mzero(path, sizeof(path));
		if (v) mcopy(path, v, MIN(n, (u32)sizeof(path) - 1));
		int img = find_img(r, path), id = 0;
		for (int i = 0; img >= 0 && i < 8 && !id; i++)
			if (!r->caller[i].open) {
				r->caller[i].open = true;
				r->caller[i].node = node;
				r->caller[i].img = img;
				id = i + 1;
			}
		r->opens++;
		mlog("rmtfs: open %s: %s\n", path, id ? r->img[img].name : "not there");
		if (!id) {
			qmi_put_result(&resp, 1, 1);
			r->errors++;
		} else {
			qmi_put_result(&resp, 0, 0);
			qmi_put_u32(&resp, 0x10, id);
		}
		break;
	}
	case RM_CLOSE:
		v = qmi_tlv(&m, 0x01, &n);
		if (v && n >= 4 && caller_ok(r, get32(v), node)) {
			r->caller[get32(v) - 1].open = false;
			qmi_put_result(&resp, 0, 0);
		} else {
			qmi_put_result(&resp, 1, 1);
		}
		break;
	case RM_IOVEC: {
		const u8 *id = qmi_tlv(&m, 0x01, &n), *dir = qmi_tlv(&m, 0x02, 0);
		u32 vn;
		const u8 *vec = qmi_tlv(&m, 0x03, &vn);
		bool ok = id && dir && vec && vn >= 1 && caller_ok(r, get32(id), node);
		u32 count = ok ? vec[0] : 0;
		if (ok && vn < 1 + 12 * count) ok = false;
		for (u32 i = 0; ok && i < count; i++) {
			const u8 *e = vec + 1 + 12 * i;
			u64 off = (u64)get32(e) * RMTFS_SECTOR;
			u64 pa = get32(e + 4);
			u32 bytes = get32(e + 8) * RMTFS_SECTOR;
			int img = r->caller[get32(id) - 1].img;
			if (pa < r->mem_pa || pa + bytes > r->mem_pa + r->mem_size) { ok = false; break; }
			volatile u8 *buf = r->mem + (pa - r->mem_pa);
			u8 *data = r->img[img].data;
			u32 isz = r->img[img].size;
			if (dir[0]) {                  /* write: buffer to the image */
				if (off + bytes > isz) { ok = false; break; }
				sm_copy_from(data + off, buf, bytes);
				r->writes++;
			} else {                       /* read: the image (zeros behind it) to the buffer */
				u32 have = off < isz ? MIN(bytes, (u32)(isz - off)) : 0;
				if (have) sm_copy_to(buf, data + off, have);
				for (u32 j = have; j < bytes; j++) buf[j] = 0;
				r->reads++;
			}
		}
		sm_barrier();
		if (!ok) r->errors++;
		qmi_put_result(&resp, ok ? 0 : 1, ok ? 0 : 1);
		break;
	}
	case RM_ALLOC: {
		v = qmi_tlv(&m, 0x02, &n);
		u32 want = v && n >= 4 ? get32(v) : 0;
		mlog("rmtfs: buffer of %u bytes asked for (%u there)\n", want, r->mem_size);
		if (!r->mem || want > r->mem_size) {
			qmi_put_result(&resp, 1, 1);
		} else {
			qmi_put_result(&resp, 0, 0);
			qmi_put_u64(&resp, 0x10, r->mem_pa);
		}
		break;
	}
	case RM_DEV_ERROR:
		qmi_put_result(&resp, 0, 0);
		qmi_put_u8(&resp, 0x10, 0);
		break;
	default:
		qmi_put_result(&resp, 1, 2);
		break;
	}
	reply(r, node, port);
}

void rmtfs_client_gone(struct rmtfs *r, u32 node, u32 port)
{
	(void)port;
	for (int i = 0; i < 8; i++)
		if (r->caller[i].open && r->caller[i].node == node) r->caller[i].open = false;
}

void rmtfs_init(struct rmtfs *r, struct qrtr *q, volatile u8 *mem, u64 mem_pa, u32 mem_size,
		u8 *bundle, u32 bundle_size)
{
	mzero(r, sizeof(*r));
	r->q = q;
	r->mem = mem;
	r->mem_pa = mem_pa;
	r->mem_size = mem_size;
	if (rmtfs_parse_bundle(r, bundle, bundle_size) < 0)
		mlog("rmtfs: no EFS images: the modem's file systems cannot be read\n");
	for (int i = 0; i < r->nimg; i++)
		mlog("rmtfs: %s, %u KiB\n", r->img[i].name, r->img[i].size >> 10);
	qrtr_bind(q, RMTFS_PORT, rx, r);
	qrtr_serve(q, QMI_SVC_RMTFS, 1, 0, RMTFS_PORT);
}
