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

The network (SSID and passphrase) comes from a file the device test
provides: `tools/pro1x-kexec.sh -w FILE` puts it into the initrd as the
image `wifi` (never in the repository).

## The Wi-Fi so far: stages

| Stage | What | Where | Tested |
| --- | --- | --- | --- |
| 0 | random numbers from the hardware: the SoC's PRNG (`qcom,prng-ee` at 0x1b53000; its AHB clock voted on in the GCC, bit 13 of 0x79004, on when bit 31 of 0x21004 clears), QEMU's virtio-rng; getrandom's ChaCha20 key and lwIP's numbers from it | `kernel/rng.c`, `kernel/platform/pro1x.c`, `kernel/net/virtio_rng.c` | QEMU (virtio-rng, `nettest` with TLS); the PRNG needs the phone |
| 1 | the RPM: its GLINK edge in the message RAM (`qcom,rpm-msg-ram`, 0x45f0000, 28 KiB: a table of contents, the FIFOs `ap2r` and `r2ap`), the channel `rpm_requests` Linux left open (its id read from Linux's requests still in the FIFO), SMD-RPM requests; `kolibri.wifi` switches on (`swen`) the PM6125's L8, L16, L17 and L23, the rails of the WLAN part, at the voltages the RPM has from Linux | `kernel/modem/rpm.c` | `tests/modem` against an RPM in RAM (Linux's channel id found, acknowledgements, a refusal, the FIFOs wrapped); the phone next |
| 2 | WLFW (service 69 on the modem, node 0 port 77) over the existing QRTR: the indication register (ath10k's client id; the answer says whether the firmware is ready, which it is after a kexec: Linux's re-probe path), the capabilities (chip, board, SoC, firmware version and build) | `kernel/modem/wlfw.c` | `tests/modem` against a WLFW service in the fake modem; the phone next |
| 3 | the copy engines (12 engines at 0x240000 + 0x1000 * id of `membase`, their rings in RAM), the SMMU's context bank of stream 0x1a0 bypassed as the display's, then WLFW's `WLAN_CFG` (the pipes, the service map, the shadow registers: ath10k's `snoc.c` tables) and `WLAN_MODE` (mission) | `kernel/modem/ce.c`, `wlfw.c` (`wlfw_cfg_msg`, `wlfw_mode_msg`) | the rings and the messages: `make -C tests/wifi` (against a simulated engine block), `make -C tests/modem` (the QMI messages); not sent on the phone yet, nor the SMMU |
| 4 | the bring-up as `ath10k_core_start` orders it: WLFW's `WLAN_CFG` and `WLAN_MODE` sent, the engines set up, HTC (READY, HTT's service, then WMI's, SETUP_COMPLETE_EX), WMI-TLV (SERVICE_READY, the host memory it asks for, INIT, READY with the MAC address), HTT (VERSION_REQ, FRAG_DESC_BANK_CFG, RX_RING_CFG, AGGR_CFG), `ath10k_start`'s device parameters, the channel list | `kernel/modem/wifi.c` (the driver), `htc.c`, `wmi.c`, `htt.c`, `wlfw.c` (`wlfw_wlan_enable`), `kernel/net/wifi_dev.c`, `kernel/platform/pro1x.c` (`wifi_ready`) | `make -C tests/wifi` (`station`: against a simulated WCN3990), `make -C tests/modem` (WLAN_CFG and WLAN_MODE through the fake modem), `make -C tests/wifi qemu` (the kernel's side in QEMU up to the engines) |
| 5 | a station vdev, the scan (the BSS of the configured SSID: CCMP, PSK), vdev start, the AP's peer, open authentication and association (`MGMT_TX_SEND` by reference, or HTT), `PEER_ASSOC`, `VDEV_UP`; WPA2-PSK: the PMK (PBKDF2), the 4-way handshake over the data path, the keys installed (`VDEV_INSTALL_KEY`, `SEC_IND`), the peer authorized; the group key handshake; deauthentication and connection loss: scanning again | `wifi.c`, `wlan.c` (802.11 frames), `wpa.c` (the supplicant), `crypto.c` | `tests/wifi`: `crypto` (published vectors, OpenSSL), `station` (an AP with OpenSSL's crypto: the installed keys checked by CCMP both ways; a wrong PSK; no AP) |
| 6 | HTT's data path: Ethernet to native Wi-Fi frames in `TX_FRM` (gathered on CE4), `RX_IN_ORD_PADDR_IND`'s buffers from the RX ring to Ethernet, lwIP's interface (DHCP when the link is up) | `wifi.c`, `htt.c`, `wlan.c`, `kernel/net/wifi_dev.c`, `kernel/net/net.c` | `tests/wifi` `station`: lwIP above the driver takes a lease, answers a ping, an ARP broadcast with a new group key |

`kolibri.wifi` (with `tools/pro1x-kexec.sh -m -w FILE`: the modem, so
QRTR, and the network) runs all of it on the phone: once WLFW says the
firmware is ready, the driver starts. Nothing of stages 3-6 has run on the
phone yet (below: the device test).

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

### Stages 4-6: the driver

`kernel/modem/wifi.c` is what Linux's ath10k (`snoc.c`, `core.c`,
`htc.c`, `htt*.c`, `wmi*.c`, `mac.c`) does with the firmware, and what
mac80211 and wpa_supplicant do above it for one WPA2-PSK network; plain C
over hooks (`struct wifi_hw`: the register window, DMA memory, WLFW, the
clock, random numbers, the frames up), the same on the phone and in
`tests/wifi`. Everything runs from `wifi_poll`: the engines' interrupts
(`kolibri.wifi=irq`) or the tick (by default: every 10 ms until the
station is up, every 20 ms after). The facts and where they are from:

