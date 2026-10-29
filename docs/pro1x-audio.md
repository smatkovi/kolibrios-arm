# The Pro1X's call audio

A call's sound does not pass through the application processor: the
modem hands the voice to the ADSP (the LPASS DSP), which runs it through
its voice processing and out to the codec DMA ports, and from there
through the codec to the earpiece, and from the microphone back. What
the apps processor does is set this path up: the codec's routes, the
ADSP's ports, and for each call a voice session on the ADSP. KolibriOS
takes the first two over from Linux at the kexec and does the third
itself (`kernel/modem/callaudio.c`, `kernel/modem/apr.c`).

## The hardware

From the phone's device tree (Maemo Leste's 7.1 kernel):

| part | what |
|---|---|
| ADSP | `remoteproc@a400000` (`qcom,sm6115-adsp-pas`); GLINK edge `lpass`, SMEM host 2, interrupt APCS IPC bit 8 |
| APR | `qcom,apr-v2` on the channel `apr_audio_svc`, domain 4 (ADSP; the apps processor is 5); services CORE 3, AFE 4, ASM 7, ADM 8, MVM 9, CVS 0xa, CVP 0xb. The edge also has `fastrpcglink-apps-dsp` |
| voice | `q6voice-dais` without `qcom,cvd-v2.3`: CVP is created with v2 |
| playback | RX macro → SoundWire (`a610000`) → WCD9370. The earpiece is the EAR output (RDAC3 from RX1) together with the HPHL stage |
| speakers | two AW87339 amplifiers on GENI I2C `4a84000` (the keyboard's bus): Spk0 at 0x59 (enable GPIO 85), Spk1 at 0x5a (GPIO 98), fed from HPHL and HPHR |
| microphone | DMIC3, digital, on the VA macro (VA DEC0 → `VA_CODEC_DMA_TX_0`); no WCD in the way |

## What Linux does

The UCM files on Leste (`/usr/share/alsa/ucm2/Pro1X`) come from the
NixOS port ([fxtec-pro1x-nixos](https://github.com/nyymi609/fxtec-pro1x-nixos),
`device/audio`), which reports working calls with its patched q6voice:

1. **The codec's routes**: the devices "Earpiece" and "Mic" (`HiFi.conf`,
   the same in `VoiceCall.conf`): MultiMedia1 → `RX_CODEC_DMA_RX_0`, the
   RX macro's RX0/RX1 mixers and interpolators, `HPHL_RDAC`, `RDAC3_MUX`
   RX1, `EAR_RDAC`, `EAR_PA Volume` 8, `HPHL Switch` last; DMIC3 →
   VA DEC0 → MultiMedia2.
2. **The verb "Voice Call"** adds the routes of the DSP's voice front end
   (`RX_CODEC_DMA_RX_0 Voice Mixer VoiceMMode1`,
   `VoiceMMode1 Capture Mixer VA_CODEC_DMA_TX_0`) and starts
   `pro1x-voice-call`, which
   - keeps a stream playing into the Voice_Call sink and one recording
     from its source (MultiMedia1/2) for as long as the profile is on, so
     that DAPM keeps the codec powered and the back ends (the ADSP's codec
     DMA ports) running;
   - starts `pro1x-voicepcm` when ModemManager lists a call (dialling,
     ringing either way, active, held) and stops it when the last call is
     over. It opens the hostless PCM VoiceMMode1 (hw:0,5) both ways
     (S16, mono, 8 kHz) and holds it. Its comment: the session "was found
     to work when it starts while a call exists and not when it was
     started beforehand".
3. **q6voice**, when both directions of VoiceMMode1 are open, starts the
   voice session over APR, one command at a time, each answered by
   `APR_BASIC_RSP_RESULT` (0x110E8: the opcode, a status) within 300 ms:

   | # | service | command | what |
   |---|---|---|---|
   | 1 | MVM | `CREATE_PASSIVE_CONTROL_SESSION` 0x110FF | the name "11C05000" (the modem's VSID); the session's handle is the reply's source port |
   | 2 | MVM | `SET_POLICY_DUAL_CONTROL` 0x11327 | 1 |
   | 3 | CVP | `CREATE_FULL_CONTROL_SESSION_V2` 0x112BF | direction RX+TX, tx port 0xB021 (`VA_CODEC_DMA_TX_0`) with topology NONE 0x10F70, rx port 0xB030 (`RX_CODEC_DMA_RX_0`) with RX_DEFAULT 0x10F77, profile NONE 0x1135E, mode EC_INT_MIXING 0x10F7C, EC reference port none; the handle again from the reply |
   | 4 | CVP | `ENABLE` 0x100C6 | |
   | 5 | MVM | `ATTACH_VOCPROC` 0x1123E | the CVP handle |
   | 6 | MVM | `START_VOICE` 0x11190 | |

   and at the end `STOP_VOICE`, `DETACH_VOCPROC`, CVP `DISABLE`, and
   `DESTROY_SESSION` 0x1003C for CVP, then MVM. The APR header is 20
   bytes (`hdr_field` 0x250 for commands), from domain 5 to domain 4,
   source and destination service the same, the path's id (6,
   VoiceMMode1) as the source port.
4. **The AFE** (q6afe, for the back ends when a stream starts, and for
   the macros' clocks): hardware votes `0x100F4` for the MACRO (3) and
   DCODEC (4) blocks, the reply `0x100F5` carrying the client handle; the
   clocks RX/TX MCLK and NPL (Q6 ids 0x30c-0x30f) at 19.2 MHz with
   `AFE_SVC_CMD_SET_PARAM` 0x100F3 (token 1024); the port's
   `CODEC_DMA_CONFIG` (module 0x1020C, parameter 0x102B8, 48 kHz, 16 bit,
   2 channels) with `AFE_PORT_CMD_SET_PARAM_V2` 0x100EF, then
   `DEVICE_START` 0x100E5 (`DEVICE_STOP` 0x100E6); the DAI ids as tokens
   (113 RX_0, 110 VA TX_0). `apr.c` has all of these too
   (`voicepath` with `ports`), but KolibriOS does not need them: the
   streams of the kexec leave the ports running.

On Leste itself `pro1x-voicepcm` is missing (and there is no
ModemManager), so Leste has no call audio yet either; the UCM files and
the kernel's VoiceMMode1 are there.

## Through a kexec

Neither the ADSP's driver (`qcom_q6v5_pas`) nor the SoundWire master, the
macros or the WCD937x driver has a shutdown hook: at `kexec -e` the ADSP
keeps running, and so do the codec's registers, the macros' clocks (they
are the ADSP's) and the AFE ports that were started. What is lost:

- **the voice session**, if there was one: it belongs to a call (above),
  so KolibriOS starts its own for each call;
- **Linux's APR client**: but the GLINK channel stays open on the ADSP's
  side. Unbinding APR before the kexec would close it, and also tear the
  sound card down (`snd_soc_unbind_card` → `snd_soc_dapm_shutdown`: the
  codec powered off), so it is left open and KolibriOS takes it over as
  it is.
- **the streams' data**: the ADSP plays Linux's buffers only as Linux
  hands them over (ASM write commands), so the playback stream simply
  runs dry (silence). The recording has a read or two queued: the ADSP
  fills those buffers once more, a few tens of milliseconds after the
  jump, in RAM that is KolibriOS's by then. A small risk, untested.

## KolibriOS's side

**Before the kexec**: `tools/pro1x-kexec.sh -m -a` suspends PulseAudio's
devices (as its user, `pactl suspend-sink/-source`), sets UCM's Earpiece
and Mic routes with `amixer` (not the verb: its `exec` wants
`pro1x-voice-call`), starts `aplay -D hw:N,0 … /dev/zero` (48 kHz, 2
channels) and `arecord -D hw:N,1 … /dev/null` (1 channel), checks after
a second that both run, and adds `kolibri.callaudio` to the command line.
The voice mixer routes are not needed: KolibriOS names the ports in its
CVP command itself.

**The ADSP's edge** (`kernel/platform/pro1x.c`): SMEM's partition with
host 2, the GLINK items 478-480 in it (`glink_init`), the interrupt bit
from the edge's `mboxes`.

**Linux's channel** (`callaudio.c`, `glink.c`): GLINK addresses a
channel by the sender's own id: Linux's data commands carry Linux's id
for `apr_audio_svc` (lcid), the ADSP's carry the ADSP's. Both rings still
hold their last commands behind their heads, and the two streams talk APR
up to the jump (ASM writes and their events), so `glink_scan_data` goes
back from each head in 8-byte steps and takes the newest whole data
command whose payload is an APR packet in the right direction (header
length 5 words, type 0-2, the length the chunk's, the same known service
at both ends, domain 5 → 4 in the tx ring, 4 → 5 in the rx ring). The
OPENs of boot (`glink_scan_open`) are the fallback. `glink_adopt` then
marks the channel open without an OPEN:
- the ADSP's receive intents are not known: a message without one asks
  for one (`RX_INTENT_REQ`), as for any channel;
- the ADSP sends into the intents Linux had advertised: their data is
  received through a bounce buffer and the intent given back with
  `RX_DONE_W_REUSE`; KolibriOS's own intents are numbered from 0x10000,
  clear of Linux's;
- what Linux left unread is received (and given back) before the first
  command.

Without the ids (no `-a`, or the rings hold no APR), calls have no sound
and the receive FIFO is only emptied now and then, so that the ADSP is
not blocked by Linux's full ring.

**The voice session** (`voicepath`, `ports` false): the six commands
above when a call exists (dialling, ringing either way: as
`pro1x-voice-call`), the five back when it is over, anew for another
call (a new id). If the start fails while the call is being set up, it
is tried once more when the call is answered (the modem's own session may
come only then). The log (`callaudio:`) has each outcome.

**The speaker**: on the button (or S) during a call, the AW87339 Spk0
gets its enable line and Linux's `aw87339_music_seq` (chip id 0x39 read
first); off again on the button or at the end of the call, and the next
call starts on the earpiece. The HPHL stage it is fed from is on already
(the earpiece's route). The earpiece is not muted meanwhile.

**The programs**: `KOS_MODEM_STATUS` has `audio` (`KOS_AUDIO_PREPARED`
when the sound can be had) and `audio_route`; `KOS_MODEM_ROUTE` (function
100, subfunction 9) switches. The phone app shows "Speaker" beside "End
call" (lit while on), or the hint to start with `pro1x-kexec.sh -a`.

## Tests

`make -C tests/audio` (with the sanitizers):
- the packets' sizes against Linux's packed structures, field by field;
- the sequences against a fake ADSP: with the ports 16 commands up and
  13 down, CVD 2.3 21, without the ports 6 and 5; a refused command
  (undone, then failed), no answer (300 ms, undone step by step), a stop
  while starting (the reply under way first, then down, idle), others'
  replies;
- the takeover against a GLINK edge in memory as Linux leaves it: the ids
  from the data and not from the OPENs of boot (which have other ids),
  fastrpc's newer data passed over, Linux's unread event received and
  its intent given back; a call: an intent asked for, the six commands
  on Linux's id, the replies into Linux's intent and given back; the end,
  a second call with a new id, a refused start tried again once when
  answered; the OPENs as fallback; no APR at all: no sound, the FIFO
  emptied;
- the speaker: the amplifier's sequence, only during a call, the
  earpiece again after it, no amplifier.

In QEMU (realview, the fake modem, no ADSP): the phone app's Speaker
button and S key during a call switch the amplifier (`callaudio: speaker
on/off` in the log, the button lit).

## Not done

- The codec itself: KolibriOS relies on Linux leaving the RX and VA
  macros, SoundWire and the WCD937x running; it writes none of their
  registers. Without `-a` there is no sound.
- On the speaker the earpiece plays along (EAR not muted), only Spk0
  (the left one), no volume or mute, no ring tone, no headset or
  Bluetooth.
- CVD 2.3 (create v3 with its parameters) is written but not used: the
  device tree does not ask for it.
- An ADSP restart; a second channel on the edge.
- **None of it has run on the device.**

## The device test

Needs the user at the phone (a reboot into KolibriOS, the watchdog back
into the boot menu), with a SIM. On Leste, as root:

```sh
cp build/arm64/kernel.bin /host/kolibri/kernel.bin
tools/pro1x-kexec.sh -n -m -a        # checks: kernel, EFS images, sound card, PulseAudio's user; no start
tools/pro1x-kexec.sh -m -a /host/kolibri/kernel.bin "kolibri.lifetime=300"
```

Then in the phone app: the "Speaker" button during a call, no hint about
`-a`. A call out: the other side in the earpiece, the microphone heard
there; Speaker (or S): the loudspeaker, again: the earpiece; ended. A
second call (the session anew), and a call in, answered. After the reset
back in Leste, `tools/pro1x-read.py` has the `pro1x: ADSP edge` and
`callaudio:` lines (the channel's ids, `voice: up`/`down`, or the status
of a refused command).
