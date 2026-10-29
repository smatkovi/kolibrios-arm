# Networking: the Pro1X and QEMU

KolibriOS for ARM has no network yet. This document is what the
Pro1X offers, what each way onto a network would take under KolibriOS,
what was chosen, and what exists so far. The other half is the messengers
built on top of it: [messengers.md](messengers.md).

## What the phone has (Maemo Leste, kernel 7.1.0-pro1x.3, read on the phone)

| Piece | Linux on the Pro1X |
| --- | --- |
| Wi-Fi | `wifi@c800000`, `qcom,wcn3990-wifi` (the SM6115's WCN3990-class core, `chip_id 0x120`, calibration variant `Fxtec_QX1050`), driver `ath10k_snoc`: 12 copy-engine interrupts (SPI 358-369), one register window (`membase`: `reg = <0 0xc800000 0 0x800000>`, 8 MiB; the copy engines at 0x240000-0x24c000 in it, after `hw.c`'s `wcn3990_regs`), stream `0x1a0` on the apps SMMU (`iommu@c600000`), a DMA-type IOMMU group, `qcom,msa-fixed-perm`. Rails `vdd-0.8-cx-mx`, `vdd-1.8-xo`, `vdd-1.3-rfa`, `vdd-3.3-ch0` from the PMIC through the RPM |
| its firmware | not on the apps processor: `wlanmdsp.mbn` (WLAN.HL.3.2.4) runs on the modem's DSP, which fetches it from `/lib/firmware` through TFTP over QRTR (`tqftpserv`, service 4096). `firmware-5.bin` (60 bytes) and `board-2.bin` are what the host reads. The host talks to it through the QMI service WLFW (69), offered by the modem on node 0, port 77 |
| mobile data | `ipa@5840000` (IPA with its GSI, `qcom,gsi-loader = "self"`: the apps processor loads `ipa_fws.mbn` through TrustZone), interface `rmnet_ipa0`; set up over QMI: WDS (1), the data port mapper (47), IPA control (49, offered by both sides) |
| USB | `usb@4e00000` (DWC3, peripheral mode, `g_cdc`: RNDIS `usb0` = 172.16.42.1) |
| user space | `wpa_supplicant` for WPA2, `tqftpserv`, `rmtfs` |

What a kexec leaves (Linux's shutdown hooks in the running kernel's sources):

- **Wi-Fi**: `ath10k_snoc_shutdown` unregisters the device: the firmware is told
  `WLAN_MODE_OFF` over QMI, the copy engines are freed, the clock and the
  four rails are released. The firmware itself stays loaded on the modem
  (the modem keeps running through the kexec, see
  [pro1x-modem.md](pro1x-modem.md)). Linux can bring it back from there
  without the modem restarting (`rmmod`/`modprobe ath10k_snoc` does), so
  KolibriOS can as well, but the rails have to be switched on again
  through the RPM first.
- **Mobile data**: `ipa_remove` tears the IPA down completely; bringing it
  up again needs the GSI firmware loaded through TrustZone (SCM PAS) and
  the whole IPA/GSI programming.
- **USB**: `dwc3_qcom_remove` turns the controller, its PHY and its clocks off.

All three are behind the apps SMMU in translating (DMA) domains. KolibriOS
already switches the display's context banks to bypass
(`kernel/platform/pro1x.c`, `smmu_bypass`); the same is needed for the
stream of whatever device does DMA (Wi-Fi `0x1a0`).

## The ways onto a network

