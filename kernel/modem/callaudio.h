/* KolibriOS for ARM: the sound of a call on the Pro1X.
 *
 * Linux (Maemo Leste) leaves the codec side running at the kexec
 * (tools/pro1x-kexec.sh -a): the routes of UCM's "Earpiece" and "Mic"
 * devices (the RX macro, the WCD937x's EAR and HPHL stages, the VA
 * macro's DMIC3) and two streams held open through the jump, one played
 * into MultiMedia1, one recorded from MultiMedia2, so that DAPM keeps the
 * codec powered and the ADSP keeps the codec DMA ports RX_CODEC_DMA_RX_0
 * and VA_CODEC_DMA_TX_0 running. Nothing of it stops at a kexec (no
 * shutdown hooks). What KolibriOS does here (docs/pro1x-audio.md):
 *  - takes over Linux's APR channel to the ADSP (GLINK "apr_audio_svc",
 *    its ids found in what the rings still hold) and, while a call is up,
 *    runs the voice session on it as Linux's q6voice does when the
 *    VoiceMMode1 PCM opens: MVM (passive, the modem's session), CVP on the
 *    two ports, attached and started; stopped again when the call ends.
 *    The session is started per call: it was found to work only when
 *    started while the call exists;
 *  - the loudspeaker: the AW87339 amplifier fed from HPHL (Spk0, I2C
 *    0x59), on while a call is on the speaker, off otherwise.
 * Without that preparation (no kolibri.callaudio on the command line), or
 * when the channel's ids are not found, calls connect without sound. */
#ifndef CALLAUDIO_H
#define CALLAUDIO_H
#include "modem.h"
#include "glink.h"

enum { CALLAUDIO_NONE, CALLAUDIO_PREPARED };     /* the call's sound can be had */
enum { CALLAUDIO_EARPIECE, CALLAUDIO_SPEAKER };
/* the call, for the sound: none (or over), being set up (dialling,
 * ringing either way), talking (or held) */
enum { CALLAUDIO_CALL_NONE, CALLAUDIO_CALL_SETUP, CALLAUDIO_CALL_ACTIVE };

struct callaudio_hw {
	/* the amplifiers' I2C (GENI 4a84000) and their enable lines */
	bool (*i2c_write)(u8 addr, u8 reg, u8 val);
	bool (*i2c_read)(u8 addr, u8 reg, u8 *val);
	void (*gpio_set)(int pin, bool high);
	void (*delay_us)(u32 us);
	/* the ADSP's GLINK edge (SMEM host 2, glink_init done); NULL: none
	 * (QEMU's fake modem: only the speaker switch) */
	struct glink *adsp;
	bool prepared;                 /* kolibri.callaudio: pro1x-kexec.sh -a ran */
};

/* the speaker amplifiers: I2C address, enable GPIO (the device tree) */
#define AW87339_SPK0_ADDR 0x59
#define AW87339_SPK0_GPIO 85
#define AW87339_SPK1_ADDR 0x5a
#define AW87339_SPK1_GPIO 98

void callaudio_init(const struct callaudio_hw *hw);
/* every tick: the call (CALLAUDIO_CALL_*) and the modem's id for it */
void callaudio_poll(u32 ms, int call, u32 call_id);
/* the speaker for the call on or off (back to the earpiece); -1: no
 * prepared sound */
int callaudio_set_route(int route);
int callaudio_route(void);
int callaudio_state(void);
int callaudio_voice(void);             /* the voice session: VP_* (apr.h) */
u32 callaudio_drained(void);           /* bytes of the ADSP thrown away (no channel) */

/* the amplifier's sequence (Linux: sound/soc/codecs/aw87339.c) */
bool aw87339_on(const struct callaudio_hw *hw, u8 addr, int gpio);
void aw87339_off(const struct callaudio_hw *hw, u8 addr, int gpio);

/* APR from the apps processor (tx) or from the ADSP (rx): a data
 * command's payload that is one, for glink_scan_data */
bool callaudio_apr_from_apps(const u8 *p, u32 len);
bool callaudio_apr_from_adsp(const u8 *p, u32 len);

#endif