- **The start** (`qmi.c`'s `ath10k_qmi_event_server_arrive`): with the
  firmware ready at the indication register (after a kexec from Linux:
  Linux's own re-probe path), Linux goes straight to the driver; no host
  capabilities, no MSA, no board data download (`board-2.bin`): the
  firmware has them from Linux's boot. A firmware that is not ready (the
  modem restarted) would need that whole path: not done, logged.
  `ath10k_snoc_hif_power_up`: `WLAN_CFG`, `WLAN_MODE` mission, then the
  engines, then `ath10k_core_start`: the buffers posted, HTC's READY (one
  WMI credit: `use_fw_tx_credits` false), HTT connected before WMI,
  SETUP_COMPLETE_EX, SERVICE_READY.
- **The firmware's files** (`firmware-5.bin` of the phone's
  `/lib/firmware/ath10k/WCN3990/hw1.0`, 60 bytes): WMI op version TLV (4),
  HTT op version TLV (3), features WOWLAN, MGMT_TX_BY_REF, NON_BMI. So
  management frames go by `MGMT_TX_SEND` with their address (when the
  firmware has `WMI_SERVICE_MGMT_TX_WMI`, 76; else by HTT `TX_FRM` in
  management mode, as `ath10k_mac_tx_h_get_txpath`).
- **INIT's host memory**: `ath10k_wmi_event_service_ready_work`'s sizes
  (units times the size rounded to 4; 34 for a "number of peers" request,
  5 for "number of vdevs"), below 4 GiB (no
  `WMI_SERVICE_SUPPORT_EXTEND_ADDRESS` used).
- **The layouts**: every WMI-TLV command and event structure, tag, id and
  offset used, the HTT messages, and the WCN3990's RX descriptor
  (`htt_rx_desc_v2`: 360 bytes; attention at 4, MPDU start at 16, MSDU
  start at 28, MSDU end at 48, the received 802.11 header at 296) were
  read out of Linux 7.1's `wmi.h`, `wmi-tlv.h`, `htt.h` and `rx_desc.h` by
  compiling them on the PC (they are packed, little endian), not typed
  from memory.
- **The RX ring** (`ath10k_htt_rx_alloc`, `_ring_fill_n`, the 64-bit
  `RX_RING_CFG`): 2048 entries of 64-bit addresses, 2 KiB buffers, the
  host's fill index in a word the firmware reads; `fw_idx_init_val` is that
  index as filled before `RX_RING_CFG` (Linux fills first). With
  `WMI_SERVICE_RX_FULL_REORDER` the firmware hands buffers back by address
  (`RX_IN_ORD_PADDR_IND`); the ring-order path without it is not done.