| | Wi-Fi (WCN3990) | Mobile data (IPA) | USB to a PC (gadget) |
| --- | --- | --- | --- |
| what KolibriOS must do | RPM votes for the four rails; SMMU bypass; WLFW over the existing QRTR/QMI (indications, host capabilities, board data, `WLAN_CFG` with the copy-engine layout, `WLAN_MODE` on); the copy engines (rings in RAM, 12 pipes); HTC, WMI (scan, vdev/peer, keys) and HTT (the data frames); the 802.11 part ath10k leaves to mac80211 (probe, authentication, association, 802.11 <-> Ethernet frames); WPA2-PSK (PBKDF2 for the PMK, the 4-way handshake with EAPOL, AES key unwrap, keys installed through WMI: the hardware encrypts) | QMI WDS (start a network interface: the APN, the address), the data port mapper, the IPA control service the apps processor offers; the GSI firmware through TrustZone; IPA and GSI themselves (endpoints, channels, rings, routing/filter tables), QMAP framing | clocks and power domain (GCC), the USB 2 PHY, DWC3 in device mode, a CDC NCM or ECM function; the PC routes (NAT) |
| reused | QRTR, QMI from the modem stack | QRTR, QMI, the running modem's registration | nothing |
| size (estimate) | 8000-10000 lines | 6000-8000 lines, plus the firmware loading | 1500-2000 lines |
| firmware | already on the modem | `ipa_fws.mbn` (the phone's own, from its file system) | none |
| for whom | the target: the phone on its own, at home and on the road where Wi-Fi is | the phone on its own anywhere; later | a test harness: debugging over a cable, not an end state |

**Chosen:** the network stack goes into the kernel now (below), tested in
QEMU. On the phone, Wi-Fi is the target, mobile data comes after it, and
the USB gadget is the optional first step if a cable network is wanted
for testing the messengers on the phone before the Wi-Fi exists. The
Wi-Fi's first stages are written (below).

The Wi-Fi in steps, each testable on the phone by its log:

1. RPM: the regulator votes over the RPM's GLINK edge (SMD-RPM key/value
   requests). Read-only check first: what the RPM reports for the four LDOs
   after a kexec.
2. WLFW: connect to service 69 on node 0, the sequence of
   `ath10k_qmi_event_server_arrive` (indication register, host capability,
   MSA info and ready, capabilities, board data download from
   `board-2.bin` by the board id the firmware reports, `WLAN_CFG`,
   `WLAN_MODE` mission mode); the firmware's "ready" indication.
3. Copy engines and HTC: the rings, the interrupts, the HTC services
   (WMI control, HTT data); WMI's ready event with the MAC address.
4. Scan (WMI start scan, the beacons and probe responses as management
   frames), then association to an open network, then WPA2-PSK.
5. HTT: Ethernet frames both ways, into lwIP's netif.

The passphrase comes from the kernel command line or a ramdisk file
written by `tools/pro1x-kexec.sh` from Leste's own connection (the user's
choice; never in the repository).

## The Wi-Fi so far: stages

