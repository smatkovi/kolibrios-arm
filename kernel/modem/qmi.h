/* KolibriOS for ARM: QMI messages over QRTR: a 7-byte header (type,
 * transaction, message, length), then TLVs (type, 16-bit length, value) */
#ifndef QMI_H
#define QMI_H
#include "qrtr.h"

enum { QMI_REQUEST = 0, QMI_RESPONSE = 2, QMI_INDICATION = 4 };
#define QMI_HDR 7
#define QMI_TLV_RESULT 0x02

/* the services used here */
enum { QMI_SVC_DMS = 2, QMI_SVC_NAS = 3, QMI_SVC_WMS = 5, QMI_SVC_VOICE = 9, QMI_SVC_UIM = 11,
       QMI_SVC_RMTFS = 14 };

struct qmi_buf { u32 len; u8 d[1536]; };

void qmi_begin(struct qmi_buf *b, u8 type, u16 txn, u16 msg);
void qmi_put(struct qmi_buf *b, u8 t, const void *v, u32 n);
void qmi_put_u8(struct qmi_buf *b, u8 t, u8 v);
void qmi_put_u16(struct qmi_buf *b, u8 t, u16 v);
void qmi_put_u32(struct qmi_buf *b, u8 t, u32 v);
void qmi_put_u64(struct qmi_buf *b, u8 t, u64 v);
void qmi_put_result(struct qmi_buf *b, u16 result, u16 error);
/* the header's length filled in; the message's size */
u32 qmi_end(struct qmi_buf *b);

struct qmi_msg { u8 type; u16 txn, id; const u8 *tlvs; u32 tlen; };
bool qmi_parse(const u8 *p, u32 len, struct qmi_msg *m);
/* a TLV's value and length; NULL: not there */
const u8 *qmi_tlv(const struct qmi_msg *m, u8 t, u32 *n);
/* a response's result TLV: true for success; err the error code */
bool qmi_ok(const struct qmi_msg *m, u16 *err);

#endif