- **TX** (`ath10k_htt_tx_64` with `continuous_frag_desc`): a descriptor
  with HTC's header in front (transfer id 0, no HTC send path), the
  frame's first bytes (prefetch, 46 rounded up to 4) gathered behind it
  on CE4 (`ce_send_sg`: the gather flag in the descriptor's flags and,
  with the send flag, in the address's high word, the write index after
  the last piece, as `_ath10k_ce_send_nolock_64`), the frame itself by the
  fragment bank's descriptor; done by `TX_COMPL_IND`.
- **The station** (`mac.c`): `ath10k_add_interface` (vdev create, power
  save off), `ath10k_hw_scan` with `ath10k_wmi_start_scan_init`'s values
  (the TLV firmware's inverted probe-request filter flag), the beacons
  and the AP's answers by MGMT_RX (dropped only for a CRC, decryption or
  key cache error, as `ath10k_wmi_event_mgmt_rx`: EXT_INFO, 0x40, is
  extension data), vdev start on the BSS's channel
  (`ath10k_vdev_start_restart`; the mode by the channel as
  `chan_to_phymode`: 11g, 11a, channel 14's 11b), the AP's peer and its
  `PEER_MAP` (`ath10k_wait_for_peer_created`), `ath10k_peer_assoc_prepare`
  for a legacy peer (auth, `need_ptk_4_way` with an RSN element, the
  rates as `ath10k_mac_bitrate_to_rate`, 11a/b/g, listen interval 1),
  `VDEV_UP` with the AID; the keys as `ath10k_set_key` for a station (the
  BSSID as the peer for both, pairwise index 0, the group key's index
  from the KDE), done when HTT's `SEC_IND` comes (`install_key_done`),
  then `WMI_PEER_AUTHORIZE`.
- **WPA2-PSK** (`wpa.c`, wpa_supplicant's `rsn_supp/wpa.c` and
  `wpa_common.c`): the PMK by PBKDF2-SHA1 (4096, the SSID), message 1's
  ANonce and a new SNonce from the SoC's PRNG (stage 0), the PTK by the
  PRF (KCK, KEK, TK), message 2 with our RSN element, message 3's MIC,
  ANonce and RSN element (the beacon's) checked, its key data unwrapped
  (AES key wrap with the KEK) for the GTK, message 4; the keys after
  message 4's TX completion (else the firmware would encrypt it); EAPOL
  that comes before the association's answer kept (wpa_supplicant's
  `pending_eapol_rx`: they come on different engines); the group key
  handshake. CCMP for both ciphers, AKM PSK, no PMF (an AP that requires
  it is skipped, one that offers it is joined without).
- **802.11** (`wlan.c`): beacons and probe responses read; the probe
  request's rates; open authentication; the association with the BSS's
  rates and our RSN element (no WMM, no HT: 802.11a/b/g rates); native
  Wi-Fi data frames (`ATH10K_HW_TXRX_NATIVE_WIFI`, the decapsulation INIT
  asks for: a 3-address header without QoS control, then LLC/SNAP) to and
  from Ethernet as `ath10k_htt_rx_h_undecap_nwifi` and
  `ath10k_tx_h_nwifi` do; the Protected bit once the keys are in (as
  mac80211 sets it for frames the hardware encrypts).

Not as Linux, on purpose: the RX ring holds 256 buffers (Linux 2047), the
engines 32 (CE1), 16 (CE2), 4 (the other receive engines) instead of
filling them; 64 TX ids instead of 1056; the regulatory domain is not set
(`ath10k_regd_update`'s `PDEV_SET_REGDOMAIN` needs ath's regulatory tables
for the EEPROM's code, which SERVICE_READY reports and the console shows;
the firmware keeps its own) and the channel list is the EU's (2.4 GHz
1-13 at 20 dBm; 5 GHz 36-64 at 23 dBm and 100-140 at 30 dBm, passive and
DFS from 52 on); no power save; no A-MSDU, no aggregation set up by the
host; one network, no roaming.

