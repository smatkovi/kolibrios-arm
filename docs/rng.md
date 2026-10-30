# Random numbers

One generator in the kernel for everything that needs random numbers: the
Linux personality's `getrandom`, `/dev/urandom`, `/dev/random` and
`AT_RANDOM` (and with them the messengers' keys: WhatsApp's, Telegram's,
Signal's), lwIP's ids and ports, the Wi-Fi's nonces and SAE's scalars, and
KolibriOS programs through function 102. It is seeded from the hardware
where the hardware has a source, from the seed kexec hands over, and only
when those give too little from the timing jitter of the CPU's counters,
with a warning that stays on the console.

| | the sources | the seed | linking real messenger accounts |
| --- | --- | --- | --- |
| Pro1X | the SoC's PRNG (`qcom,prng-ee`), kexec's `/chosen/rng-seed` (128 bytes from Linux's own generator) | both, each enough on its own | yes, once the device check below shows the seed line (2026-10-31) |
| N950 | the OMAP3630's RNG through the secure ROM (Nokia's PPA, service 29): **not tried on the device**; timing jitter | the ROM's RNG if it answers, else jitter only (`rng: WEAK`) | no Linux personality there; with jitter only nothing that needs a secret key |
| QEMU's virt | virtio-rng, QEMU's `rng-seed` | both | (tests only) |
| QEMU's realview | timing jitter | jitter only | - |

## The generator

`kernel/rng_core.c` (plain C, built on the PC by `tests/rng`):

- **The pool**: SHA-256 over everything mixed in since the last reseed,
  with the bits credited to it counted. A hardware source is credited by
  its quality (bits per 1024 read, as Linux's hwrng quality: the Pro1X's
  PRNG and virtio-rng 1024, the N950's ROM RNG 900 as Linux's
  `omap3-rom-rng`), the boot loader's seed fully (as Linux with
  `random.trust_bootloader`, its default), timing jitter at 1/64 of a bit
  per sample (below), input events and clock values not at all.
- **The reseed**: the key becomes HMAC-SHA256(key, label | the pool's hash |
  the reseed's number), and only with 256 credited bits in the pool; the
  next pool starts with the old pool's hash.
- **The output**: ChaCha20 (RFC 8439) with fast key erasure: each request
  takes block 0 of the key, whose first half is the next key and whose
  second half the key of this request's blocks, wiped afterwards. Nothing
  left in memory gives earlier output again.
- **Before the seed**: requests that cannot wait (lwIP's first ids, at its
  start, before the platform's sources are all registered) get output of
  a key stirred from the pool so far, which keeps the pool and its credit.

`kernel/rng.c`, around it:

- The seed is made after the platform's drivers (`rng_init` in `kmain`,
  before any program): the boot loader's seed, 64 bytes from each
  hardware source, the clocks. Should these credit fewer than 256 bits,
  timing jitter is collected until they are there, for a few seconds at
  most; that is the waiting. One line says what seeded it:

  ```
  rng: seeded from the SoC's PRNG (64 bytes), the boot loader's seed (128 bytes)
  ```

  and, with jitter only, `rng: WEAK: no hardware source and no boot
  loader's seed, only timing jitter; not for keys that matter`.
- Every word a source gives goes through FIPS 140-2's continuous test
  (4.9.2): a word as the one before throws the read away; the third time
  the source is not used any more (`rng: ... gives the same word again and
  again: not used`).
- Once a minute, while a hardware source works, 64 new bytes of each go
  into the pool and the key is made anew.
- The kernel runs on one CPU with the interrupts masked, so nothing here
  needs a lock.

| who | how | before the seed |
| --- | --- | --- |
| `getrandom` | Linux's flags: `GRND_NONBLOCK` (1), `GRND_RANDOM` (2, the same generator), `GRND_INSECURE` (4); `GRND_INSECURE \| GRND_RANDOM` and unknown flags `EINVAL` | waits (a batch of jitter at each look), `GRND_NONBLOCK`: `EAGAIN`, `GRND_INSECURE`: served |
| `/dev/urandom`, `/dev/random` | the generator | both wait (`O_NONBLOCK`: `EAGAIN`); Linux's `/dev/urandom` would not |
| `AT_RANDOM` | 16 bytes at the start of each Linux program | served (the start cannot wait) |
| lwIP (`LWIP_RAND`) | DHCP's and DNS's ids, TCP's initial sequence numbers, local ports | served |
| the Wi-Fi | the SNonce, SAE's scalars and masks, the random MAC address | the Wi-Fi does not start without a seed; a warning when it is jitter only |
| function 102.1 | KolibriOS programs (`kos_random` in `lib/kos.h`) | -1 |

The first use by each Linux program is said on the console, with what
seeded the generator:

```
lx: signal-host: getrandom from the kernel's generator, seeded from virtio-rng (64 bytes), the boot loader's seed (32 bytes)
```

**Function 102** (KolibriOS for ARM, next to 100, the modem, and 101,
Linux programs; KolibriOS itself has no function for random numbers, its
programs seed `rand()` from the time): `EBX` = 1, `ECX` the buffer, `EDX`
the length (at most 4096); `EAX` the length, or -1 while the generator
has no seed.

## The Pro1X: the SoC's PRNG and kexec's seed

What the hardware is, from Linux 7.1's sources (`arch/arm64/boot/dts/qcom/sm6115.dtsi`,
`drivers/crypto/qcom-rng.c`, `drivers/clk/qcom/gcc-sm6115.c`,
`clk-branch.c`):

