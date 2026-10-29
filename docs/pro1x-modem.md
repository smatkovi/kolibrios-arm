# The Pro1X's modem: calls and SMS

KolibriOS talks to the Pro1X's modem (the SM6115's MPSS, a Hexagon DSP
running Qualcomm's firmware) the way Linux does: QMI messages over QRTR
over GLINK, whose FIFOs are in the shared memory (SMEM). This document is
what Linux does on the phone today (Maemo Leste, kernel 7.1), what
KolibriOS does instead, and what is not done yet.

## What Linux does

| Piece | Linux on the Pro1X |
| --- | --- |
| the modem's firmware | `qcom_q6v5_pas` (`remoteproc@6080000`, `qcom,sm6115-mpss-pas`) loads `qcom/sm6115/Fxtec/QX1050/modem.mbn` into its carveout (`memory@4ab00000`, 105 MiB) and starts it through TrustZone (SCM PAS calls); `rmtfs -s` asks for the start once its service is up |
| shared memory | `memory@46000000`, 2 MiB (`qcom,smem`): a table of contents, a partition per pair of processors (the apps processor is host 0, the modem host 1) |
| the link | GLINK over SMEM (`glink-edge`, label `mpss`, `qcom,remote-pid = 1`): two ring buffers, SMEM items 479 (to the modem) and 480 (from it), their indices in item 478; the modem's interrupt is SPI 68 (GIC INTID 100), the apps processor's kick bit 12 of the APCS IPC register (`mailbox@f111000` + 8) |
| channels | `IPCRTR` (QRTR) and others the modem opens (`DATA*`, `DS`, `apr_voice_svc`, `glink_ssr`, ...) |
| routing | QRTR (`net/qrtr`, its name service in the kernel): the modem is node 0, the apps processor node 1, the ADSP 5, the CDSP 10 |
| services the apps processor offers | rmtfs (QMI service 14, port 14: the modem's file systems), the service registry locator (`pd-mapper`, 64), TFTP (`tqftpserv`, 4096), IPA control (49) |
| services the modem offers | among 40: DMS (2, port 51), NAS (3, 39), WMS (5, 55: SMS), VOICE (9, 38), UIM (11, 48), ... (`qrtr-lookup` on the phone) |
| telephony | `ofonod` with its QMI driver |
| the modem's file systems | `rmtfs -r -P -s`: the partitions `modemst1`, `modemst2`, `fsg` (2 MiB each) and `fsc` (128 KiB), served through `memory@89b01000` (2 MiB, `qcom,rmtfs-mem`); `-r`: the modem's writes stay in rmtfs's memory, the partitions are never written; `-s`: rmtfs starts the modem, and stops it when it ends (SIGTERM) |

## The modem through a kexec

`qcom_q6v5_pas` has no shutdown hook, and `kexec -e` does not end
processes: **the modem keeps running** while Linux goes and KolibriOS
starts. Linux's votes with the RPM (power rails, clocks) stay as they
were. What must not happen is a normal shutdown of rmtfs: on SIGTERM it
stops the modem (`-s`). `tools/pro1x-kexec.sh` stops no services.

So KolibriOS does not boot the modem (that would be the SCM PAS calls,
loading the firmware from the phone's file system, and the RPM's power
votes over GLINK: a much larger piece, for later, should the modem ever
have to start under KolibriOS). It takes over a running modem, which has
its SIM unlocked and its network registration from Linux's time.

GLINK has no notion of the apps processor starting again while the modem
runs on. A channel Linux left open stays open on the modem's side, with
Linux's channel ids and receive buffers (intents), which KolibriOS does
not know. `pro1x-kexec.sh -m` therefore leaves `IPCRTR` open and
KolibriOS takes it over (`kolibri.ipcrtr=adopt`), as it does with the
ADSP's APR channel (`docs/pro1x-audio.md`):

1. `ofonod` is stopped: its QMI clients go away, and QRTR tells the
   modem so.
2. The EFS partitions are copied into a file (`efs.img`, below) that goes
   along as the kexec "initrd".
3. `kexec -e`, rmtfs still running (it no longer reaches the modem).
   Linux's shutdown on the way switches the WLAN firmware off cleanly
   over this same QRTR (ath10k's shutdown hook).

KolibriOS finds the channel's ids in what the FIFOs still hold behind
their heads: Linux's lcid in the newest QRTR packet from node 1 in the
transmit ring, the modem's in the newest one to node 1 in the receive
ring. It sends no OPEN, advertises receive buffers of its own, asks the
modem for intents as it needs them, and then says HELLO on QRTR, as a
peer that started again. If no QRTR packets are found it opens `IPCRTR`
anew. `tests/modem` tries that (`./test adopted`).

The way before (until 2026-10-29) was to unbind `IPCRTR` from
`qcom_smd_qrtr` so that GLINK closes it. On Linux 7.1 that unbind hangs
in the kernel for good (D state): `qcom_glink_destroy_ept()`, called from
the rpmsg device's own remove, unregisters that same device, and
`device_del()` then waits for the device lock the unbind holds. The WLAN
firmware also crashed when QRTR went away under it. The old fallbacks
(KolibriOS opening `IPCRTR` anew, answering the modem's OPEN from the
ring's past, `./test preopened`) stay for a kernel where the channel was
closed.

## KolibriOS's side

`kernel/modem/`, plain C without kernel dependencies (`modem_hw.h` is the
interface: the mapped SMEM and rmtfs memory, the kick, the EFS images),
so that `tests/modem/` runs all of it on the PC:

| File | |
| --- | --- |
| `smem.c` | the table of contents, the private partition with the modem (host 1), an item by number (uncached and cached lists) |
| `glink.c` | GLINK over SMEM: the two FIFOs, OPEN/OPEN_ACK/CLOSE, intents both ways (ours advertised and reused, the modem's tracked, asked for when none fits), TX_DATA in chunks, VERSION answered if the modem asks |
| `qrtr.c` | QRTR v1 on the `IPCRTR` channel: node 1, HELLO, the modem's NEW_SERVER/DEL_SERVER, DEL_CLIENT, BYE, RESUME_TX, our own servers announced |
| `qmi.c` | QMI messages: the 7-byte header, TLVs, requests with transaction ids, responses and indications to the services |
| `rmtfs.c` | the rmtfs service (14) from RAM: OPEN by path (`/boot/modem_fs1` = modemst1, ...), CLOSE, RW_IOVEC between the images and `memory@89b01000`, ALLOC_BUFF (that region), GET_DEV_ERROR; writes go into the images in RAM only, as with `rmtfs -r` |
| `sms.c` | 3GPP TS 23.040 PDUs: SMS-SUBMIT out (GSM 7-bit, or UCS-2 for other text), SMS-DELIVER in (GSM 7-bit with its extension table, 8-bit, UCS-2, concatenation headers), addresses, time stamps; UTF-8 for the programs |
| `phone.c` | the state machine: DMS operating mode to online, WMS routes (store on the modem and notify) and event report, the stored messages read at start, new ones on their indication, sending; VOICE indications, dial, answer, end, call states; NAS registration, operator and signal every few seconds |
| `fake.c` | a modem in RAM: the other end of SMEM, GLINK, QRTR and the services, enough for the programs in QEMU and the tests |
| `kmodem.c` | the kernel's side: the start, the work from the timer tick, function 100 |

`make -C tests/modem` builds all of it on the PC (with the address and
undefined behaviour sanitizers) and runs it: SMS PDUs against known ones
and round trips (7-bit with the extension table, UCS-2 with surrogates,
concatenation, alphanumeric senders), then the stack against `fake.c`
tick by tick: online, registered, the stored messages read, a message
sent and its echo received, a long one in three parts, a call dialled,
alerting, answered and ended, one coming in and answered, rmtfs reading
its image, the network's time; and the kexec case above.

The Pro1X's platform (`kernel/platform/pro1x.c`) maps SMEM, the rmtfs
memory and the APCS register (Device-GRE: never read speculatively),
reads their addresses, the modem's GLINK edge and the initrd from the
device tree, and runs the modem's work from the 100 Hz tick (polling the
FIFO; the interrupt is not needed for SMS and call control). In QEMU the
kernel runs the fake modem instead (`kolibri.fakemodem`, and always on
realview).

### The programs

Function 100 (KolibriOS for ARM's own number):

| Sub | |
| --- | --- |
| 0 | the modem's state: `struct kos_modem` (state, registration, operator, signal, counts and sequence numbers of messages and calls, the last error) |
| 1 | message N (`struct kos_sms`: number, time, direction, state, text in UTF-8) |
| 2 | send a message: number, text |
| 3 | dial a number |
| 4 | answer the incoming call |
| 5 | end the call |
| 6 | the call (`struct kos_call`: id, state, direction, number, start) |
| 7 | delete message N |
| 8 | mark message N as read |

`apps/phone`: the dialler, the call screen (incoming: answer, reject;
active: end), and the messages: conversations by number, a
conversation's messages as bubbles, writing one (the keyboard). In the
Hildon look; `@hildon` starts it when a call comes in.

### The EFS images

The rmtfs service needs the modem's partitions should the modem read
them while KolibriOS runs (it reads them when it starts; while it runs it
mostly writes). `pro1x-kexec.sh -m` passes them as the initrd, the format
`tools/mkefs.py` writes and `rmtfs.c` reads:

| Offset | |
| --- | --- |
| 0 | `KOSEFS01` |
| 8 | the number of images (u32 LE), then for each: a name (16 bytes, `modemst1`, `modemst2`, `fsg`, `fsc`), offset (u32), size (u32) |
| ... | the images, 4 KiB aligned |

KolibriOS keeps the initrd's memory out of its own. Without it, rmtfs
answers OPEN with an error, as Linux's rmtfs does for a missing partition.

## Not done yet

- **Call audio** is written but not tried on the device
  (`docs/pro1x-audio.md`: `pro1x-kexec.sh -a`, Linux's APR channel to the
  ADSP taken over, the voice session per call, the speaker switch).
  Without `-a`, calls connect without sound.
- Starting the modem (above), restarting it after a crash (the crash
  shows: the QRTR BYE, or nothing any more).
- SIM PIN entry (UIM service): the SIM stays unlocked from Linux.
- Messages are kept on the modem (NV storage) and read at start; sent ones
  only in RAM.
- Mobile data (IPA), USSD, MMS.

## The device test

Needs the user at the phone (a reboot into KolibriOS, the watchdog back
into the boot menu). On Leste, as root:

```sh
cp build/arm64/kernel.bin /host/kolibri/kernel.bin
tools/pro1x-kexec.sh -n -m           # loads kernel and EFS images, unloads: no start
tools/pro1x-kexec.sh -m /host/kolibri/kernel.bin "kolibri.lifetime=300"
```

Then: the phone app's state (registered, operator), a message to the
phone and one from it, a call to it and one from it (answered, ended:
without sound; with `-a` too, the sound: `docs/pro1x-audio.md`). The RAM console (`tools/pro1x-read.py`) has the modem's
log lines (`modem:`) after the reset.
