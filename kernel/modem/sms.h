/* KolibriOS for ARM: SMS PDUs (3GPP TS 23.040, 23.038): SMS-DELIVER and
 * SMS-SUBMIT, the text in UTF-8 */
#ifndef SMS_H
#define SMS_H
#include "modem.h"

#define SMS_PDU_MAX 180        /* SMSC address and TPDU */
#define SMS_TEXT_MAX 680       /* UTF-8 of one part (70 UCS-2 characters up to 4 bytes) */

struct sms_part {
	char number[32];           /* the sender (DELIVER), the recipient (SUBMIT) */
	u32 time;                  /* the service centre's time stamp, Unix (UTC); 0: none */
	char text[SMS_TEXT_MAX];
	u16 ref;                   /* concatenation: reference, parts, this part (1..) */
	u8 total, seq;
	bool submit;               /* an SMS-SUBMIT (sent, stored on the modem) */
};

/* a PDU as the modem stores and delivers it (the SMSC address in front);
 * 0, or -1 for what is not a message (a status report, ...) */
int sms_decode(const u8 *pdu, u32 len, struct sms_part *p);
/* text to number as SMS-SUBMIT PDUs (the default SMSC), one per part
 * (concatenated beyond 160 GSM characters or 70 UCS-2 ones): out[i] of
 * len[i] bytes; the number of parts, 0 when it does not fit max */
int sms_encode(const char *number, const char *utf8, u8 ref, u8 out[][SMS_PDU_MAX], u32 *len, int max);

/* UTF-8 helpers */
u32 utf8_get(const char **s);          /* the next code point (0: end) */
int utf8_put(char *d, u32 cp);         /* bytes written (up to 4) */

#endif