| Stage | What | Where | Tested |
| --- | --- | --- | --- |
| 0 | random numbers from the hardware: the SoC's PRNG (`qcom,prng-ee` at 0x1b53000; its AHB clock voted on in the GCC, bit 13 of 0x79004, on when bit 31 of 0x21004 clears), QEMU's virtio-rng; getrandom's ChaCha20 key and lwIP's numbers from it | `kernel/rng.c`, `kernel/platform/pro1x.c`, `kernel/net/virtio_rng.c` | QEMU (virtio-rng, `nettest` with TLS); the PRNG needs the phone |
| 1 | the RPM: its GLINK edge in the message RAM (`qcom,rpm-msg-ram`, 0x45f0000, 28 KiB: a table of contents, the FIFOs `ap2r` and `r2ap`), the channel `rpm_requests` Linux left open (its id read from Linux's requests still in the FIFO), SMD-RPM requests; `kolibri.wifi` switches on (`swen`) the PM6125's L8, L16, L17 and L23, the rails of the WLAN part, at the voltages the RPM has from Linux | `kernel/modem/rpm.c` | `tests/modem` against an RPM in RAM (Linux's channel id found, acknowledgements, a refusal, the FIFOs wrapped); the phone next |
| 2 | WLFW (service 69 on the modem, node 0 port 77) over the existing QRTR: the indication register (ath10k's client id; the answer says whether the firmware is ready, which it is after a kexec: Linux's re-probe path), the capabilities (chip, board, SoC, firmware version and build) | `kernel/modem/wlfw.c` | `tests/modem` against a WLFW service in the fake modem; the phone next |
| 3 | the copy engines (12 engines at 0x240000 + 0x1000 * id of `membase`, their rings in RAM), the SMMU's context bank of stream 0x1a0 bypassed as the display's, then WLFW's `WLAN_CFG` (the pipes, the service map, the shadow registers: ath10k's `snoc.c` tables) and `WLAN_MODE` (mission) | `kernel/modem/ce.c`, `wlfw.c` (`wlfw_cfg_msg`, `wlfw_mode_msg`) | the rings and the messages: `make -C tests/wifi` (against a simulated engine block), `make -C tests/modem` (the QMI messages); not sent on the phone yet, nor the SMMU |
| 4 | HTC (its services: WMI control, HTT data), WMI-TLV (init, the ready event with the MAC address, service ready) | `kernel/modem/htc.c`, `wmi.c` | `make -C tests/wifi`: READY, two services connected, SETUP_COMPLETE_EX, SERVICE_READY, INIT, READY with the MAC address, the credits, through the copy engines of the simulation |
| 5 | scan, a station vdev, authentication and association (management frames through WMI), WPA2-PSK (PBKDF2, the 4-way handshake, the keys installed through WMI) | to do | |
| 6 | HTT: Ethernet frames both ways into lwIP's netif, DHCP | to do | |

`kolibri.wifi` (with `tools/pro1x-kexec.sh -m`: the modem, so QRTR) runs
stages 0-2 on the phone; stage 2 switches nothing on in the firmware.

### Stages 3 and 4, prepared on the PC

What the host does with the WCN3990 above WLFW, after Linux's ath10k
(`ce.c`, `snoc.c`, `htc.c`, `wmi-tlv.c` of the Pro1X's 7.1 kernel; the
WCN3990's `hw_params` in `core.c`: `target_64bit`, `shadow_reg_support`,
`rri_on_ddr`, `use_fw_tx_credits` false, 33 peers):

- **The copy engines** (`kernel/modem/ce.c`, `ce.h`): the host's rings as
  `host_ce_config_wlan` (CE0 16 source entries for HTC control, CE1 512
  destination entries for HTT and HTC, CE2 64 for WMI, CE3 32 for WMI's
  commands, CE4 2048 for HTT's frames without interrupts, CE5 and CE9-11
  512, CE7 2 and 2, CE8 128; sizes powers of two), 16-byte descriptors
  (`ce_desc_64`: the address in 35 bits, its high bits and the gather flag
  in the second word, the length, the HTC endpoint as the transfer id in
  bits 4-15 of the flags), each engine programmed as
  `ath10k_ce_init_src_ring`/`init_dest_ring` do (base, size, the largest
  transfer in CTRL1, no byte swap, the watermarks) from the indices the
  engine has (after a kexec Linux's), the read indices in a word of RAM
  per engine (`ath10k_ce_alloc_rri`: CE_WRAPPER + 4/8, CTRL1 bit 19), the
  source rings' write index through the shadow registers (0x32000 + 4 *
  id), copy-complete interrupts where Linux has a callback (not CE4, CE6-8).
  As in Linux, a source ring's free entries count from the target's read
  index in RAM, so completions are taken before the ring goes round again.
- **WLAN_CFG and WLAN_MODE** (`wlfw_cfg_msg`, `wlfw_mode_msg`): the 12
  target pipes (`target_ce_config_wlan`, five words each: QMI's structure
  has no reserved word), the 21 service entries (`target_service_to_ce_map_wlan`
  with its terminating zeros, as `ath10k_snoc_wlan_enable` counts them),
  the 12 shadow registers (`target_shadow_reg_cfg_map`); no host version;
  then the mission mode (0).
- **HTC** (`kernel/modem/htc.c`): the 8-byte header, endpoint 0's control
  messages (READY: credits and their size, the extended form by its
  length; CONNECT_SERVICE with the credits for WMI control only, credit
  flow off for every other service; its response hands out the endpoint
  and the largest message; SETUP_COMPLETE_EX, 12 bytes), the credit
  reports in a received trailer. The WCN3990 does not use the firmware's
  credit count (`use_fw_tx_credits` false): WMI control has one credit,
  given back in a trailer.
- **WMI-TLV** (`kernel/modem/wmi.c`): the 4-byte command header, the TLV
  walk with Linux's minimum lengths, SERVICE_READY (the firmware's build,
  the ABI checked as Linux checks it, the channels, the service bitmap
  with four services a word as `WMI_SERVICE_IS_ENABLED` reads it, the
  memory requests), INIT (the ABI, the resource configuration's 44 words as
  `ath10k_wmi_tlv_op_gen_init` fills them for the WCN3990, the host memory
  chunks each with its TLV header), READY (the MAC address).

