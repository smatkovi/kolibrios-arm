# Networking: the Pro1X and QEMU

KolibriOS for ARM has no network yet. This document is what the
Pro1X offers, what each way onto a network would take under KolibriOS,
what was chosen, and what exists so far. The other half is the messengers
built on top of it: [messengers.md](messengers.md).

## What the phone has (Maemo Leste, kernel 7.1.0-pro1x.3, read on the phone)

| Piece | Linux on the Pro1X |
| --- | --- |
| Wi-Fi | `wifi@c800000`, `qcom,wcn3990-wifi` (the SM6115's WCN3990-class core, `chip_id 0x120`, calibration variant `Fxtec_QX1050`), driver `ath10k_snoc`: 12 copy-engine interrupts (SPI 358-369), one register window (`membase`, 32 KiB), stream `0x1a0` on the apps SMMU (`iommu@c600000`), a DMA-type IOMMU group, `qcom,msa-fixed-perm`. Rails `vdd-0.8-cx-mx`, `vdd-1.8-xo`, `vdd-1.3-rfa`, `vdd-3.3-ch0` from the PMIC through the RPM |
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
for testing the messengers on the phone before the Wi-Fi exists. Nothing
of the three is written yet.

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
on virtio-mmio (the transport QEMU's `virt` board has; no PCI in this
kernel) and user networking (`-netdev user`): QEMU's DHCP server gives
10.0.2.15, its DNS is 10.0.2.3, the host is 10.0.2.2.

The kernel's self test (`kolibri.nettest`) on the serial console: the
DHCP lease, a DNS lookup, a TCP connection to the host
(`make run-net` starts a small server there).