**The tests on the PC** (`make -C tests/wifi`):

- `crypto`: SHA-1, HMAC-SHA1 (RFC 2202), PBKDF2 (RFC 6070), the PMK (IEEE
  802.11 J.4.2), the PRF (J.3.2), AES-128 (FIPS 197), the key wrap (RFC
  3394), and 200 random cases against OpenSSL; the RSN elements taken and
  refused (TKIP group, SAE only, MFP required). 22 checks.
- `station`: the whole driver against a simulated WCN3990 (the engines'
  registers as `test.c` has them; HTC, WMI-TLV and HTT as above; CCMP with
  the keys the driver installed: OpenSSL's AES-CCM) and an access point
  with three BSSs on channel 6 (the network with CCMP, the same SSID with
  a TKIP group cipher and more SNR, another SSID): the scan, the join, the
  4-way handshake checked at the AP with OpenSSL (its PTK, its MICs), the
  installed keys equal to the AP's (frames decrypt both ways), lwIP above
  the driver takes a DHCP lease (192.168.77.100), answers a ping, takes a
  new group key (the group key handshake) and answers an ARP broadcast
  encrypted with it, is deauthenticated and joins again. 401 checks;
  `station wrongpsk` (management frames by HTT; message 2's MIC wrong at
  the AP, never up, tried again), `station noap` (scanning again and
  again).
- `qemu`: the kernel's side in QEMU (the Pro1X's tree from
  `tests/power/mkdtb.py --wifi`: a `qcom,wcn3990-wifi` node whose register
  window is RAM, the fake modem's WLFW, the network in the initrd): the
  network read, WLAN_CFG and WLAN_MODE over the fake QRTR, 4 MiB of
  uncached DMA memory below 4 GiB, the engines set up, then waiting for
  READY, which no firmware sends there.

## The device test (prepared, not run: the phone until 2026-10-31)

Wi-Fi needs QRTR, so the modem: `pro1x-kexec.sh -m`. **`-m` in its
current form (Linux's IPCRTR taken over, `kolibri.ipcrtr=adopt`) has not
run on the phone either**; the earlier form hung Linux (2026-10-29: the
unbind's D state, a power-key reset). So the modem first, then the Wi-Fi,
each step ending with the watchdog's reset into the boot menu, then
`tools/pro1x-read.py` for the console. On Leste, as root, the kernel in
`/host/kolibri/kernel.bin`:

0. The modem alone (docs/pro1x-modem.md): `tools/pro1x-kexec.sh -n -m`,
   then `tools/pro1x-kexec.sh -m /host/kolibri/kernel.bin
   "kolibri.lifetime=120"`. Expected: `glink:`/`modem:` lines, the modem
   registered. A hang in Linux before the jump: as on 2026-10-29, the
   power key; then no Wi-Fi test until `-m` works.
1. Stages 0-2 without the driver (no `-w`): `tools/pro1x-kexec.sh -m
   /host/kolibri/kernel.bin "kolibri.wifi kolibri.lifetime=90"`.
   Expected: `rng: the SoC's PRNG`, `rpm: L8 on ... done` (and L16, L17,
   L23), `wlfw: the WLAN firmware's service at 0:77`, `wlfw: registered,
   the firmware is ready`, `wlfw: chip 120 family 4007 board ff soc
   40670000, firmware 324103d6 (...) ...WLAN.HL.3.2.4`, then `pro1x:
   Wi-Fi: no network in the initrd (pro1x-kexec.sh -w FILE)`.
2. The network: a file on the phone, e.g. `/root/kolibri/wifi.conf`
   (root only):

   ```
   ssid=<the network's name>
   psk=<its passphrase>
   ```

   `tools/pro1x-kexec.sh -m -w /root/kolibri/wifi.conf
   /host/kolibri/kernel.bin "kolibri.lifetime=180"`. Expected, in order
   (what `tests/wifi` prints, with the phone's values):

   ```
   pro1x: initrd at ..., N KiB (KOSEFS01)
   wlfw: registered, the firmware is ready
   pro1x: Wi-Fi at 0x000000000c800000 (8192 KiB), 12 interrupts from INTID 390, stream 0x1a0
   pro1x: stream 0x1a0: context bank N, SCTLR 0x... -> 0
   wifi: DMA 0x... (4 MiB), engines by polling, network "<SSID>"
   wifi: starting: WLAN_CFG, WLAN_MODE (mission)
   wlfw: WLAN_CFG taken
   wlfw: WLAN_MODE mission
   wifi: copy engines up, waiting for HTC READY
   wifi: HTC ready: N credits of N bytes (one used), N endpoints
   wifi: HTC: HTT on endpoint N (CE4/CE1), WMI on N (CE3/CE2)
   wifi: SERVICE_READY: firmware build ..., ... regulatory domain X; RX full reorder yes, management frames by WMI
   wifi: host memory N: N KiB at ...
   wifi: READY: the MAC address xx:xx:..., ABI 53
   wifi: HTT 3.x
   wifi: scanning 32 channels for "<SSID>"
   wifi: joining xx:xx:... on NNNN MHz (channel N, SNR N, ...)
   wifi: the AP's peer, id N
   wifi: authenticated (open system)
   wifi: associated, AID N
   wifi: 4-way handshake: message 2 out
   wifi: 4-way handshake: message 4 out
   wifi: the pairwise key installed
   wifi: the group key N installed
   wifi: up: "<SSID>" (xx:xx:...), WPA2-PSK CCMP
   net: wcn3990, MAC xx:xx:..., DHCP
   net: 192.168.x.y, mask ..., gateway ..., DNS ...
   wifi: up; N WMI commands, ... (at the lifetime's end)
   ```

   The MAC address READY reports should be the one Linux's `wlan0` had.
3. With the network: the same with `kolibri.nettest=80` added (the
   kernel's self test: a DNS lookup of example.com, an HTTP request to the
   gateway's port 80; a router without a web server answers with an
   error, which is fine), then the messengers (docs/messengers.md) on it.
4. The interrupts: step 2 with `kolibri.wifi=irq` instead of the default
   polling (the engines' SPIs 358-369 level-triggered; a storm would end
   in the watchdog's reset at about 32 s after `copy engines up`: then
   polling stays).

Where it can fail, and what the console then says:

- **The register window** (`membase`, 0xc800000): readable only while the
  WLAN part has its rails (stage 1) and after `WLAN_MODE`; a bus hang at
  `copy engines up` (the console ending there, the watchdog's reset) would
  mean a missing clock or rail vote after the kexec (Linux's `ath10k_snoc`
  has no clocks on the SM6115's node, only the four rails).
- **The SMMU** (stream 0x1a0): no `stream 0x1a0: context bank` line, or
  the firmware's DMA into the rings going nowhere (no `HTC ready` after
  `copy engines up`: `no answer in time (in HTC ready)`): the bypass did
  not take; Linux's mapping is a DMA domain (translation), so without the
  bypass the addresses are IOVAs.
- **DMA memory**: uncached, below 4 GiB (`wifi: DMA ...`); a firmware that
  wants the `memory-region` (MSA, `wlan_msa_mem`) for something of the
  host's would crash (`wlfw: indication 0x..`; then the modem's console
  lines).
- **The interrupts**: the engines raise them once set up
  (copy-complete), polled or not; in the default polling their SPIs stay
  masked (`gic3_init` masks every SPI, so nothing Linux left enabled
  fires); with `kolibri.wifi=irq` they are taken level-triggered and
  cleared before each engine's completions.
- **The firmware's state**: `wlfw: the firmware is not ready` (the modem
  restarted under Linux): the board data path, not done.
- **The network**: `"<SSID>" not found (N networks seen)` (the channel
  list, a hidden SSID: the scan probes for it), `skipped: ...` (TKIP,
  SAE/WPA3, MFP required), `EAPOL dropped: message 3's MIC is wrong (the
  PSK?)`, `association refused (status N)` (an N-only AP: no HT here).
- **The regulatory domain**: not set; if the firmware's own refuses
  channels or power, SERVICE_READY's `regulatory domain X` says which code
  the board has, for the table to be added.

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