`make -C tests/wifi` runs them against a simulated target: the engines'
registers in RAM, written and read as the hardware does (a source ring's
write index only through its shadow register, the read indices in the
registers and the RAM word, the destination descriptors filled with their
lengths), the host's memory above 16 GiB (so the address's high bits
count), the firmware's HTC and WMI on top (READY, the connects, the
credit back in READY's trailer, INIT decoded field by field). 96 checks.

What the phone has to show next (needs the device): the engines'
registers readable at all once WLAN_MODE is on (Linux sends WLAN_CFG and
WLAN_MODE first, then sets the engines up: `ath10k_snoc_hif_power_up`),
the SMMU bypass for stream 0x1a0, uncached DMA memory below 2^35 for the
rings and buffers (and the `memory-region` the tree gives the Wi-Fi), the
12 interrupts (SPI 358-369, one per engine), and then the firmware's
real READY.
What the RAM console should show:

```
rng: the SoC's PRNG
pro1x: RPM: message RAM ...45f0000 (28 KiB), IPC bit 0
rpm: Linux's channel id N (M of its requests seen)
rpm: FIFOs of ... and ... bytes
rpm: L8 on at 0 uV: done          (and L16, L17, L23)
wlfw: the WLAN firmware's service at 0:77
wlfw: registered, the firmware is ready
wlfw: chip 120 family 4007 board ff soc 40670000, firmware 324103d6 (...) ...WLAN.HL.3.2.4...
```

## The stack: lwIP in the kernel

- **lwIP 2.2.1**, fetched by `tools/fetch-lwip.sh` (a pinned release,
  checked by its SHA-256) into `third_party/lwip`, not kept in the
  repository. `NO_SYS=1`: the raw API only, no threads. lwIP runs in the
  kernel's context only (interrupts are masked in the kernel, so there is
  no locking): from the network interrupt (frames in), from the timer tick
  (its timeouts, `sys_now` from the tick), and from system calls (the
  sockets of the Linux personality, [messengers.md](messengers.md)).
- **IPv4** with DHCP and DNS (lwIP's own resolver, for the kernel's self
  test; Go resolves by itself over UDP), TCP and UDP. IPv6 later.
- **Interfaces** (`kernel/net/`): one `struct netif` per device; the first
  is QEMU's virtio-net.

## QEMU

`make PLATFORM=arm64 run-net`: QEMU's `virt` with a `virtio-net-device`
on virtio-mmio, version 2 (`-global virtio-mmio.force-legacy=false`;
the kernel has no PCI) and user networking (`-netdev user`): QEMU's DHCP
server gives 10.0.2.15, its DNS is 10.0.2.3, the host is 10.0.2.2.

`kernel/net/virtio_net.c` (the card: two queues of 64 buffers),
`kernel/net/net.c` (lwIP's glue: the interface, DHCP, `sys_now`, the
timeouts from the tick), `kernel/net/lwipopts.h`.

The kernel's self test, `kolibri.nettest=<port>` on the command line
(`run-net` passes it, `NETTEST_PORT`, 18765 by default), on the serial
console: the lease, a DNS lookup, an HTTP request to the host's port,
answered by `python3 -m http.server --bind 127.0.0.1 18765` there:

```
net: virtio-net, MAC 52:54:00:12:34:56, DHCP
net: 10.0.2.15, mask 255.255.255.0, gateway 10.0.2.2, DNS 10.0.2.3
nettest: example.com is 172.66.147.243
nettest: TCP to 10.0.2.2:18765
nettest: connected
nettest: the host answered "HTTP/1.0 200 OK"
nettest: done
```