- `rng@1b53000`, `compatible = "qcom,prng-ee"`, 4 KiB: the execution
  environment's window of the PRNG: `DATA_OUT` at 0, `STATUS` at 4 (bit 0: a
  word is there). Its configuration (`LFSR_CFG` at 0x100, `CONFIG` at
  0x104) belongs to TrustZone, which set it up: Linux's `prng-ee` match data
  has `skip_init`, and nothing here reads or writes those registers.
- Its clock `GCC_PRNG_AHB_CLK`: a branch voted on in the APCS's vote
  register (GCC 0x79004, bit 13), with hardware clock gating (`hwcg_bit` 1
  of its CBCR at 0x21004) and `BRANCH_HALT_VOTED`. Linux votes it on
  around each read (`qcom_rng_generate`: `clk_prepare_enable`, the read,
  `clk_disable_unprepare`). On, `clk_branch_wait` waits up to 200 us for
  CLK_OFF (bit 31) to clear or the NOC FSM's status (bits 30:28) to say
  on, and checks nothing at all while the branch is under hardware gating
  (HW_CTL, bit 1); off, 10 us.
- A read: a word whenever STATUS says one is there, polled every 200 us
  for at most 10 ms (`readl_poll_timeout`); a word of zero is an error
  (`-EINVAL`, the whole read).
- Linux registers it with the crypto API only, as the `stdrng` driver
  `qcom-rng`, not as a hwrng: it does not feed Linux's own pool (only the
  newer `qcom,trng` is a hwrng, quality 1024). Qualcomm calls the block a
  pseudo-random number generator: a generator inside the SoC, seeded
  inside the SoC; neither its entropy source nor its design is
  documented, and nothing outside can check it beyond statistics.

`kernel/platform/qcom_prng.c` reads it exactly so, on its registers
through functions given to it (`tests/rng` drives it against a model of
the registers). It is credited as a full source, but it is never the only
one: kexec (`tools/pro1x-kexec.sh`: `kexec -s`, i.e. kexec_file_load)
puts 128 bytes of Linux's generator into the device tree as
`/chosen/rng-seed` (Linux's `of_kexec_alloc_and_setup_fdt`, only once
Linux's generator is seeded, which on the phone it is long before the boot
menu's kexec), and 8 as `kaslr-seed`. The kernel copies both before the
MMU is on and wipes them in the tree (as Linux's `early_init_dt_scan_chosen`
does). Either alone credits the 256 bits of the seed.

## The N950: the secure ROM, and jitter

The OMAP3630 has an RNG module (`rng@480a0000`, `ti,omap2-rng`), but the
N950 is an HS device and the module sits behind the secure side's
firewall: Linux's `omap3-n950-n9.dtsi` disables it ("RNG not directly
accessible on N950/N9"). Nokia's PPA reads it for the normal world:
Harmattan's kernel did so on this very device (`arch/arm/mach-omap2/hs.c`,
`sec_hal.c`, `sec_hal_entry.S` in `kernel_2.6.32-20115101`):
`SEC_HAL_RNG_GENERATE` (29) with `RNG_GEN_HW` (8), `smc #1` with the
service in r12, r6 = 0xff (a new call), the flag 4
(`FLAG_START_HAL_CRITICAL`) in r2 and in r3 the physical address of
`{ 3, buffer, length, command }`; the caches flushed around it, the
interrupts masked. Linux's `omap3-rom-rng` reads the N900's PPA the same
way (service 29, there with nargs + 1 and flag 0), with the interface
clock `rng_ick` (CM_ICLKEN2_CORE bit 2) on and `RNG_GEN_PRNG_HW_INIT` (2)
first.

