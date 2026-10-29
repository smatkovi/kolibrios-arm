/* KolibriOS for ARM: function 100, the modem (calls and SMS), the
 * structures the kernel and the programs share (docs/pro1x-modem.md).
 * Needs u8, u32 and s32 first (kos.h, kernel.h). */
#ifndef KOS_MODEM_H
#define KOS_MODEM_H

enum {
	KOS_MODEM_NONE,        /* no modem (no device tree entry, no fake one) */
	KOS_MODEM_STARTING,    /* the link (GLINK, QRTR) comes up */
	KOS_MODEM_SERVICES,    /* the modem's services set up */
	KOS_MODEM_READY,
	KOS_MODEM_FAILED,
};

/* registration: QMI NAS's, and 5 for roaming */
enum { KOS_REG_NONE, KOS_REG_HOME, KOS_REG_SEARCHING, KOS_REG_DENIED, KOS_REG_UNKNOWN, KOS_REG_ROAMING };

struct kos_modem {
	u32 state;             /* KOS_MODEM_* */
	u32 registration;      /* KOS_REG_* */
	s32 signal;            /* dBm, 0: not known */
	u32 online;            /* the modem's operating mode is online */
	u32 sms_count;         /* messages kept (1: index 0 ... count - 1) */
	u32 sms_unread;
	u32 sms_seq;           /* changes with every new or changed message */
	u32 call_seq;          /* changes with every change of the call */
	u32 fake;              /* a modem in RAM (QEMU), not a real one */
	char operator_name[32];
	char status[64];       /* what happens now, or the last error */
};

enum { KOS_SMS_IN, KOS_SMS_OUT };
enum { KOS_SMS_UNREAD, KOS_SMS_READ, KOS_SMS_SENDING, KOS_SMS_SENT, KOS_SMS_FAILED };
#define KOS_SMS_TEXT 1024

struct kos_sms {
	u32 id;                /* stays with the message */
	u32 time;              /* Unix time, 0: not known */
	u8 dir, state, pad[2];
	char number[32];
	char text[KOS_SMS_TEXT];   /* UTF-8 */
};

enum { KOS_CALL_NONE, KOS_CALL_DIALING, KOS_CALL_ALERTING, KOS_CALL_INCOMING, KOS_CALL_ACTIVE,
       KOS_CALL_HELD, KOS_CALL_ENDED };
enum { KOS_CALL_OUT = 1, KOS_CALL_IN = 2 };

struct kos_call {
	u32 id;                /* the modem's call id */
	u32 state;             /* KOS_CALL_* */
	u32 dir;               /* KOS_CALL_OUT or KOS_CALL_IN */
	u32 seconds;           /* talking for, KOS_CALL_ACTIVE */
	char number[32];
};

/* function 100's subfunctions (ebx) */
enum {
	KOS_MODEM_STATUS = 0,  /* ecx: struct kos_modem * */
	KOS_MODEM_SMS = 1,     /* ecx: index, edx: struct kos_sms *; eax 0, -1: none */
	KOS_MODEM_SEND = 2,    /* ecx: number, edx: text (UTF-8); eax 0, -1 */
	KOS_MODEM_DIAL = 3,    /* ecx: number */
	KOS_MODEM_ANSWER = 4,
	KOS_MODEM_HANGUP = 5,
	KOS_MODEM_CALL = 6,    /* ecx: struct kos_call * */
	KOS_MODEM_DELETE = 7,  /* ecx: index */
	KOS_MODEM_READ = 8,    /* ecx: index: marked read */
};

#endif
