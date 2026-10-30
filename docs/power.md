# Power management (AArch64 kernel, the Pro1X)

Standby on the Pro1X as low as the SM6115 allows. What Linux reaches on
the phone (Maemo Leste, display off, 2026-10-28): about 160 mA, the SoC
never in its sleep states (`qcom_stats` vlow and vmin 0), the CX rail up,
80-90 interrupts a second, no hand-over to the RPM. The large step on any
system is the SoC's XO shutdown and vmin; it needs the CPUs and their
cluster really powered down (PSCI), few wake-ups, the RPM's sleep set
voting the shared resources down, and a wake-up the RPM can see (the next
timer, through the MPM). The steps:

| Stage | What | Where | Tested |
| --- | --- | --- | --- |
| 1 | tickless idle: the timer at the next deadline instead of 100 Hz; the Pro1X's input and modem by interrupt | `kernel/arch/arm64/timer.c`, `kernel/sched.c`, `kernel/power.c`, `kernel/platform/pro1x.c`, `kernel/modem/kmodem.c` | QEMU (`make -C tests/power`) |
| 2 | CPU idle through PSCI: the core's and the cluster's power-down states, the context kept across them, the memory-mapped timer as the wake-up | `kernel/arch/arm64/psci.c`, `suspend.S`, `powerdt.c`, `timer.c` | the device tree on the PC against the phone's (`make -C tests/power host`); QEMU's CPU_SUSPEND, the way back played (`kolibri.psci=simulate`) under Go, TLS and the network |
| 3 | the RPM's sleep set, the MPM's timer before the cluster's collapse, the RPM's sleep counts | `kernel/modem/rpmsleep.c`, `rpm.c`, `kernel/platform/pro1x.c`, `powerdt.c` | `make -C tests/modem` (the requests against an RPM in RAM, the vMPM's layout, the statistics), `make -C tests/power host` (the addresses in the phone's tree) |
| 4 | the display off (DCS, the scan-out stopped), the programs' timeouts on whole seconds, the battery's voltage and current in the power test's lines | `kernel/platform/pro1x.c`, `kernel/modem/pmic.c`, `kernel/sched.c` | `make -C tests/power host` (the SPMI arbiter's channels, the gauge, the ADC against an arbiter in RAM); QEMU (`kolibri.standby`: 1.4 wake-ups/s) |
| 5 | the standby by the power key, ended by the power key or a key: the PMIC's interrupts through the SPMI arbiter, the MPM's wake pins with their types, the MPM's wake interrupt | `kernel/platform/pro1x.c`, `kernel/modem/pmic.c`, `rpmsleep.c`, `kernel/arch/arm64/psci.c` | `make -C tests/power host` (Linux's interrupts quieted, the PON's set up, a press latched and taken, against the arbiter in RAM), `make -C tests/modem` (the wake set in the vMPM), QEMU (`kolibri.keytest`: the standby ended by a played press, the wake-ups back from 2.3/s to 5.2/s) |

## Measuring: `kolibri.powertest=N`

N minutes (10 without a number) with a console line a minute, on the
Pro1X followed by the reset into the boot menu (a lifetime of N minutes
and 30 s; `tools/pro1x-read.py` shows the RAM console afterwards):

```
power: test for 10 min, a line every 60 s (tickless idle)
power: 60 s: 251 wake-ups (4.1/s), timer 250, other 1, idle 99.9 %, wfi 251, core 0, cluster 0, refused 0
power:   by deadline: tick 12 tasks 228 platform 6 modem 12, @hildon 228
...
power: test over, all 600 s: ...
```