`kernel/platform/n950.c` does Harmattan's call, the caches cleaned and
invalidated by address around it (the kernel's RAM is mapped at its
physical address), `rng_ick` on, and when the first call fails,
`RNG_GEN_PRNG_HW_INIT` and once more. It answers or it is not used
(`n950: the secure ROM's RNG: an error ... not used`).
**Untried on the device**: should the call hang the N950,
`kolibri.romrng=off` leaves the PPA alone.

**Timing jitter** (both kernels, used only when the sources give too
little): a walk through 64 KiB (beyond the L1 cache) of a length that
depends on the last sample, timed by the fastest counter: the cycle
counter (PMCCNTR) on ARMv7, the generic timer on AArch64 (19.2 MHz on the
Pro1X). On the N950 the sample ends at the next edge of the 32 kHz sync
timer, whose crystal (the TWL5031's) is independent of the CPU's DPLL, so
each sample holds the phase between the two. How much of a sample is
unpredictable is not known. Each batch of 1024 samples is estimated (SP
800-90B's most common value estimate on their low bytes) and credited 16
bits, 1/64 of a bit per sample, only when the estimate is at least 1 bit
per sample; batches below that count nothing. That is a guess with a
margin, not a bound: a CPU this simple (the Cortex-A8 is in order) may
have far less jitter than the estimate says, and the estimate cannot tell
noise from a pattern it does not see. In QEMU the estimate went from 0.7
to 4 bits per sample on virt with the PC's load (from 16 to some 50
batches) and 3.2-4.1 on realview; that is the host's timing, not a CPU's.
On the N950 it is still to be measured (`kolibri.rngtest`). Input events (keys,
the touch screen) are mixed in with their time, credited nothing.

So on the N950 without the ROM's RNG the seed is weak, and the console
says so at every start. The N950 has no Linux personality (no
messengers); function 102 serves its programs what there is.

## FreeBSD on the Pro1X

The same questions for FreeBSD are answered in `pro1x-maemo-leste`'s
`freebsd/rng/README.md`: FreeBSD 15.1's `qcom_rnd` driver knows only
`qcom,prng` (ARMv7, IPQ4018); a patch adds `qcom,prng-ee` on arm64 with
the clock voted as above, and FreeBSD now also takes kexec's `rng-seed`.

## Tests

On the PC, `make -C tests/rng` (needs OpenSSL's command line):

- ChaCha20 against RFC 8439's vectors (2.3.2, 2.4.2), and the Python
  model's ChaCha20 against OpenSSL's for 20 random keys, nonces and
  counters
- the generator: no seed below 256 credited bits, a new key after every
  request, the request's output only from the key before it, the stir
  keeping the pool, the old key's part in a reseed; the same sequence of
  mixes, stirs, reseeds and requests in `tests/rng/model.py` (Python's
  hashlib and hmac), byte for byte
- FIPS 140-2's tests on fixed patterns and their statistics on three
  blocks as the model computes them; the continuous test; the most common
  value estimate within 2/256 of the model's (floating point); log2
- the PRNG's driver against a model of its registers and its clock: six
  words, a partial word, the vote back with the other votes kept,
  hardware gating (CLK_OFF not waited for), a clock that does not come on
  (200 us, the PRNG not touched), the NOC FSM's status, a word of zero, no
  word (10 ms), a slow PRNG; no access the hardware would refuse (the
  PRNG without its vote, its configuration). 31 checks

In QEMU, `make -C tests/rng qemu` (`tests/rng/qemu.py`; logs kept):

| scenario | what | shown |
| --- | --- | --- |
| `virt` | virtio-rng and QEMU's `rng-seed` | both in the seed; `kolibri.rngtest`: virtio-rng and the generator 8 of 8 FIPS blocks; `tests/linux/rngtest`: getrandom with each flag, the refused flags, 300000 bytes, `/dev/urandom`, `/dev/random`, Go's `crypto/rand`, FIPS on getrandom's output |
| `virtio` | virtio-rng alone (`dtb-randomness=off`) | seeded from it, no warning |
| `jitter` | no source | seeded from jitter, the warning, getrandom works |
| `noseed` | no source, `kolibri.rngjitter=off` | `rng: NOT seeded`; `GRND_NONBLOCK`: `EAGAIN`, `GRND_INSECURE`: bytes, `/dev/urandom` with `O_NONBLOCK`: `EAGAIN`, a blocking getrandom and a blocking read still waiting after 3 s |
| `realview` | ARMv7, jitter | seeded, the warning, the generator 8 of 8 |
| `pro1x-seed` | the Pro1X's code paths (`tests/power/mkdtb.py`) with a `qcom,prng-ee` and a `qcom,gcc-sm6115` node in RAM, `/chosen/rng-seed` of 128 bytes | the seed from kexec's 128 bytes, wiped in the tree (read back from QEMU's memory); the PRNG's RAM gives one word again and again: the continuous test throws it out, FIPS fails, not used |
| `pro1x-nodata` | STATUS never set | `pro1x: the PRNG: no word for 10 ms`, the seed from kexec's |
| `pro1x-clock` | CLK_OFF stays set | `pro1x: the PRNG: its clock does not come on` |
| `messengers` | `PURPLE_HOST`, `SIGNAL_HOST`, `YAPPARI_WA` given: each backend started by the messenger, QEMU's network | each takes its random bytes from the kernel's generator (2026-10-30: purple-host, signal-host and yappari-wa each `getrandom` and `/dev/(u)random`, seeded from virtio-rng and QEMU's seed; Telegram with a number of its reserved test range, 99966...) |

## On the devices (2026-10-31)

**The Pro1X**, first under Leste, the PRNG through Linux's driver (as
root; the Pro1X kernel has both as modules, `CONFIG_CRYPTO_DEV_QCOM_RNG=m`,
`CONFIG_CRYPTO_USER_API_RNG=m`):

```
modprobe qcom_rng; modprobe algif_rng
grep -A4 'driver       : qcom-rng' /proc/crypto
python3 tools/qcom-rng-check.py 200000
```

Expected: `qcom-rng: 200000 bytes, 0 read errors; FIPS 140-2: 80 of 80
blocks pass (or 79); repeated words: 0, zero words: 0; most common byte:
7.9 bits per byte` or close. Read errors are Linux's `EINVAL` (a word of
zero) or `ETIMEDOUT` (no word in 10 ms).

Then KolibriOS with the check (`docs/pro1x.md` for the kexec):

```
tools/pro1x-kexec.sh /host/kolibri/kernel.bin "kolibri.rngtest kolibri.lifetime=60"
# after the next boot, as root:
tools/pro1x-read.py | grep -E 'rng|PRNG'
```

Expected, in this order:

```
rng: the SoC's PRNG
rng: seeded from the SoC's PRNG (64 bytes), the boot loader's seed (128 bytes)
rngtest: the generator: the SoC's PRNG (64 bytes), the boot loader's seed (128 bytes); seeded
rngtest: the SoC's PRNG: 8 of 8 blocks pass FIPS 140-2 (...); 20000 bytes in ... us; ...
rngtest: the SoC's PRNG: 0 repeated words, 0 empty reads while in use
rngtest: timing jitter: 4096 samples in ... us; most common value: ...
rngtest: the generator: 8 of 8 blocks pass FIPS 140-2 (...)
```

and no `pro1x: the PRNG: ...` lines. Worth noting: the PRNG's speed (the
microseconds for 20000 bytes) and the jitter's estimate on the generic
timer. What else could show:

- no `the boot loader's seed`: the kexec was not `kexec -s`, or Linux's
  generator was not seeded (both unlikely); the PRNG alone still seeds
- `pro1x: the PRNG: its clock does not come on`: the vote at 0x79004 or
  the CBCR at 0x21004 differ from Linux's description; the seed then comes
  from kexec's alone
- `no word for 10 ms` or `a zero word`: the PRNG is not producing in the
  EE window; the same
- `rng: WEAK`: neither; then no messenger account may be linked

Once the first two expected lines are there, the messengers may link real
accounts as far as the random numbers go (`docs/messengers.md`).

**The N950** (`docs/n950.md` for NOLO), the kernel with the check built in:

```
make PLATFORM=n950 CMDLINE=kolibri.rngtest
```

Expected on the serial console: `n950: the secure ROM's RNG (PPA service
29): asking ...`, then either `n950: the secure ROM's RNG answers`, `rng:
the secure ROM's RNG` and `rng: seeded from the secure ROM's RNG (64
bytes)` (no `WEAK`: its 64 bytes credit 450 bits), and `rngtest: the
secure ROM's RNG: 8 of 8 blocks pass`; or `n950: the secure ROM's RNG: an error (0x...,
init 0x...): not used`, the error code worth writing down, and `rng: WEAK`.
Either way the `rngtest: timing jitter` line gives the estimate on the
real Cortex-A8 against the 32 kHz crystal. A hang right after `asking
...`: again with `CMDLINE="kolibri.rngtest kolibri.romrng=off"`.
