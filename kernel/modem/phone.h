/* KolibriOS for ARM: the modem for the programs: its link, its services,
 * the messages and the call (function 100) */
#ifndef PHONE_H
#define PHONE_H
#include "rmtfs.h"
#include "sms.h"
#include "../../lib/kos_modem.h"

/* what the platform gives (kernel/platform/pro1x.c, the fake modem) */
struct modem_hw {
	volatile u8 *smem;
	u32 smem_size;
	void (*kick)(void);            /* the modem's GLINK interrupt */
	volatile u8 *rmtfs_mem;        /* its shared buffer, mapped */
	u64 rmtfs_pa;
	u32 rmtfs_size;
	u8 *efs;                       /* tools/mkefs.py's bundle (the initrd), or NULL */
	u32 efs_size;
	u32 (*clock)(void);            /* Unix time now, 0: not known */
	void (*set_clock)(u32 t);      /* the network's time (Unix) */
	bool fake;
	bool wlfw;                     /* talk to the WLAN firmware's service (wlfw.c) */
	bool adopt_ipcrtr;             /* Linux's IPCRTR channel left open: taken over */
};

void modem_start(const struct modem_hw *hw);
/* the work: every tick, ms the time since boot */
void modem_poll(u32 ms);
bool modem_present(void);
bool modem_ipcrtr_adopted(void);  /* Linux's channel taken over (tests) */

void modem_status(struct kos_modem *m);
int modem_sms(u32 index, struct kos_sms *s);
int modem_send(const char *number, const char *text);
int modem_dial(const char *number);
int modem_answer(void);
int modem_hangup(void);
void modem_call(struct kos_call *c);
int modem_delete(u32 index);
int modem_mark_read(u32 index);

#endif
