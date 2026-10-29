/* KolibriOS for ARM: QMI messages (the encoding of Linux's
 * soc/qcom/qmi_encdec.c and libqmi, without the generated tables: the
 * TLVs are built and read by hand where they are used) */
#include "qmi.h"

void qmi_begin(struct qmi_buf *b, u8 type, u16 txn, u16 msg)
{
	b->d[0] = type;
	put16(b->d + 1, txn);
	put16(b->d + 3, msg);
	put16(b->d + 5, 0);
	b->len = QMI_HDR;
}

void qmi_put(struct qmi_buf *b, u8 t, const void *v, u32 n)
{
	if (b->len + 3 + n > sizeof(b->d)) return;
	b->d[b->len] = t;
	put16(b->d + b->len + 1, n);
	mcopy(b->d + b->len + 3, v, n);
	b->len += 3 + n;
}

void qmi_put_u8(struct qmi_buf *b, u8 t, u8 v) { qmi_put(b, t, &v, 1); }
void qmi_put_u16(struct qmi_buf *b, u8 t, u16 v) { u8 x[2]; put16(x, v); qmi_put(b, t, x, 2); }
void qmi_put_u32(struct qmi_buf *b, u8 t, u32 v) { u8 x[4]; put32(x, v); qmi_put(b, t, x, 4); }
void qmi_put_u64(struct qmi_buf *b, u8 t, u64 v) { u8 x[8]; put64(x, v); qmi_put(b, t, x, 8); }

void qmi_put_result(struct qmi_buf *b, u16 result, u16 error)
{
	u8 x[4];
	put16(x, result);
	put16(x + 2, error);
	qmi_put(b, QMI_TLV_RESULT, x, 4);
}

u32 qmi_end(struct qmi_buf *b)
{
	put16(b->d + 5, b->len - QMI_HDR);
	return b->len;
}

bool qmi_parse(const u8 *p, u32 len, struct qmi_msg *m)
{
	if (len < QMI_HDR) return false;
	m->type = p[0];
	m->txn = get16(p + 1);
	m->id = get16(p + 3);
	u32 n = get16(p + 5);
	if (n > len - QMI_HDR) return false;
	m->tlvs = p + QMI_HDR;
	m->tlen = n;
	return true;
}

const u8 *qmi_tlv(const struct qmi_msg *m, u8 t, u32 *n)
{
	for (u32 o = 0; o + 3 <= m->tlen; ) {
		u32 l = get16(m->tlvs + o + 1);
		if (o + 3 + l > m->tlen) return 0;
		if (m->tlvs[o] == t) {
			if (n) *n = l;
			return m->tlvs + o + 3;
		}
		o += 3 + l;
	}
	return 0;
}

bool qmi_ok(const struct qmi_msg *m, u16 *err)
{
	u32 n;
	const u8 *r = qmi_tlv(m, QMI_TLV_RESULT, &n);
	if (!r || n < 4) {
		if (err) *err = 0xffff;
		return false;
	}
	if (err) *err = get16(r + 2);
	return get16(r) == 0;
}