- **wake-ups**: the idle loop's, per second;
- **timer**, **other**: the interrupts, the timers' and the rest;
- **idle**: the share of the time in the idle loop;
- **wfi, core, cluster, refused**: the idle states entered (stage 2);
- **by deadline**: who asked for the wake-ups that came by the timer:
  the tasks (by name), the platform (the watchdog, input polls), the
  modem, the screen (a flush), the network (lwIP's timers), the test
  itself; `tick`: the 100 Hz tick, kept while something is about to
  happen.

`kolibri.powerline=S` makes it a line every S seconds (the QEMU tests).

On the Pro1X the test is a standby (stage 4): 10 s after the start the
display goes off and the SoC may sleep (the sleep set, the MPM: stage
3); every 10 s (with the watchdog's feeding) the battery is sampled, a
line a minute shows the minute's mean, and the RPM's counts:

```
battery: 3902 mV, 152 mA (6 samples, 140 to 171)
rpm: this minute: vmin 0 (+0, 0 s in all), vlow 12 (+12, 50 s), votes 0 0; MPM hand-overs 58
...
battery: the test's mean 150 mA over 54 samples, 3921 mV to 3903 mV
pro1x: display on: ...
```

The current is positive while discharging (the gauge's sign); on a USB
cable it is the charger's, so the test is meant to run unplugged.
`kolibri.displayon` keeps the display on (and the sleep set as it is),
for a comparison.

## Stage 1: tickless idle

The clock is the generic timer's virtual counter. Its virtual timer, at
the interrupt the device tree's `arm,armv8-timer` node names third (the
binding's order: secure, non-secure physical, virtual, hypervisor; QEMU's
virt: PPI 11, INTID 27; the SM6115: PPI 3, INTID 19 — the kernel had 27
fixed before, which is not the Pro1X's), ticks at 100 Hz while tasks run,
on a fixed grid of the counter; `ticks` follows the counter and is read
again at every interrupt.

When nothing is ready, `arch_idle` (start.S's idle loop, with interrupts
masked: the wake-up's interrupt is taken when the loop opens them) asks
`sched_next_work` how many ticks from now something needs the CPU:

- the tasks' timeouts (functions 5 and 23), the Linux threads' deadlines
  (nanosleep, futex and poll timeouts);
- the platform (`next_tick`): on the Pro1X the watchdog (fed every third
  of its bark time, 10 s with `pro1x-kexec.sh`'s 32 s, instead of every
  second), the end of the lifetime, the input while keys are held or the
  touch screen is touched;
- the modem: its setup, an answer it waits for, a call, the next network
  poll (5 s); what the modem sends comes with its GLINK edge's interrupt
  (SPI 68 on the SM6115, the device tree's `glink-edge`), without it a
  poll every second;
- the screen (a flush pending), lwIP's timeouts (`sys_timeouts_sleeptime`),
  the power test's next line.

and moves the timer there (at most 10 minutes). A wake-up by another
interrupt puts the 100 Hz grid back, for what the interrupt made ready.
`kolibri.tick=periodic` keeps the 100 Hz tick in idle too.

The Pro1X's keyboard and touch screen were polled by the tick. Now the
expander's INT (GPIO 107, falling edge), the modifier keys (gpio-keys,
either edge) and the touch screen's INT (GPIO 80, either edge) are TLMM
interrupts (pinctrl-msm.c's register layout: `INTR_CFG` with the
detection, the polarity, `RAW_STATUS_EN` and the target KPSS 3, Linux's
`pinctrl-sm6115.c`), forwarded through the TLMM's summary interrupt
(SPI 227). Every GPIO interrupt Linux left enabled for this processor (the
target KPSS) is masked first (an unknown one would hold the summary
interrupt); those routed to the modem or the DSPs and the reserved GPIOs
(`gpio-reserved-ranges`, 0-3 and 14-17) are not touched. Polled still:
every tick while the modifiers settle and a scan waits for its
confirmation, every 30 ms while keys are held, every 50 ms while the
screen is touched. Without the interrupts (`kolibri.tick=periodic`, or
no INT in the device tree): every 50 ms (the keyboard), 250 ms (the touch
screen).

In QEMU (`make -C tests/power`, 10 s lines, the Hildon shell and the
fake modem):

| | wake-ups/s |
| --- | --- |
| `kolibri.tick=periodic` | 100 |
| tickless, QEMU's virt | 4.1-4.8 |
| tickless, the Pro1X's code paths (`tests/power/mkdtb.py`) | 5.0-5.1 |
| tickless, with the network (lwIP's DHCP and ARP timers) | 6.0 |

Nearly all of what is left is the Hildon shell, which looks at its
windows every 250 ms (`kos_wait_event_timeout(25)`), and the watchdog
once a second in QEMU (no watchdog there: the default interval).

## Stage 2: CPU idle through PSCI

What the device tree says (`kernel/arch/arm64/powerdt.c`, read while the
MMU is off; Linux's bindings `cpu/idle-states.yaml`,
`power/domain-idle-state.yaml`, `timer/arm,arch_timer_mmio.yaml`), on the
Pro1X (`sm6115.dtsi`, checked by `tests/power/host.c` against the
phone's own tree):

| | |
| --- | --- |
| PSCI | 1.0 binding, `smc` |
| CPUs | 8: 0x0-0x3 (silver, cluster 0), 0x100-0x103 (gold, cluster 1); KolibriOS runs on CPU 0 (Linux's kexec leaves the others off: CPU_OFF) |
| CPU 0's state | `silver-rail-power-collapse` 0x40000003, entry 290 us, exit 376 us, at least 1182 us, `local-timer-stop` (its power domain `cpu_pd0`) |
| cluster 0's states | GDHS 0x40000022 (782 us), power collapse 0x41000044 (exit 2118 us, at least 7376 us) (the domain above, `cluster_0_pd`) |
| the broadcast timer | `arm,armv7-timer-mem` at 0xf120000, frame 0 at 0xf121000, its physical timer SPI 8 (INTID 40) |

`psci_init` asks the firmware (PSCI_VERSION, PSCI_FEATURES of
CPU_SUSPEND: bit 0 OS-initiated mode, bit 1 the extended state ids),
asks AFFINITY_INFO for the other CPUs and, as Linux's
`cpuidle-psci-domain.c` does, sets OS-initiated mode (SET_SUSPEND_MODE)
when there are cluster states. Then each idle chooses by the time to
the next deadline, as Linux's hierarchical PSCI idle does: below 1182 us
`wfi`, from there the core's state, and with OSI and the other cores
off the cluster's on top of it (GDHS from 1182 us, power collapse from
7376 us; the cluster's parameter alone, the domain state wins). The core
state stops the core's timer: before it the frame's physical timer is
set to the same deadline (`timer_bcast_set`, the frame's count compared
with the virtual one, which may be offset; CNTACR as Linux's
`find_best_frame` sets it). Without the frame those states are not used.

The states are power-down states: the core comes back at `cpu_resume`
(`suspend.S`, as Linux's `sleep.S`) with the MMU and the caches off, its
registers gone. `psci_suspend` keeps the callee-saved registers, the
stack pointer, the EL1 system registers (SCTLR, TCR, TTBR0/1, MAIR,
VBAR, CPACR, CNTKCTL, the thread registers, SP_EL0), the virtual timer
and, if the Linux personality has them on, the FP/SIMD registers (a
thread's are kept in them lazily) in a context cleaned to the point of
coherency; `cpu_resume` loads it, turns the MMU on under the running
code (the kernel's addresses are its physical ones) and returns 0 from
`psci_suspend`. The GIC's CPU interface is set up again
(`gic3_cpu_init`, as Linux's CPU PM notifier does).

| `kolibri.psci=` | |
| --- | --- |
| (none) | the core's and the cluster's states (OSI) |
| `off` | `wfi` only |
| `core` | no cluster state |
| `cluster` | the cluster's states also without OSI (the firmware coordinates) |
| `simulate`, `simulate,cluster` | QEMU: after CPU_SUSPEND (QEMU's is a `wfi`) the core's state is destroyed as a power-down would (the MMU and the caches off, the system registers, the timer, the GIC's CPU interface and the FP registers cleared, the stack pointer 0) and the way back through `cpu_resume` taken |

The power test's line counts the states and the returns through
`cpu_resume` (`resumed`).

In QEMU (`make -C tests/power`): QEMU replaces `/psci` in a tree it is
given, so `mkdtb.py` puts the power domains in a node of their own, and
its CPU_SUSPEND takes only PSCI 0.2's original format (0x10003 and so
on). The results: every idle in the core's state (QEMU has no OSI), with
`kolibri.psci=simulate` every one back through `cpu_resume` (52 in 10
s), with `simulate,cluster` in the cluster's; Go's runtime (the hello
test) and its TLS 1.3 to telegram.org and web.whatsapp.com
(`tests/linux/nettest`, FP/SIMD crypto) through 117 played power-downs
in 10 s.

Not testable in QEMU: the firmware's own power-down and warm boot, OSI
on the SM6115, the memory-mapped timer as the wake-up.

## Stage 3: the RPM's sleep set and the MPM

On the RPM-based SM6115 the SoC's sleep states (vlow: XO shutdown; vmin:
CX and MX at retention as well) are the RPM's to enter, when every
master (the apps processor, the modem, the ADSP, the CDSP) sleeps and
none of them needs the shared resources then. Each master's votes come
in two sets: the active set while it runs, the sleep set while it
sleeps. Three things on the apps processor's side, as Linux does them:

**The sleep set** (`kolibri.rpmsleep`; `rpm_sleep_votes`,
`kernel/modem/rpmsleep.c`): after a kexec it is what Linux left, its
consumers' votes and its maxima from the boot (rpmpd and clk-smd-rpm
clamp to the highest until `sync_state`). KolibriOS sends what Linux
sends when nothing uses a resource, 0, in the sleep set (state 1 of an
SMD-RPM request, `include/linux/soc/qcom/smd-rpm.h`), the active set as
it is:

| resource | RPM type, id, key | Linux |
| --- | --- | --- |
| the XO | `clk0` 0 `Enab` | `clk-smd-rpm.c` bi_tcxo |
| its buffers | `clka` 1-3 (LN_BB), 4-5 (RF, not with `kolibri.wifi`) `swen` | XO_BUFFER |
| CX, MX | `rwcx` 0, `rwmx` 0 `vlvl` | `rpmpd.c`, sm6115 |
| the bus clocks | `clk2` 0 bimc, `clk1` 1 cnoc, `clk1` 2 snoc, `mmxi` 0-1, `qup` 0 `KHz` | `sm_qnoc_icc_clks` |

Not voted: the interconnect's per-node bandwidth (`icc-rpm.c`'s master
and slave requests), the regulators (Linux votes them in the active set,
which the RPM keeps in sleep too).

**The MPM** (`mpm_prepare`): while the SoC sleeps the GIC is off and the
MPM, in the always-on domain, wakes it: by one of its pins or by its
timer. Its registers are mirrored in the RPM's message RAM (the vMPM,
Linux's `irq-qcom-mpm.c`: TIMER0/1, then ENABLE, FALLING_EDGE,
RISING_EDGE, POLARITY and STATUS, three words each for 96 pins), at
0x45f01b8 (the device tree's `qcom,mpm` node, from pro1x-maemo-leste's
patch 0006; without it the same place, 96 pins and IPC bit 1, said on
the console). Before the cluster's deepest state (its power collapse)
the next deadline goes into TIMER0/1 as a count of the virtual counter,
no pin is enabled, the status is cleared, and the RPM is told through
the MPM's bit in the APCS IPC register (`mboxes = <&apcs_glb 1>`), as
the Linux patch 0011 does when the last CPU goes down. The keyboard's
GPIOs are not among the MPM's pins (106, 107, 112), so in vmin only the
timer (and what the RPM itself wakes for) wakes the phone.

**The RPM's statistics** (`rpm_stats_read`, Linux's `qcom_stats.c` for
`qcom,rpm-stats` at 0x4690000: the records' offset in the word at 0x14,
two records of 48 bytes, the name, the count, the times, the masters'
votes): at the start, every minute of a power test and at its end:

```
rpm: left by Linux: vmin 0 (+0, 0 s in all), vlow 0 (+0, 0 s), votes 0 0; MPM hand-overs 0
rpm: the sleep set: 12 of 12 resources at 0
pro1x: the SoC's sleep: the sleep set voted, the MPM handed over before the cluster's collapse
...
rpm: this minute: vmin 0 (+0, ...), vlow 3 (+3, 41 s), ...; MPM hand-overs 120
```

A count that does not move while the hand-overs do: another master or a
vote still holds the SoC up (the display is the first: stage 4).

**Devices whose interrupts must get through** (`power_hold_awake`,
`kernel/power.c`): the Wi-Fi's copy engines (SPIs 358-369) are no MPM
pins, so with `kolibri.wifi=irq` the driver holds the SoC's sleep off for
a second after each frame: `psci_idle` then takes the state for an idle
just short of the collapse's residency (GDHS, 40000022: the GIC on) and
counts it (`SoC sleep held N` in the power test's lines). Idle Wi-Fi
holds nothing; a frame that comes while the SoC sleeps waits at its
engine for the next wake-up by time (docs/pro1x-network.md, stage 7c).

## Stage 4: the display off, the battery

**The display** (`display_power`, `kernel/platform/pro1x.c`): Linux
leaves the panel (BOE BF060Y8M-AJ0) in video mode, 60 frames a second
read from RAM. Off: DCS commands through the DSI controller's command
DMA, as FreeBSD's `qcom_dpu` sends them (pro1x-maemo-leste
`freebsd/patches/0022`, after Linux's msm `dsi_cmd_dma_tx`: the packet
in a buffer below 4 GiB, the controller in command mode as well, the DMA
started after a frame's video, done when CMD_DMA_DONE latches; untested
on the phone there too): brightness 0, display off (high speed), 20 ms,
sleep in (high speed), as Linux's `panel-boe-bf060y8m-aj0.c` switches it
off; then the interface's timing engine stops (INTF_1 at the DPU's
0x6a800, `INTF_TIMING_ENGINE_EN`; `dpu_6_3_sm6115.h`, `dpu_hw_intf.c`):
no more frames. The flushes into the framebuffer stop meanwhile. On at
the end: the timing engine, sleep out, 30 ms, display on, brightness 127
(the panel driver's default; the panel keeps its registers through
sleep). Each step's result goes to the console.

**The programs' timeouts** while the display is off: on a grid of whole
seconds (`sched_next_work`), at most a second late, so that the shell's
look at its windows every 250 ms and a clock's second come together:
one wake-up a second instead of four or five. `kolibri.standby` does the
same without a display to switch off (QEMU: 4.1-4.8 wake-ups/s become
1.4).

**The battery** (`kernel/modem/pmic.c`): the SPMI PMIC arbiter, version
5 (Linux's `spmi-pmic-arb.c`, FreeBSD's `qcom_spmi`: the APID map in the
core region, the owners in cnfg, reads through this EE's observer
channel, writes through the channel itself), the regions by the device
tree's `reg-names`, the EE from `qcom,ee`. The gauge and the ADC from
the device tree's `fxtec,pro1x-adc-battery` (pro1x-maemo-leste's Linux
driver `pro1x-adc-battery.c`): the PMI632's fuel gauge at 2:0x4800, its
burst-averaged current (LAST_BURST_AVG_I at 0xc6, held while it is read,
152588 nA a step on the 5 A part), the ADC5 at 2:0x3100, channel
VBAT_SNS (0x84, pre-scaled 1/3, 1875 mV over 0x70e4). Read only with the
power test or `kolibri.battery`.

## Stage 5: the standby, the power key, the wake-up by a key

Without it a standby (the power test's) ended only with the lifetime's
reset. Now the power key starts and ends one at any time:

**The power key** (`pon_init`, `pon_isr`): the PM6125's PON
(`qcom,pm8998-pon` at 0x800 of slave 0) has two keys in the Pro1X's tree,
`qcom,pm8941-pwrkey` (KPDPWR_N, its interrupt 0) and `qcom,pm8941-resin`
(the volume-down key, interrupt 1), `interrupts = <0 0x8 0|1
IRQ_TYPE_EDGE_BOTH>`; their state is RT_STS (0x810) bits 0 and 1
(Linux's `pm8941-pwrkey.c`). A peripheral's interrupt is configured in
its own registers (QPNPINT: SET_TYPE 0x11 edge, POLARITY_HIGH/LOW 0x12/13
both, LATCHED_CLR 0x14, EN_SET 0x15) and reaches the processor through
the SPMI arbiter (version 5, Linux's `spmi-pmic-arb.c`): its channel's
ACC_ENABLE, IRQ_STATUS and IRQ_CLEAR (chnls + 0x10000 * channel + 0x100,
0x104, 0x108), this EE's ACC_STATUS bitmap (the `intr` region, 0x3f00000
+ 0x10000 * EE), and one interrupt for all of them, `periph_irq`: in the
Pro1X's tree `interrupts-extended = <&mpm 86 IRQ_TYPE_LEVEL_HIGH>`, the
MPM's pin 86, which the MPM's `qcom,mpm-pin-map` (`<86 183>`) puts on GIC
SPI 183 (INTID 215). The channel map's bit 24 says whether a channel's
owner gets its interrupts. As with the TLMM, every PMIC interrupt Linux
left on for this EE is taken out of the arbiter's line first (ACC_ENABLE
0, `spmi_irq_quiet`: the charger's, the gauge's, the RTC's, the
volume-up GPIO's ...), and PON's other interrupts off (EN_CLR). A release
seen without its press (both edges between two looks) is a short press,
as in Linux. If the line stayed up with nothing to take (1000 times in a
row), the interrupt is switched off and said so.

**The standby** (`standby_enter`, `standby_leave`): the power key starts
it (as the power test does after 10 s): the display off, the programs'
timeouts on whole seconds, the SoC's sleep (the sleep set, the MPM). The
power key again, the volume-down key, any key of the keyboard, the
modifiers or Fn end it: the display on, the timeouts as before. The key
that ends it is not passed on (neither its press nor its release);
touches are read and dropped while the display is off.

**The wake-up from the SoC's sleep:** while the SoC sleeps (vlow: XO
off; vmin: CX and MX at retention) the GIC sees nothing; the MPM wakes
the SoC by its timer or by one of its pins, then raises its own
interrupt (the `qcom,mpm` node's, SPI 197 rising: "triggered by the RPM
when the system resumes", Linux's `qcom_mpm_handler`). So before the
cluster's collapse the vMPM gets a wake set (`mpm_wake_pin`, the types
as Linux's `qcom_mpm_set_type`: an edge's bits, POLARITY for a high
level; the other pins' types left alone):

| pin | what | type |
| --- | --- | --- |
| 86 | the SPMI arbiter: the power key, the volume keys | high level |
| 46, 9, 39, 53 | GPIO 93 Home, 83 right Shift, 84 left Ctrl, 19 left Alt (the gpio-keys) | both edges |

The GPIOs' pins are Linux's `sm6115_mpm_map` (`pinctrl-sm6115.c`; the
MPM does not take over the TLMM's interrupts on the SM6115, it only
wakes: `IRQ_DOMAIN_FLAG_QCOM_MPM_WAKEUP`). The two Fn keys (GPIO 106,
112) and the keyboard expander's INT (107) have no MPM pin: a letter key
pressed while the SoC sleeps is seen at its next wake-up by time (in the
standby the shell's, a second), when its INT is still low (the AW9523B
holds it until its ports are read; `p_soc_wake` looks after every
hand-over). The touch screen's INT (GPIO 80) has a pin (21) but is not
in the set: a touch in a pocket should not wake it. After the wake the
MPM's status says which pin it was: the arbiter's interrupt comes by
itself (a level), the modifiers are looked at at once (their TLMM edge
may have been lost while the TLMM slept). `pro1x: awake (...)` counts
both kinds.

`kolibri.nopowerkey` leaves the PMIC's interrupts alone. `kolibri.keytest=S`
plays a press of the power key S seconds into each standby (QEMU: `make
-C tests/power qemu`, the `keytest` run: 5.0 wake-ups/s, 2.3 in the
standby, 5.2 after it; on the phone a standby that ends without hands).

## The device test (prepared, 2026-10-29)

Nothing of this has run on the phone yet (nor has KolibriOS itself: the
first device test ended in Linux). From a running Maemo Leste, as root,
the kernel in `/host/kolibri/kernel.bin` (`docs/pro1x.md`), without the
modem (no `-m`, `kolibri.nomodem`: it runs on, on its own firmware), in steps, each one ending with the watchdog's reset into
the boot menu, then `tools/pro1x-read.py` for the console (the power
key, stage 5, is set up in every step; `kolibri.nopowerkey` added leaves
the PMIC's interrupts as Linux left them, if a step hangs early):

1. `tools/pro1x-kexec.sh /host/kolibri/kernel.bin "kolibri.nomodem kolibri.lifetime=60 kolibri.psci=off"`: the
   tick (INTID 19), tickless idle, the input by interrupt. Expected:
   `timer: 19200000 Hz, INTID 19, tickless idle`, `timer: broadcast:
   frame 0 at ...f121000, INTID 40, 19200000 Hz`, `psci: 1.x over smc,
   the firmware's mode (OSI there), extended state ids; CPUs off: fe of
   8`, the three states, `psci: off (kolibri.psci=off): wfi`, `rpm: left
   by Linux: vmin ...` (the RPM's counts readable), `pro1x: input
   interrupts: N GPIOs through INTID 259 (...)`, keys and touch working,
   `pro1x: lifetime over` after 60 s. A console that ends after
   `running` and a reset after about 32 s: no tick (the timer's
   interrupt), the watchdog not fed.
2. `... "kolibri.nomodem kolibri.powertest=3 kolibri.displayon"` (PSCI on):
   `psci: ... OS-initiated mode (OSI there) ...`, `power:` lines with
   `core`/`cluster` counts above 0 and `resumed` close to them, `pro1x:
   SPMI: arbiter 5.x, EE 0, ...`, `pro1x: battery: gauge at 2:4800 (5 A),
   ADC at 2:3100, channel 84 (1/3)`, the battery's lines. A hang after the first idle (a reset at
   about 32 s, the console ending at `running`): the way back from the
   power-down (cpu_resume) or the broadcast timer; then once more with
   `kolibri.psci=core`.
3. `... "kolibri.nomodem kolibri.powertest=10"` unplugged: the standby: `pro1x: display
   off: brightness 0, display off done, sleep in done, timing engine 1
   -> 0`, the sleep set (`rpm: the sleep set: 12 of 12 resources at 0`),
   the MPM's hand-overs, and whether `vlow`/`vmin` move; the battery's
   mean current against Leste's 160 mA.
4. The power key (stage 5), `... "kolibri.nomodem kolibri.lifetime=180"`, unplugged; at
   about a minute press the power key (the display goes off), after 20 s press it again
   (on), then once more off and a letter key (on), then off and left Shift (on). Expected:
   `pro1x: SPMI: arbiter 5.x, EE 0, ...`, `pro1x: power key: PON at 0:800, interrupt 0,
   volume down 1, through INTID 215 (MPM pin 86); N of Linux's PMIC interrupts quieted`
   (N: what Linux left on, some), then for each press `pro1x: standby (the power key)`,
   `pro1x: display off: brightness 0, display off done, sleep in done, timing engine 1 ->
   0`, the first time `rpm: the sleep set: 12 of 12 resources at 0`, `pro1x: the SoC's
   sleep: the sleep set voted, the MPM handed over before the cluster's collapse`, `pro1x:
   MPM wake-ups: SPMI (pin 86) GPIO 93 (pin 46) GPIO 83 (pin 9) GPIO 84 (pin 39) GPIO 19
   (pin 53); no MPM pin: GPIO 106 112 107`, and `pro1x: awake (the power key) after 20 s;
   MPM wake-ups: ...`, `pro1x: display on: sleep out done, display on done, brightness 127`;
   `pro1x: awake (a key) ...` for the letter key and for Shift. The key that woke it does
   not reach the shell. Failures: no `power key:` line, or `its interrupt not set up (-5:
   not this EE's)` (the PON channel's interrupts another EE's: then the volume and power keys
   stay Linux's only); `the SPMI arbiter's interrupt stays up with nothing pending: off`;
   a reset at about 32 s after a press (the interrupt storming: then `kolibri.nopowerkey`
   and the console). `MPM wake-ups: 0 by keys, 0 by the SPMI arbiter` with the display
   back on means the SoC was not asleep when the key came (the GIC saw it directly), fine
   too.
5. The standby without hands: `... "kolibri.nomodem kolibri.powertest=3 kolibri.keytest=60"`:
   the power test's standby at 10 s, `pro1x: awake (the power key, played) after 60 s`,
   the display on again, the rest of the test with the display on (the battery's current
   up again).
