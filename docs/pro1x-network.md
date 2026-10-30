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
| what KolibriOS must do | RPM votes for the four rails; SMMU bypass; WLFW over the existing QRTR/QMI (indications, host capabilities, board data, `WLAN_CFG` with the copy-engine layout, `WLAN_MODE` on); the copy engines (rings in RAM, 12 pipes); HTC, WMI (scan, vdev/peer, keys) and HTT (the data frames); the 802.11 part ath10k leaves to mac80211 (probe, authentication, association, 802.11 <-> Ethernet frames); WPA2 and WPA3 personal (PBKDF2 or SAE for the PMK, the 4-way handshake with EAPOL, AES key unwrap, keys installed through WMI: the hardware encrypts; PMF) | QMI WDS (start a network interface: the APN, the address), the data port mapper, the IPA control service the apps processor offers; the GSI firmware through TrustZone; IPA and GSI themselves (endpoints, channels, rings, routing/filter tables), QMAP framing | clocks and power domain (GCC), the USB 2 PHY, DWC3 in device mode, a CDC NCM or ECM function; the PC routes (NAT) |
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
| 0 | random numbers ([rng.md](rng.md)): one generator (ChaCha20, fast key erasure) seeded from the SoC's PRNG (`qcom,prng-ee` at 0x1b53000, read as Linux's qcom-rng reads it: its AHB clock voted on in the GCC around each read, bit 13 of 0x79004, CLK_OFF of 0x21004 waited for unless hardware gated) and kexec's `/chosen/rng-seed`, QEMU's virtio-rng; the nonces, SAE's scalars and the MAC address from it, the Wi-Fi refused without a seed | `kernel/rng.c`, `kernel/rng_core.c`, `kernel/platform/qcom_prng.c`, `kernel/net/virtio_rng.c` | `make -C tests/rng` (the PRNG against a register model), `make -C tests/rng qemu` (the Pro1X's paths with the PRNG in RAM); the PRNG itself needs the phone |
| 1 | the RPM: its GLINK edge in the message RAM (`qcom,rpm-msg-ram`, 0x45f0000, 28 KiB: a table of contents, the FIFOs `ap2r` and `r2ap`), the channel `rpm_requests` Linux left open (its id read from Linux's requests still in the FIFO), SMD-RPM requests; `kolibri.wifi` switches on (`swen`) the PM6125's L8, L16, L17 and L23, the rails of the WLAN part, at the voltages the RPM has from Linux | `kernel/modem/rpm.c` | `tests/modem` against an RPM in RAM (Linux's channel id found, acknowledgements, a refusal, the FIFOs wrapped); the phone next |
| 2 | WLFW (service 69 on the modem, node 0 port 77) over the existing QRTR: the indication register (ath10k's client id; the answer says whether the firmware is ready, which it is after a kexec: Linux's re-probe path), the capabilities (chip, board, SoC, firmware version and build) | `kernel/modem/wlfw.c` | `tests/modem` against a WLFW service in the fake modem; the phone next |
| 3 | the copy engines (12 engines at 0x240000 + 0x1000 * id of `membase`, their rings in RAM), the SMMU's context bank of stream 0x1a0 bypassed as the display's, then WLFW's `WLAN_CFG` (the pipes, the service map, the shadow registers: ath10k's `snoc.c` tables) and `WLAN_MODE` (mission) | `kernel/modem/ce.c`, `wlfw.c` (`wlfw_cfg_msg`, `wlfw_mode_msg`) | the rings and the messages: `make -C tests/wifi` (against a simulated engine block), `make -C tests/modem` (the QMI messages); not sent on the phone yet, nor the SMMU |
| 4 | the bring-up as `ath10k_core_start` orders it: WLFW's `WLAN_CFG` and `WLAN_MODE` sent, the engines set up, HTC (READY, HTT's service, then WMI's, SETUP_COMPLETE_EX), WMI-TLV (SERVICE_READY, the host memory it asks for, INIT, READY with the MAC address), HTT (VERSION_REQ, FRAG_DESC_BANK_CFG, RX_RING_CFG, AGGR_CFG), `ath10k_start`'s device parameters, the channel list | `kernel/modem/wifi.c` (the driver), `htc.c`, `wmi.c`, `htt.c`, `wlfw.c` (`wlfw_wlan_enable`), `kernel/net/wifi_dev.c`, `kernel/platform/pro1x.c` (`wifi_ready`) | `make -C tests/wifi` (`station`: against a simulated WCN3990), `make -C tests/modem` (WLAN_CFG and WLAN_MODE through the fake modem), `make -C tests/wifi qemu` (the kernel's side in QEMU up to the engines) |
| 5 | a station vdev, the scan (the BSS of the configured SSID: CCMP, PSK), vdev start, the AP's peer, open authentication and association (`MGMT_TX_SEND` by reference, or HTT), `PEER_ASSOC`, `VDEV_UP`; WPA2-PSK: the PMK (PBKDF2), the 4-way handshake over the data path, the keys installed (`VDEV_INSTALL_KEY`, `SEC_IND`), the peer authorized; the group key handshake; deauthentication and connection loss: scanning again | `wifi.c`, `wlan.c` (802.11 frames), `wpa.c` (the supplicant), `crypto.c` | `tests/wifi`: `crypto` (published vectors, OpenSSL), `station` (an AP with OpenSSL's crypto: the installed keys checked by CCMP both ways; a wrong PSK; no AP) |
| 6 | HTT's data path: Ethernet to native Wi-Fi frames in `TX_FRM` (gathered on CE4), `RX_IN_ORD_PADDR_IND`'s buffers from the RX ring to Ethernet, lwIP's interface (DHCP when the link is up) | `wifi.c`, `htt.c`, `wlan.c`, `kernel/net/wifi_dev.c`, `kernel/net/net.c` | `tests/wifi` `station`: lwIP above the driver takes a lease, answers a ping, an ARP broadcast with a new group key |
| 7a | the regulatory domain: the EEPROM's code from SERVICE_READY as ath's `regd.c` takes it (the Pro1X's 0x406c: a world domain, pair 0x6c), `country=` (AT by default) on top as cfg80211 does with a user's hint, the channel list from it (passive, radar, power, HT40+), `PDEV_SET_REGDOMAIN` | `kernel/modem/regd.c`, `regdb.c` (made by `tools/gen-regdb.py` from Linux's `regd_common.h` and wireless-regdb's `regulatory.db`), `wifi.c` | `tests/wifi` `regd` (28 checks: the phone's EEPROM with AT and without, US, a country EEPROM, 0x8000, 0x68, India, the 5 GHz range, every ath country), `station` (the channel list, the domain pair) |
| 7b | 802.11n and WMM: HT20/HT40 (short GI, STBC, LDPC as SERVICE_READY has them), the association's HT Capabilities and WMM information element, `PEER_ASSOC` with HT, the AP's WMM parameters (`VDEV_SET_WMM_PARAMS`), the TID of every frame (EAPOL 7, the DSCP's priority, lower under admission control) | `wifi.c`, `wlan.c`, `wmi.c` | `station` (an HT40 WMM AP; `legacy`; `uapsd`: HT20, admission control on VO) |
| 7c | power save: the firmware's (PS-Poll and null data, U-APSD with `uapsd=on`): `STA_POWERSAVE_MODE` and its parameters as `ath10k_mac_vif_setup_ps` and `ath10k_conf_tx_uapsd`; the host's: slower polling when idle, with `kolibri.wifi=irq` no SoC sleep while frames come | `wifi.c`, `kernel/net/wifi_dev.c`, `kernel/power.c` (`power_hold_awake`), `kernel/arch/arm64/psci.c` | `station` (the parameters and their order, the radio's doze with the AP keeping frames for the TIM, the polling's intervals), `tests/power host` (the held state) |
| 7d | WPA3-Personal and PMF: SAE (group 19; hunting and pecking or hash-to-element as the AP offers it; the anti-clogging token), the AKM choice (SAE, then PSK-SHA256, then PSK: the transition mode), PMF (MFPC, BIP-CMAC-128 with the IGTK, protected and BIP-checked disconnections, SA Query both ways, the association's comeback) | `kernel/modem/sae.c`, `ecc.c` (P-256), `crypto.c` (SHA-256, the KDF, HKDF, AES-CMAC), `wpa.c`, `wifi.c`, `wlan.c` | `crypto` (57 checks: FIPS 180-2, RFC 4231, 5869, 4493, IEEE 802.11-2020 J.10's SAE vectors both ways, OpenSSL), `station sae`, `h2e`, `mixed`, `psk256` (an AP with its own SAE by OpenSSL) |

`kolibri.wifi` (with `tools/pro1x-kexec.sh -m -w FILE`: the modem, so
QRTR, and the network) runs all of it on the phone: once WLFW says the
firmware is ready, the driver starts. Nothing of stages 3-7 has run on the
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
- **The station** (`mac.c`): `ath10k_add_interface` (vdev create, the
  power save parameters: stage 7c), `ath10k_hw_scan` with `ath10k_wmi_start_scan_init`'s values
  (the TLV firmware's inverted probe-request filter flag), the beacons
  and the AP's answers by MGMT_RX (dropped only for a CRC, decryption or
  key cache error, as `ath10k_wmi_event_mgmt_rx`: EXT_INFO, 0x40, is
  extension data), vdev start on the BSS's channel
  (`ath10k_vdev_start_restart`; the mode by the channel as
  `chan_to_phymode`: 11g, 11a, channel 14's 11b), the AP's peer and its
  `PEER_MAP` (`ath10k_wait_for_peer_created`), `ath10k_peer_assoc_prepare`
  (auth, `need_ptk_4_way` with an RSN element, the rates as
  `ath10k_mac_bitrate_to_rate`, 11a/b/g, listen interval 1; HT and QoS:
  stage 7b),
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
  handshake. CCMP for both ciphers; the other AKMs (PSK-SHA256, SAE) and
  PMF: stage 7d.
- **802.11** (`wlan.c`): beacons and probe responses read; the probe
  request's rates; open authentication; the association with the rates
  both have and our RSN element (HT and WMM: stage 7b); native
  Wi-Fi data frames (`ATH10K_HW_TXRX_NATIVE_WIFI`, the decapsulation INIT
  asks for: a 3-address header without QoS control, then LLC/SNAP) to and
  from Ethernet as `ath10k_htt_rx_h_undecap_nwifi` and
  `ath10k_tx_h_nwifi` do; the Protected bit once the keys are in (as
  mac80211 sets it for frames the hardware encrypts).

Not as Linux, on purpose: the RX ring holds 256 buffers (Linux 2047), the
engines 32 (CE1), 16 (CE2), 4 (the other receive engines) instead of
filling them; 64 TX ids instead of 1056; no A-MSDU, no aggregation set up
by the host (the firmware does A-MPDU itself: `ath10k_ampdu_action`
refuses mac80211's); no VHT (802.11ac: the WCN3990 has it; the
association is HT only); one network, no roaming.

**The tests on the PC** (`make -C tests/wifi`):

- `crypto`: SHA-1, HMAC-SHA1 (RFC 2202), PBKDF2 (RFC 6070), the PMK (IEEE
  802.11 J.4.2), the PRF (J.3.2), AES-128 (FIPS 197), the key wrap (RFC
  3394), and 200 random cases against OpenSSL; stage 7d's: SHA-256 (FIPS
  180-2), HMAC-SHA256 (RFC 4231), HKDF (RFC 5869 A.1), AES-CMAC (RFC 4493),
  the SHA-256 KDF and CMAC against OpenSSL, P-256 against OpenSSL's
  prime256v1, SAE against IEEE 802.11-2020 J.10 (hunting and pecking: the
  commit, KCK, PMK, PMKID; hash-to-element: PT and the PWE), the RSN
  choice (with `pmf=off`), BIP. 57 checks.
- `regd`: the regulatory domain (stage 7a). 28 checks.
- `station`: the whole driver against a simulated WCN3990 (the engines'
  registers as `test.c` has them; HTC, WMI-TLV and HTT as above; CCMP with
  the keys the driver installed: OpenSSL's AES-CCM) and an access point
  with three BSSs on channel 6 (the network with CCMP, the same SSID with
  a TKIP group cipher and more SNR, another SSID): the scan, the join, the
  4-way handshake checked at the AP with OpenSSL (its PTK, its MICs), the
  installed keys equal to the AP's (frames decrypt both ways), lwIP above
  the driver takes a DHCP lease (192.168.77.100), answers a ping, takes a
  new group key (the group key handshake) and answers an ARP broadcast
  encrypted with it, is deauthenticated and joins again; with stage 7 an
  HT40 WMM AP, the power save's parameters and the radio's doze. About 900
  checks; `station legacy` (no HT, no WMM, `powersave=off`), `station
  uapsd` (HT20, U-APSD, admission control on VO), `station sae`, `h2e`,
  `mixed`, `psk256` (stage 7d), `station wrongpsk` (management frames by
  HTT; message 2's MIC wrong at the AP, never up, tried again), `station
  noap` (scanning again and again).
- `qemu`: the kernel's side in QEMU (the Pro1X's tree from
  `tests/power/mkdtb.py --wifi`: a `qcom,wcn3990-wifi` node whose register
  window is RAM, the fake modem's WLFW, the network in the initrd): the
  network read, WLAN_CFG and WLAN_MODE over the fake QRTR, 4 MiB of
  uncached DMA memory below 4 GiB, the engines set up, then waiting for
  READY, which no firmware sends there.

### Stage 7: the refinements, on the PC

Four pieces on top of stages 4-6, each after the Linux code (7.1, the
Pro1X's tree) or the wpa_supplicant/hostapd code (2.11, BSD licence,
w1.fi) it follows; the standard texts named are IEEE 802.11-2020 and the
RFCs. The network's file (`tools/pro1x-kexec.sh -w FILE`) has, besides
`ssid=` and `psk=` (the passphrase serves WPA2 and WPA3 alike; 64 hex
digits are a PMK: WPA2 only) and `bssid=`:

| key | values | default |
| --- | --- | --- |
| `country=` | two capital letters (ISO 3166), `00` for none | `AT` |
| `powersave=` | `on`, `off` (as `iw set power_save off`) | `on` (cfg80211's `CONFIG_CFG80211_DEFAULT_PS=y`) |
| `uapsd=` | `on` (all four ACs), `off` | `off` (mac80211's `IEEE80211_DEFAULT_UAPSD_QUEUES` is 0) |
| `pmf=` | `on`, `off` (as `ieee80211w=0`), `fw` (also the firmware's peer PMF flag) | `on` (PMF whenever the AP is capable: `ieee80211w=1`) |

**7a, the regulatory domain** (`kernel/modem/regd.c`; ath's `regd.c`,
ath10k's `mac.c`, cfg80211's `reg.c`):

- The EEPROM's code is SERVICE_READY's (`hal_reg_capabilities.eeprom_rd`,
  16 bits: `ath10k_mac_init_rd`, no ACPI on ARM). The Pro1X's is 0x406c
  (Linux's log on the phone: `ath: EEPROM regdomain: 0x406c`, `Country
  alpha2 being used: 00`, `Regpair used: 0x6c`): the worldwide-roaming
  flag and WORC_WORLD, a world domain.
- `__ath_regd_init`: 0x8000 alone becomes 0x64, a code that is neither
  one of ath's countries nor of its domain pairs is refused (Linux: no
  wiphy; here: `regulatory domain X: ...`, the driver stops), 0 means the
  United States; the pair gives `PDEV_SET_REGDOMAIN`'s domain and its
  conformance test limits (0x6c: `NO_CTL` both), sent after the channel
  list as `ath10k_regd_update` does (`WMI_UNINIT_DFS_DOMAIN`: the kernel
  has no `CONFIG_ATH10K_DFS_CERTIFIED`).
- The channels (ath10k's 14 and 27, at most 30 dBm): a world domain's own
  rules (0x6c: 1-11 active, 12-13 passive, 5 GHz 5140-5360 and 5460-5860
  MHz passive at 30 dBm) as the base, the radar range 5260-5700 MHz
  always passive (`ath_reg_apply_radar_flags`; India 5500-5700); a
  country's EEPROM: ath's 0x64, then the country's own rules replace it
  (the driver's hint, `REGULATORY_STRICT_REG`). `country=` comes on top
  as a user's hint does: cfg80211 adds its flags and takes the smaller
  power; ath itself does not follow it (`CONFIG_ATH_REG_DYNAMIC_USER_REG_HINTS`
  is off), so the domain pair stays the EEPROM's. A rule counts for a
  channel when the channel's 20 MHz fit it (or two adjacent rules:
  `handle_channel_adjacent_rules`), AUTO-BW over contiguous rules, HT40+/-
  where the channel 20 MHz above/below is usable (`reg_process_ht_flags_channel`);
  SERVICE_READY's 5 GHz range limits it (`ath10k_mac_update_channel_list`).
- The countries' rules are wireless-regdb's `regulatory.db` (2026.09.03,
  the file cfg80211 reads; the 2-6 GHz rules), ath's tables are Linux's
  `regd_common.h`: both in `kernel/modem/regdb.c`, written by
  `tools/gen-regdb.py LINUX_TREE REGULATORY_DB OUT`.
- The phone with AT: 39 channels: 1-11 active at 20 dBm, 12-13 passive,
  14 off; 36-48 passive at 23 dBm, 52-64 radar at 20 dBm, 100-140 radar at
  26 dBm, 144 radar at 13 dBm (across AT's 5470-5725 and 5725-5875 MHz),
  149-169 passive at 13 dBm, 173 off. (Linux on the phone could not load
  its `regulatory.db`: `cfg80211: loaded regulatory.db is malformed or
  signature is missing/invalid`; it runs on 0x6c's rules alone, as
  `country=00` here.)
- Not done: an AP's Country element as a hint (Linux follows it for a
  world domain), beacon hints (a beacon seen makes a passive channel
  active), regdb's WMM rules (ETSI's limits on the EDCA parameters).

**The MAC address**: the Pro1X's firmware reports none in READY (Linux:
`invalid MAC address; choosing random`, its `wlan0` 16:45:a7:...): a
random locally administered one, as `ath10k_mac_register`'s
`eth_random_addr`, new at every start.

**7b, 802.11n and WMM** (`wifi.c`, `wlan.c`, `wmi.c`; mac80211's
`mlme.c`, `ht.c`, `util.c`, `wme.c`, cfg80211's `util.c`, ath10k's
`mac.c`, `wmi-tlv.c`, `htt_tx.c`, `htt_rx.c`):

- Our HT capabilities as `ath10k_get_ht_cap` makes them from
  SERVICE_READY's `ht_cap_info` (short GI 20/40, STBC, LDPC, dynamic
  SMPS), the RF chains (at most the WCN3990's `max_spatial_stream` 4: the
  MCS sets), A-MPDU 64K, density 8 us; in the association request as
  `ieee80211_add_ht_ie` changes them: no 40 MHz when the regulatory
  domain has no HT40 in the AP's direction, SM power save off.
- HT only with WMM in the BSS, both HT elements, and the HT Operation's
  primary channel the BSS's (`ieee80211_determine_ap_chan`); 40 MHz by its
  secondary channel's offset where the channel allows it, else 20 MHz;
  `VDEV_START`'s mode (`chan_to_phymode`: 11ng/11na HT20/HT40) and its
  center 10 MHz off.
- `PEER_ASSOC` with the AP's HT capabilities (its association answer's,
  ours masked: `ieee80211_ht_cap_ie_to_sta_ht_cap`) as
  `ath10k_peer_assoc_h_ht` fills them (HT, 40 MHz, LDPC, STBC, the rate
  control's flags, the largest A-MPDU, the density, the MCS list, the
  streams), QoS when the answer has a WMM parameter element (`sta->wme`),
  the mode (`_h_phymode`); then the peer's SM power save, the peer
  statistics, `VDEV_UP`, the dummy peer parameter (`ath10k_bss_assoc`).
- WMM: the information element in the request (QoS info 0, or the U-APSD
  queues); the AP's parameters from its association answer
  (`_ieee80211_sta_wmm_params`: AIFSN at least 2, CW from ECW, TXOP;
  without them `ieee80211_set_wmm_default` with QoS), each AC through
  `ath10k_conf_tx`: `VDEV_SET_WMM_PARAMS` (all four ACs in BE, BK, VI, VO
  order, a dummy word before each, TXOP in us) and the AC's power save
  parameters (`ath10k_conf_tx_uapsd`); at the vdev's creation the defaults
  without QoS (`ieee80211_do_open`).
- The frames: native Wi-Fi without QoS control (`ath10k_tx_h_nwifi`
  takes it out), the TID in HTT's descriptor: with QoS the 802.1d
  priority (`cfg80211_classify8021d`: the DSCP's top three bits and RFC
  8325's exceptions; EAPOL 7, the control port), lower while the AP wants
  admission for it (`ieee80211_downgrade_queue`); without QoS the non-QoS
  TID 16. Received QoS headers padded to 4 (`ath10k_htt_rx_nwifi_hdrlen`).
- Not done: VHT (802.11ac), A-MSDU, the 20/40 BSS coexistence.

**7c, power save** (`wifi.c`, `kernel/net/wifi_dev.c`, `kernel/power.c`,
`kernel/arch/arm64/psci.c`; ath10k's `ath10k_mac_vif_setup_ps`,
`_recalc_ps_wake_threshold`, `_recalc_ps_poll_count`,
`ath10k_conf_tx_uapsd`, `ath10k_add_interface`,
`ath10k_mac_op_assign_vif_chanctx`; mac80211's `ieee80211_recalc_ps_vif`,
`ieee80211_powersave_allowed`):

- The firmware's: at the vdev's creation the RX wake policy WAKE, the TX
  wake threshold ALWAYS, the PS-Poll count NO_MAX; at its start the PS
  mode off; once the port is authorized the inactivity time (the beacon
  interval in ms: mac80211's `dynamic_ps_timeout` stays 0 for
  `SUPPORTS_DYNAMIC_PS` hardware, ath10k takes the beacon interval: 102
  ms for 100 TU) and the PS mode on; at a disconnection off while the vdev
  is still started. The radio then sleeps between beacons; PS-Poll and
  null data frames, the TIM and the DTIM are the firmware's.
- U-APSD with `uapsd=on`: QoS info 0x0f in the WMM element, all ACs'
  delivery and trigger bits (0xff), the RX wake policy POLL_UAPSD, the TX
  wake threshold NEVER, the PS-Poll count 3, the firmware's auto trigger
  off (`STA_UAPSD_AUTO_TRIG`, when SERVICE_READY has service 27 or 28).
- The host's: polled (the default) every 10 ms on the way up, then every
  20 ms while frames came in the last second, 200 ms up to 10 s without,
  1 s after (`wifi_poll_ms`; what comes meanwhile waits in the AP and the
  firmware: a packet after a quiet time waits up to a second). With
  `kolibri.wifi=irq`: no polling, a look once a second when idle; and
  while frames came in the last second no SoC sleep (`power_hold_awake`:
  `psci_idle` takes the cluster's GDHS, 40000022, instead of its collapse
  41000044 with the MPM handed over), because in the SoC's sleep the GIC
  is off and the copy engines' SPIs 358-369 are no MPM pins (the SM6115's
  `qcom,mpm-pin-map` has none for them); a level interrupt that comes
  then waits at the engine for the next wake-up by time.

**7d, WPA3-Personal and PMF** (`sae.c`, `ecc.c`, `crypto.c`, `wpa.c`,
`wifi.c`, `wlan.c`; hostapd/wpa_supplicant 2.11's `src/common/sae.c`,
`dragonfly.c`, `wpa_common.c`, `src/rsn_supp/wpa.c`, `wpa_ie.c`,
`wpa_supplicant/sme.c`, `wpa_supplicant.c`; IEEE 802.11-2020 12.4 (SAE),
12.5.4 (BIP), 12.6.11 (SA Query), Annex J.10 (SAE's test vectors); RFC
7664 (Dragonfly); mac80211 and ath10k for the PMF frames):

- The profile: `key_mgmt=SAE WPA-PSK-SHA256 WPA-PSK`, `ieee80211w=1`,
  `sae_pwe=2`. The choice as `wpa_supplicant_set_suites`: SAE before
  PSK-SHA256 before PSK (an AP of WPA2 and WPA3, the transition mode,
  gets SAE), PMF when the AP is capable (MFPC) with BIP-CMAC-128 (an AP
  that requires PMF with another group management cipher is skipped),
  hash-to-element when the AP's RSNX element has it, else hunting and
  pecking; an H2E-only BSS (the membership selector 123) without it in
  its RSNX element is skipped.
- SAE, group 19 only (P-256, the group WPA3 requires; the AP's status 77
  ends the attempt): hunting and pecking with at least 40 rounds, a stub
  password after the find and the blinded residue test
  (`dragonfly_is_quadratic_residue_blind`); hash-to-element with PT once
  per SSID and password, the simplified SWU map (z = -10), PWE = val PT;
  the commit (status 126 for H2E), the AP's anti-clogging token (status
  76) sent back as it is or in its container (93), the confirm, KCK, PMK,
  PMKID (not put into the association: `sae_pmkid_in_assoc` 0).
- The association: the RSN element with the AKM (8 or 6), MFPC, PMKID
  count 0 and BIP-CMAC-128; the RSNX element (H2E) with SAE; message 2
  carries both, message 3's RSNX element must be the beacon's. EAPOL-Key
  version 2 (PSK), 3 (PSK-SHA256), 0 (SAE: "AKM-defined"): the SHA-256 KDF
  for the PTK, AES-128-CMAC MICs; the IGTK from its KDE (key 4 or 5, the
  IPN).
- PMF: ath10k does BIP in software (`ath10k_set_key` returns 1 for
  AES-CMAC), so the IGTK stays in the driver; the firmware gets the peer
  PMF flag only with `ATH10K_FW_FEATURE_MFP_SUPPORT`, which the Pro1X's
  `firmware-5.bin` does not have (Linux's log: `features
  wowlan,mgmt-tx-by-reference,non-bmi`): no flag, as Linux (`pmf=fw` sets
  it, for a test). Robust management frames to the AP (SA Query) carry
  mac80211's CCMP header with our own PN (`IEEE80211_KEY_FLAG_GENERATE_IV_MGMT`)
  and room for the MIC (ath10k's WMI path adds it), the firmware
  encrypts them; from the AP the firmware hands them over decrypted, with
  the Protected bit, the CCMP header and the MIC (ath10k's `wmi.c`), the
  PN checked against the last. A unicast deauthentication or
  disassociation counts only protected; an unprotected one is dropped and
  an SA Query goes to the AP (`sme_event_unprot_disconnect`: every 201 ms,
  1000 ms at most; answered: still associated; no answer: disconnected).
  A group one counts only with a valid MMIE (BIP-CMAC-128, the IPN above
  the last). The AP's SA Queries are answered
  (`ieee80211_process_sa_query_req`); an association refused for now
  (status 30) is tried again after its Timeout Interval's comeback time.
- Not done: SAE groups 20 and 21, password identifiers (only for the test
  vectors), SAE-PK, the rejected groups element, FT, OCV, beacon
  protection.

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
   country=AT
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
   wifi: SERVICE_READY: firmware build ..., ... regulatory domain 406c; RX full reorder yes, management frames by WMI
   wifi: host memory N: N KiB at ...
   wifi: READY: the MAC address 00:00:00:00:00:00, ABI 53
   wifi: invalid MAC address; choosing random xx:xx:...
   wifi: HTT 3.96
   wifi: regulatory domain: EEPROM 406c (world, ath's "00"), domain 6c, CTL ff/ff (2/5 GHz); country AT: 39 channels, 28 passive, 16 radar
   wifi: scanning 39 channels for "<SSID>"
   wifi: joining xx:xx:... on NNNN MHz (channel N, SNR N, ...)
   wifi: WPA2-PSK                                  (or WPA3-SAE (...), management frame protection)
   wifi: HT20, WMM                                 (or HT40, no HT; U-APSD offered, not asked for)
   wifi: the AP's peer, id N
   wifi: authenticated (open system)               (WPA3: SAE commit out, SAE confirm out, authenticated (SAE))
   wifi: associated, AID N
   wifi: peer HT20 short GI, 11n, N streams, N HT rates; QoS on
   wifi: 4-way handshake: message 2 out
   wifi: 4-way handshake: message 4 out
   wifi: the pairwise key installed
   wifi: the group key N installed
   wifi: up: "<SSID>" (xx:xx:...), WPA2-PSK CCMP   (or WPA3-SAE CCMP, PMF)
   wifi: power save on (inactivity 102 ms, PS-Poll/null data)
   net: wcn3990, MAC xx:xx:..., DHCP
   net: 192.168.x.y, mask ..., gateway ..., DNS ...
   wifi: up; N WMI commands, ... (at the lifetime's end)
   ```

   READY's MAC address is expected to be zero (Linux chose a random one
   on the phone); the random one changes at every start (an AP with a MAC
   filter refuses it; DHCP hands out a new lease).
3. With the network: the same with `kolibri.nettest=80` added (the
   kernel's self test: a DNS lookup of example.com, an HTTP request to the
   gateway's port 80; a router without a web server answers with an
   error, which is fine), then the messengers (docs/messengers.md) on it.
4. The interrupts: step 2 with `kolibri.wifi=irq` instead of the default
   polling (the engines' SPIs 358-369 level-triggered; a storm would end
   in the watchdog's reset at about 32 s after `copy engines up`: then
   polling stays).
5. WPA3, if the AP offers it (WPA3-only or WPA2/WPA3): step 2 as it is;
   expected `wifi: WPA3-SAE (hash-to-element)` or `(hunting and pecking)`,
   `management frame protection`, `SAE commit out (group 19, ...)`,
   possibly `the AP asks for its anti-clogging token: commit again`, `SAE
   confirm out`, `authenticated (SAE)`, `the IGTK N kept (BIP in software,
   as ath10k)`, `up: ... WPA3-SAE CCMP, PMF`. Then, with the AP's
   administration: a reboot of the AP while the phone is up should give
   `an unprotected disconnection: an SA Query to the AP`, then `SA Query:
   no answer (the AP lost us): disconnected` and a new join. Failing:
   `the AP's confirm is wrong (the password?)`, `SAE: the AP's commit
   refused`; with PMF the risks below; `pmf=fw`, then `pmf=off` (WPA2
   only for an AP that requires PMF) in the file to tell them apart.
6. Power: step 2 with `kolibri.powertest=10` (the cable out) and nothing
   running: the power lines (docs/power.md) with Wi-Fi up, the wake-ups
   about 1/s more than without (`by deadline: network`: the 1 s polling
   once idle; lwIP's timers add nothing once the lease is there, 2/s more
   with `kolibri.nettimers=periodic` for comparison); then with
   `kolibri.wifi=irq`: `SoC sleep held N` only right
   after traffic, `vlow`/`vmin` counts moving while idle. A ping from the
   PC (`ping` to the leased address) still answered, with up to 1 s
   delay after a quiet time. `powersave=off` for comparison (the battery
   current in the power lines).
7. Other countries and bands: `country=00` (0x6c's rules alone, 5 GHz all
   passive), a 5 GHz AP (36-48: `HT40` if the AP uses 40 MHz).

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
  list, a hidden SSID: the scan probes for it, but not on passive
  channels: with AT 12-13 and all of 5 GHz are passive), `skipped: ...`
  (TKIP, 802.1X, PMF with a cipher other than BIP-CMAC-128, PMF required but `pmf=off`), `EAPOL
  dropped: message 3's MIC is wrong (the PSK?)`, `association refused
  (status N)`.
- **The regulatory domain**: `PDEV_SET_REGDOMAIN` has no answer; a
  firmware that does not like 0x6c's pair or the channel list's powers
  would show it as no beacons in the scan (`not found (0 networks seen)`):
  then `country=00`, which leaves the list as 0x6c's alone.
- **HT**: `association refused (status N)` right after `HT40` or `HT20`:
  the AP refuses our HT capabilities; `peer legacy` although the AP has HT:
  its answer lacked the elements.
- **PMF on this firmware**: without the peer PMF flag (as Linux here) the
  firmware may not encrypt our protected SA Query frames or decrypt the
  AP's: then the AP's SA Queries go unanswered (an AP disconnects us after
  its own SA Query timeout: `deauthenticated by the AP` with reason 7 or
  disassociation after a minute or so) or our SA Queries are never
  answered (`SA Query: no answer` after any spoofed or genuine
  unprotected deauthentication). Try `pmf=fw` (the flag), then `pmf=off`.
- **SAE's time**: hunting and pecking takes some ms in the tick's context
  (40 rounds of the residue test, three scalar multiplications); an AP
  with a short SAE timeout (hostapd: 1 s per message) should not mind.
- **Power save**: a firmware that loses frames in PS (pings unanswered
  after a quiet time, `restarts` growing): `powersave=off`.

## The stack: lwIP in the kernel

- **lwIP 2.2.1**, fetched by `tools/fetch-lwip.sh` (a pinned release,
  checked by its SHA-256) into `third_party/lwip`, not kept in the
  repository. `NO_SYS=1`: the raw API only, no threads. lwIP runs in the
  kernel's context only (interrupts are masked in the kernel, so there is
  no locking): from the network interrupt (frames in), from the timer tick
  (its timeouts; `sys_now` reads the generic timer), and from system calls
  (the sockets of the Linux personality, [messengers.md](messengers.md)).
- **Its timers on demand** (`kernel/net/timers.c`, `LWIP_TIMERS_CUSTOM`;
  [power.md](power.md), stage 6): no cyclic timer runs on an idle
  network. TCP's 250 ms timer only while a connection has something in
  flight or pending (an idle connection wakes the CPU only at its
  keepalive, FIN_WAIT_2 or TIME_WAIT time-out), DHCP's at a request's or a
  lease's time-out, ARP's, IP reassembly's and DNS's second only after an
  ARP request, a fragment, a DNS query; what an idle timer did not count
  (`tcp_ticks`, the tables' ages) is brought to the present at every way
  into the kernel before lwIP runs. `tests/net` compares it with lwIP's own
  timers on eleven scenarios; `kolibri.nettimers=periodic` runs them as
  lwIP does.
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
timeouts from the tick), `kernel/net/timers.c` (lwIP's timers on
demand), `kernel/net/lwipopts.h`.

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
