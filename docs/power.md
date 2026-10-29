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
| 2 | CPU idle through PSCI | | |
| 3 | the RPM's sleep set, the MPM | | |
| 4 | the display off, the battery's current in the log | | |

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
(SPI 227). Every GPIO interrupt Linux left enabled is masked first (an
unknown one would hold the summary interrupt), the reserved GPIOs
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
