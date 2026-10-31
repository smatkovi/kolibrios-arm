/* KolibriOS for ARM: the Pro1X's Wi-Fi driver (kernel/modem/wifi.c with
 * ce.c, htc.c, wmi.c, htt.c, wlan.c, wpa.c, crypto.c) on the PC, from
 * WLAN_MODE to a DHCP lease, against a simulated WCN3990 and a simulated
 * access point (make -C tests/wifi; docs/pro1x-network.md, stages 4-6).
 *
 * The WCN3990: the copy engines' registers in RAM as the hardware has
 * them (test.c's model), its HTC, WMI-TLV and HTT, the host's memory
 * reached by address (above 16 GiB; INIT's chunks below 4 GiB); its
 * scan (beacons as MGMT_RX on the channels asked), vdev, peers
 * (PEER_MAP), management frames by reference (MGMT_TX_SEND, or HTT
 * TX_FRM), the keys installed (SEC_IND), and CCMP with those keys
 * (OpenSSL's AES-CCM): frames to the air encrypted with the key the
 * driver installed, frames from the air decrypted with it, the RX
 * descriptors and RX_IN_ORD_PADDR_IND on the host's RX ring.
 *
 * The access point "KolibriNet" on channel 6 (and two others: an
 * unrelated SSID, the same SSID with TKIP): beacons, open system
 * authentication, association, the authenticator's side of the 4-way and
 * group key handshakes with OpenSSL (PBKDF2, HMAC-SHA1, the key wrap,
 * independent of kernel/modem/crypto.c), CCMP with its own keys, and a
 * router at 192.168.77.1: DHCP, ARP, ICMP echo. lwIP (the kernel's
 * options) runs above the driver and takes the lease.
 *
 * The firmware's offloads: ARP requests for the address the host gave
 * (SET_ARP_NS_OFFLOAD) answered by the firmware itself, a null data frame
 * after 30 s of silence (STA_KEEPALIVE); both services in SERVICE_READY
 * (not in "legacy", which then sees neither command).
 *
 * Scenarios: the normal one (WMI management frames, a lease, a ping both
 * ways, a GTK rekeying, a deauthentication and the reconnection),
 * "wrongpsk" (management frames by HTT; the AP drops message 2: never up),
 * "noap" (the network absent: scans repeat). */
#include <stdio.h>
#include <stdlib.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/ec.h>
#include <openssl/bn.h>
#include <openssl/obj_mac.h>
#include <openssl/core_names.h>
#include "../../kernel/modem/ce.h"
#include "../../kernel/modem/htc.h"
#include "../../kernel/modem/wmi.h"
#include "../../kernel/modem/htt.h"
#include "../../kernel/modem/wifi.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "netif/ethernet.h"

static int failed, passed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u32 now;                         /* the simulated clock, ms */
static const char *scenario = "normal";
#define IS(s) (!strcmp(scenario, s))

/* --- the host's memory --------------------------------------------------- */

#define ARENA (8u << 20)
#define ARENA_PA 0x400000000ull         /* 16 GiB: the high word 4 */
#define LOW (1u << 20)
#define LOW_PA 0x80000000ull
static u8 *arena, *low;
static u32 arena_used, low_used, bad_access;

static void *dma_alloc(u32 size, u64 *pa)
{
	arena_used = (arena_used + 63) & ~63u;
	if (arena_used + size > ARENA) return NULL;
	*pa = ARENA_PA + arena_used;
	void *p = arena + arena_used;
	memset(p, 0, size);
	arena_used += size;
	return p;
}

static void *dma_alloc32(u32 size, u64 *pa)
{
	low_used = (low_used + 4095) & ~4095u;
	if (low_used + size > LOW) return NULL;
	*pa = LOW_PA + low_used;
	void *p = low + low_used;
	memset(p, 0, size);
	low_used += size;
	return p;
}

static u8 *va(u64 pa, u32 len)
{
	if (pa >= ARENA_PA && pa + len <= ARENA_PA + ARENA) return arena + (pa - ARENA_PA);
	if (pa >= LOW_PA && pa + len <= LOW_PA + LOW) return low + (pa - LOW_PA);
	bad_access++;
	return arena;
}

/* --- the copy engines' registers (as test.c) ------------------------------ */

static u32 regs[0x250000 / 4];
static u32 shadow[CE_COUNT];

static u32 rd(u32 off) { return off < sizeof(regs) ? regs[off / 4] : (bad_access++, 0); }

static void wr(u32 off, u32 v)
{
	if (off >= 0x32000 && off < 0x32000 + 4 * CE_COUNT) {
		shadow[(off - 0x32000) / 4] = v;
		return;
	}
	if (off >= sizeof(regs)) { bad_access++; return; }
	if (off >= 0x240000 && off < CE_WRAPPER && (off & 0xfff) == CE_HOST_IS) {
		regs[off / 4] &= ~v;
		return;
	}
	regs[off / 4] = v;
	/* the read indices' word in RAM: the target writes its indices there
	 * once it knows where (a model: the hardware's timing is not known) */
	if (off == CE_RRI_HIGH)
		for (u32 id = 0; id < CE_COUNT; id++) {
			u64 pa = regs[CE_RRI_LOW / 4] | (u64)(v & 0x1f) << 32;
			*(u32 *)va(pa + 4 * id, 4) = regs[(CE_BASE(id) + CE_CURRENT_SRRI) / 4] |
						      regs[(CE_BASE(id) + CE_CURRENT_DRRI) / 4] << 16;
		}
}

static u32 creg(u32 id, u32 r) { return regs[(CE_BASE(id) + r) / 4]; }
static void cset(u32 id, u32 r, u32 v) { regs[(CE_BASE(id) + r) / 4] = v; }

static u64 ring_pa(u32 id, bool dst)
{
	return dst ? creg(id, CE_DR_BA_LO) | (u64)(creg(id, CE_DR_BA_HI) & 0x1f) << 32
		   : creg(id, CE_SR_BA_LO) | (u64)(creg(id, CE_SR_BA_HI) & 0x1f) << 32;
}

static u32 *rri_word(u32 id)
{
	u64 pa = regs[CE_RRI_LOW / 4] | (u64)(regs[CE_RRI_HIGH / 4] & 0x1f) << 32;
	return (u32 *)va(pa + 4 * id, 4);
}

/* the target's turn on a source ring: whole transfers (gathered pieces
 * joined) to fn */
static void target_take(u32 id, void (*fn)(u32 ce, u32 tid, const u8 *d, u32 len))
{
	u32 n = creg(id, CE_SR_SIZE), r = creg(id, CE_CURRENT_SRRI);
	if (!n) return;
	u8 *ring = va(ring_pa(id, false), n * 16);
	static u8 msg[8192];
	u32 mlen = 0, tid = 0;
	bool first = true;
	while (r != shadow[id]) {
		u8 *d = ring + 16 * r;
		u32 hi = get32(d + 4);
		u64 pa = get32(d) | (u64)(hi & 0x1f) << 32;
		u32 len = get16(d + 8), flags = get16(d + 10);
		bool gather = flags & 1;
		CHECK(gather == !!(hi & CE_GATHER), "the gather flag in both places (%x %x)", flags, hi);
		if (first) tid = flags >> 4;
		first = false;
		if (mlen + len <= sizeof(msg)) memcpy(msg + mlen, va(pa, len), len);
		mlen += len;
		r = (r + 1) & (n - 1);
		cset(id, CE_CURRENT_SRRI, r);
		u32 *w = rri_word(id);
		*w = (*w & 0xffff0000u) | r;
		if (!gather) {
			fn(id, tid, msg, mlen);
			mlen = 0;
			first = true;
		}
	}
}

/* the target to the host on a destination ring; queued while the host
 * has no buffer posted */
#define PENDING 256
static struct { u32 ce, len; u8 d[2048]; } pend[PENDING];
static u32 npend;

static bool give_now(u32 id, const u8 *msg, u32 len)
{
	u32 n = creg(id, CE_DR_SIZE), drri = creg(id, CE_CURRENT_DRRI);
	if (!n || drri == creg(id, CE_DST_WR_INDEX)) return false;
	u8 *d = va(ring_pa(id, true), n * 16) + 16 * drri;
	u64 pa = get32(d) | (u64)(get32(d + 4) & 0x1f) << 32;
	memcpy(va(pa, len), msg, len);
	put16(d + 8, (u16)len);
	drri = (drri + 1) & (n - 1);
	cset(id, CE_CURRENT_DRRI, drri);
	u32 *w = rri_word(id);
	*w = (*w & 0xffffu) | drri << 16;
	cset(id, CE_HOST_IS, creg(id, CE_HOST_IS) | CE_IE_COPY_COMPLETE);
	return true;
}

static void give(u32 id, const u8 *msg, u32 len)
{
	if (npend == 0 && give_now(id, msg, len)) return;
	CHECK(npend < PENDING && len <= 2048, "the simulation's queue (%u)", npend);
	if (npend >= PENDING || len > 2048) return;
	pend[npend].ce = id;
	pend[npend].len = len;
	memcpy(pend[npend].d, msg, len);
	npend++;
}

static void give_pending(void)
{
	u32 i = 0;
	while (i < npend && give_now(pend[i].ce, pend[i].d, pend[i].len)) i++;
	memmove(pend, pend + i, (npend - i) * sizeof(pend[0]));
	npend -= i;
}

/* --- CCMP (IEEE 802.11-2016 12.5.3) with OpenSSL ---------------------------- */

static int ccmp(bool enc, const u8 tk[16], const u8 *hdr, u64 pn, const u8 *in, u32 len, u8 *out)
{
	u8 nonce[13], aad[22];
	u16 fc = get16(hdr);
	bool mgmt = (fc & 0x000c) == 0;
	nonce[0] = mgmt ? 0x10 : 0;             /* priority 0 (no QoS); management frames: their bit (802.11w) */
	memcpy(nonce + 1, hdr + 10, 6);         /* A2 */
	for (int i = 0; i < 6; i++) nonce[7 + i] = (u8)(pn >> (40 - 8 * i));
	/* the AAD: FC masked (subtype bits 4-6, retry, power management,
	 * more data off; Protected on), A1-A3, the sequence control's
	 * fragment number */
	put16(aad, (u16)((fc & ~(mgmt ? 0x3800u : 0x3870u)) | 0x4000u));
	memcpy(aad + 2, hdr + 4, 18);
	put16(aad + 20, get16(hdr + 22) & 0x000f);
	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	int ol, ok = 1;
	if (enc) {
		EVP_EncryptInit_ex(c, EVP_aes_128_ccm(), NULL, NULL, NULL);
		EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_CCM_SET_IVLEN, 13, NULL);
		EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_CCM_SET_TAG, 8, NULL);
		EVP_EncryptInit_ex(c, NULL, NULL, tk, nonce);
		EVP_EncryptUpdate(c, NULL, &ol, NULL, (int)len);
		EVP_EncryptUpdate(c, NULL, &ol, aad, sizeof(aad));
		EVP_EncryptUpdate(c, out, &ol, in, (int)len);
		EVP_EncryptFinal_ex(c, out + len, &ol);
		EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_CCM_GET_TAG, 8, out + len);
	} else {
		if (len < 8) ok = 0;
		else {
			len -= 8;
			EVP_DecryptInit_ex(c, EVP_aes_128_ccm(), NULL, NULL, NULL);
			EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_CCM_SET_IVLEN, 13, NULL);
			EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_CCM_SET_TAG, 8, (void *)(in + len));
			EVP_DecryptInit_ex(c, NULL, NULL, tk, nonce);
			EVP_DecryptUpdate(c, NULL, &ol, NULL, (int)len);
			EVP_DecryptUpdate(c, NULL, &ol, aad, sizeof(aad));
			ok = EVP_DecryptUpdate(c, out, &ol, in, (int)len) > 0;
		}
	}
	EVP_CIPHER_CTX_free(c);
	return ok ? (int)len : -1;
}

/* an MPDU encrypted: the header, the CCMP header (PN, key id, ExtIV),
 * the data, the MIC */
static u32 ccmp_encrypt(const u8 tk[16], int keyid, u64 pn, const u8 *f, u32 len, u8 *o)
{
	memcpy(o, f, 24);
	put16(o, get16(o) | 0x4000);
	u8 *h = o + 24;
	h[0] = (u8)pn, h[1] = (u8)(pn >> 8), h[2] = 0, h[3] = (u8)(0x20 | keyid << 6);
	h[4] = (u8)(pn >> 16), h[5] = (u8)(pn >> 24), h[6] = (u8)(pn >> 32), h[7] = (u8)(pn >> 40);
	ccmp(true, tk, o, pn, f + 24, len - 24, o + 32);
	return len + 16;
}

static int ccmp_decrypt(const u8 tk[16], const u8 *f, u32 len, u8 *o, int *keyid)
{
	if (len < 24 + 16) return -1;
	const u8 *h = f + 24;
	u64 pn = h[0] | (u64)h[1] << 8 | (u64)h[4] << 16 | (u64)h[5] << 24 | (u64)h[6] << 32 | (u64)h[7] << 40;
	*keyid = h[3] >> 6;
	memcpy(o, f, 24);
	put16(o, get16(o) & ~0x4000);
	int n = ccmp(false, tk, f, pn, f + 32, len - 32, o + 24);
	return n < 0 ? -1 : n + 24;
}

/* --- the access point and its router --------------------------------------- */

static const u8 ap_mac[6] = { 0x02, 0xa9, 0x00, 0x00, 0x00, 0x06 };
static const u8 other_mac[6] = { 0x02, 0xa9, 0x00, 0x00, 0x00, 0x0b };
static const u8 tkip_mac[6] = { 0x02, 0xa9, 0x00, 0x00, 0x00, 0x0c };
static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const char *ssid = "KolibriNet", *passphrase = "correct horse battery";
static const u8 rsn_ccmp[] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0x0c, 0 };
static const u8 rsn_tkip[] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 2, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };

/* the AP "KolibriNet"'s 802.11n and WMM: HT40 above channel 6 (or HT20,
 * or none), WMM with hostapd's default parameters, U-APSD, admission
 * control on VO */
static struct { bool ht, ht40, wmm, uapsd, acm_vo; } apc;

/* the AP's security: its AKMs (1 << 2 PSK, 6 PSK-SHA256, 8 SAE), MFP
 * capable/required, H2E in an RSNX element, H2E only (the BSS membership
 * selector 123), the anti-clogging token asked for, PMF's behaviours */
static struct { u32 akms; bool mfpc, mfpr, h2e, h2e_only, anticlog; } aps;
static u8 ap_rsn[48], ap_rsnx[3];
static u32 ap_rsn_len, ap_rsnx_len;

static void ap_build_rsn(void)
{
	u32 n = 0;
	u8 *o = ap_rsn;
	o[n++] = 48, o[n++] = 0, o[n++] = 1, o[n++] = 0;
	o[n++] = 0, o[n++] = 0x0f, o[n++] = 0xac, o[n++] = 4;
	o[n++] = 1, o[n++] = 0, o[n++] = 0, o[n++] = 0x0f, o[n++] = 0xac, o[n++] = 4;
	u32 cnt = 0, at = n;
	n += 2;
	for (u32 a = 0; a < 32; a++)
		if (aps.akms & (1u << a)) o[n++] = 0, o[n++] = 0x0f, o[n++] = 0xac, o[n++] = (u8)a, cnt++;
	put16(o + at, (u16)cnt);
	put16(o + n, (u16)(0x000c | (aps.mfpc ? 0x80 : 0) | (aps.mfpr ? 0x40 : 0)));
	n += 2;
	if (aps.mfpc) {
		o[n++] = 0, o[n++] = 0;                 /* no PMKIDs */
		o[n++] = 0, o[n++] = 0x0f, o[n++] = 0xac, o[n++] = 6;   /* BIP-CMAC-128 */
	}
	o[1] = (u8)(n - 2);
	ap_rsn_len = n;
	ap_rsnx_len = 0;
	if (aps.h2e) ap_rsnx[0] = 244, ap_rsnx[1] = 1, ap_rsnx[2] = 0x20, ap_rsnx_len = 3;
}
#define AP_HT_CAP 0x116f                /* LDPC, 20/40, SM PS off, short GI 20/40, RX STBC 1, DSSS/CCK in 40 */

static u32 ap_ies(u8 *f)
{
	u32 n = 0;
	if (apc.ht) {
		u8 h[28] = { 45, 26 };
		put16(h + 2, AP_HT_CAP);
		h[4] = 0x17;                    /* A-MPDU 64K, density 4 us */
		h[5] = 0xff, h[6] = 0xff;       /* MCS 0-15 */
		h[5 + 12] = 0x01;               /* TX MCS set defined */
		memcpy(f + n, h, 28);
		n += 28;
		u8 o[24] = { 61, 22, 6, (u8)(apc.ht40 ? 0x05 : 0x00) };
		memcpy(f + n, o, 24);
		n += 24;
	}
	if (apc.wmm) {
		/* hostapd's defaults: BE AIFSN 3 CW 4/10, BK 7 4/10, VI 2 3/4 TXOP
		 * 94, VO 2 2/3 TXOP 47 */
		u8 w[26] = { 221, 24, 0x00, 0x50, 0xf2, 2, 1, 1, (u8)((apc.uapsd ? 0x80 : 0) | 1), 0,
			     0x03, 0xa4, 0, 0, 0x27, 0xa4, 0, 0, 0x42, 0x43, 94, 0, (u8)(0x62 | (apc.acm_vo ? 0x10 : 0)), 0x32, 47, 0 };
		memcpy(f + n, w, 26);
		n += 26;
	}
	return n;
}

static struct {
	u8 sta[6];
	bool authed, assoced, keys;
	u8 pmk[32], anonce[32], snonce[32], ptk[48], gtk[16], gtk2[16];
	int gtk_id;
	u64 replay, pn, gpn;
	u8 sta_ie[64];
	u32 sta_ie_len;
	int msg;                            /* the next 4-way message the AP waits for */
	u32 m1_tries;
	u8 assoc_ht[28], assoc_wmm[9];
	bool assoc_had_ht, assoc_had_wmm;
	u32 assoc_rates;
	/* the association's security: the AKM, PMF, BIP's key, the PNs of
	 * protected management frames, SA Query */
	u8 akm, assoc_rsnx[8];
	bool mfp, assoc_had_rsnx;
	u8 igtk[16];
	u64 ipn, mgmt_pn, sta_mgmt_pn;
	bool saq_silent;                    /* (the AP does not answer the station's SA Queries) */
	u32 saq_requests, saq_responses, mgmt_replays, comebacks, assocs;
	u8 saq_id[2];
	bool comeback_armed;
	u32 arp_requests, bad_mic, m2, m4, group2, dhcp_discover, dhcp_request, arp_replies, echo_replies, decrypt_fail, frames;
	u32 nulls;                          /* null data frames from the station (the firmware's keepalive) */
	bool rekey_sent, rekey_done, arp_after_rekey, deauth_sent;
	u32 eapol_at;
} ap;

static void ap_prf(const u8 *key, const char *label, const u8 *data, u32 dl, u8 *out, u32 n)
{
	u8 buf[128], h[20];
	u32 ll = strlen(label), pos = 0;
	memcpy(buf, label, ll);
	buf[ll] = 0;
	memcpy(buf + ll + 1, data, dl);
	for (u8 i = 0; pos < n; i++) {
		buf[ll + 1 + dl] = i;
		unsigned hl = 20;
		HMAC(EVP_sha1(), key, 32, buf, ll + 2 + dl, h, &hl);
		u32 k = n - pos < 20 ? n - pos : 20;
		memcpy(out + pos, h, k);
		pos += k;
	}
}

static void ap_cmac(const u8 *key, const u8 *d, u32 len, u8 *mac)
{
	size_t ml = 16;
	EVP_MAC *m = EVP_MAC_fetch(NULL, "CMAC", NULL);
	EVP_MAC_CTX *mc = EVP_MAC_CTX_new(m);
	OSSL_PARAM prm[2] = { OSSL_PARAM_construct_utf8_string("cipher", "AES-128-CBC", 0), OSSL_PARAM_construct_end() };
	EVP_MAC_init(mc, key, 16, prm);
	EVP_MAC_update(mc, d, len);
	EVP_MAC_final(mc, mac, &ml, 16);
	EVP_MAC_CTX_free(mc);
	EVP_MAC_free(m);
}

/* IEEE 802.11's SHA-256 KDF, by OpenSSL's HMAC */
static void ap_kdf(const u8 *key, u32 kl, const char *label, const u8 *data, u32 dl, u8 *out, u32 bits)
{
	u8 buf[512], h[32];
	u32 ll = strlen(label), n = bits / 8;
	for (u32 pos = 0, i = 1; pos < n; i++) {
		buf[0] = (u8)i, buf[1] = 0;
		memcpy(buf + 2, label, ll);
		memcpy(buf + 2 + ll, data, dl);
		buf[2 + ll + dl] = (u8)bits, buf[3 + ll + dl] = (u8)(bits >> 8);
		unsigned hl = 32;
		HMAC(EVP_sha256(), key, (int)kl, buf, 4 + ll + dl, h, &hl);
		u32 k = n - pos < 32 ? n - pos : 32;
		memcpy(out + pos, h, k);
		pos += k;
	}
}

static bool ap_sha256(void) { return ap.akm == 6 || ap.akm == 8; }
static u16 ap_ver(void) { return ap.akm == 8 ? 0 : ap.akm == 6 ? 3 : 2; }

static void ap_mic(const u8 *kck, u8 *frame, u32 len, u8 *mic)
{
	if (ap_sha256()) {
		ap_cmac(kck, frame, len, mic);
		return;
	}
	u8 h[20];
	unsigned hl = 20;
	HMAC(EVP_sha1(), kck, 16, frame, len, h, &hl);
	memcpy(mic, h, 16);
}

static void fw_from_air(const u8 *f, u32 len);

/* a data frame from the AP (FromDS): to dst, from src, an Ethernet type
 * and payload; encrypted once the keys are set (multicast: the GTK) */
static void ap_data(const u8 *dst, const u8 *src, u16 type, const u8 *p, u32 len)
{
	u8 f[2048], e[2048];
	put16(f, 0x0208);                       /* data, FromDS */
	put16(f + 2, 0);
	memcpy(f + 4, dst, 6);
	memcpy(f + 10, ap_mac, 6);
	memcpy(f + 16, src, 6);
	put16(f + 22, 0);
	static const u8 snap[6] = { 0xaa, 0xaa, 0x03, 0, 0, 0 };
	memcpy(f + 24, snap, 6);
	f[30] = type >> 8, f[31] = type & 0xff;
	memcpy(f + 32, p, len);
	u32 n = 32 + len;
	if (ap.keys && type != 0x888e) {
		bool group = dst[0] & 1;
		n = ccmp_encrypt(group ? (ap.rekey_done ? ap.gtk2 : ap.gtk) : ap.ptk + 32, group ? (ap.rekey_done ? 2 : 1) : 0,
				 group ? ++ap.gpn : ++ap.pn, f, n, e);
		fw_from_air(e, n);
		return;
	}
	if (ap.keys && type == 0x888e) {
		/* EAPOL after the PTK: encrypted too (the group key handshake) */
		n = ccmp_encrypt(ap.ptk + 32, 0, ++ap.pn, f, n, e);
		fw_from_air(e, n);
		return;
	}
	fw_from_air(f, n);
}

static void ap_mgmt(u16 fc, const u8 *body, u32 len)
{
	u8 f[512];
	put16(f, fc);
	put16(f + 2, 0);
	memcpy(f + 4, ap.sta, 6);
	memcpy(f + 10, ap_mac, 6);
	memcpy(f + 16, ap_mac, 6);
	put16(f + 22, 0);
	memcpy(f + 24, body, len);
	fw_from_air(f, 24 + len);
}

static void eapol_hdr(u8 *o, u16 info, u64 replay, const u8 *nonce, u32 dlen)
{
	memset(o, 0, 99);
	o[0] = 2, o[1] = 3;
	put16(o + 2, 0);
	o[2] = (u8)((95 + dlen) >> 8), o[3] = (u8)(95 + dlen);
	o[4] = 2;
	o[5] = info >> 8, o[6] = info & 0xff;
	o[7] = 0, o[8] = 16;                    /* key length: CCMP */
	for (int i = 0; i < 8; i++) o[9 + i] = (u8)(replay >> (56 - 8 * i));
	if (nonce) memcpy(o + 17, nonce, 32);
	o[97] = (u8)(dlen >> 8), o[98] = (u8)dlen;
}

static void ap_msg1(void)
{
	u8 m[128];
	for (int i = 0; i < 32; i++) ap.anonce[i] = (u8)rand();
	eapol_hdr(m, 0x0088 | ap_ver(), ++ap.replay, ap.anonce, 0);   /* pairwise, ack, the AKM's version */
	ap.m1_tries++;
	ap_data(ap.sta, ap_mac, 0x888e, m, 99);
	ap.msg = 2;
	ap.eapol_at = now;
}

/* the GTK KDE and the AP's RSN element, wrapped with the KEK */
static u32 ap_key_data(u8 *o, const u8 *gtk, int id, bool with_ie)
{
	u8 plain[192];
	u32 n = 0;
	if (with_ie) {
		/* the beacon's RSN and RSNX elements */
		memcpy(plain, ap_rsn, ap_rsn_len);
		n = ap_rsn_len;
		memcpy(plain + n, ap_rsnx, ap_rsnx_len);
		n += ap_rsnx_len;
	}
	u8 kde[] = { 0xdd, 22, 0x00, 0x0f, 0xac, 1, (u8)id, 0 };
	memcpy(plain + n, kde, sizeof(kde));
	memcpy(plain + n + 8, gtk, 16);
	n += 24;
	if (ap.mfp) {
		/* the IGTK KDE: key id 4, IPN, the key */
		u8 ik[] = { 0xdd, 28, 0x00, 0x0f, 0xac, 9, 4, 0 };
		memcpy(plain + n, ik, 8);
		for (int i = 0; i < 6; i++) plain[n + 8 + i] = (u8)(ap.ipn >> (8 * i));
		memcpy(plain + n + 14, ap.igtk, 16);
		n += 30;
	}
	if (n % 8) {
		plain[n++] = 0xdd;
		while (n % 8) plain[n++] = 0;
	}
	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	int ol;
	EVP_EncryptInit_ex(c, EVP_aes_128_wrap(), NULL, ap.ptk + 16, NULL);
	EVP_EncryptUpdate(c, o, &ol, plain, (int)n);
	EVP_CIPHER_CTX_free(c);
	return (u32)ol;
}

static void ap_eapol(const u8 *p, u32 len)
{
	if (len < 99) return;
	u16 info = (u16)(p[5] << 8 | p[6]);
	u8 f[512], mic[16];
	if (len > sizeof(f)) return;
	memcpy(f, p, len);
	memset(f + 81, 0, 16);
	if (info & 0x0008) {
		if (ap.msg == 2 && !(info & 0x0200)) {
			/* message 2: the PTK from the SNonce, the MIC, our IE */
			memcpy(ap.snonce, p + 17, 32);
			u8 d[76];
			bool alo = memcmp(ap_mac, ap.sta, 6) < 0, nlo = memcmp(ap.anonce, ap.snonce, 32) < 0;
			memcpy(d, alo ? ap_mac : ap.sta, 6);
			memcpy(d + 6, alo ? ap.sta : ap_mac, 6);
			memcpy(d + 12, nlo ? ap.anonce : ap.snonce, 32);
			memcpy(d + 44, nlo ? ap.snonce : ap.anonce, 32);
			if (ap_sha256()) ap_kdf(ap.pmk, 32, "Pairwise key expansion", d, 76, ap.ptk, 384);
			else ap_prf(ap.pmk, "Pairwise key expansion", d, 76, ap.ptk, 48);
			ap_mic(ap.ptk, f, len, mic);
			if (memcmp(mic, p + 81, 16)) {
				ap.bad_mic++;
				return;
			}
			u32 dl = (u32)(p[97] << 8 | p[98]);
			CHECK(dl == ap.sta_ie_len + (ap.assoc_had_rsnx ? 3u : 0) && !memcmp(p + 99, ap.sta_ie, ap.sta_ie_len) &&
			      (!ap.assoc_had_rsnx || !memcmp(p + 99 + ap.sta_ie_len, ap.assoc_rsnx, 3)) && (info & 7) == ap_ver(),
			      "message 2: the association's RSN (and RSNX) element, the AKM's version");
			ap.m2++;
			u8 m[256], wrapped[160];
			u32 wl = ap_key_data(wrapped, ap.gtk, 1, true);
			eapol_hdr(m, 0x13c8 | ap_ver(), ++ap.replay, ap.anonce, wl);  /* encr, secure, MIC, ack, install, pairwise */
			memcpy(m + 99, wrapped, wl);
			ap_mic(ap.ptk, m, 99 + wl, m + 81);
			ap_data(ap.sta, ap_mac, 0x888e, m, 99 + wl);
			ap.msg = 4;
		} else if (ap.msg == 4 && (info & 0x0200)) {
			ap_mic(ap.ptk, f, len, mic);
			if (memcmp(mic, p + 81, 16)) {
				ap.bad_mic++;
				return;
			}
			CHECK(p[97] == 0 && p[98] == 0, "message 4 without key data");
			ap.m4++;
			ap.keys = true;
			ap.msg = 0;
		}
	} else if (ap.rekey_sent && !ap.rekey_done) {
		ap_mic(ap.ptk, f, len, mic);
		CHECK(!memcmp(mic, p + 81, 16) && (info & 0x0300) == 0x0300, "group message 2's MIC");
		ap.group2++;
		ap.rekey_done = true;
	}
}

/* --- the AP's SAE, with OpenSSL's EC and BN (independent of sae.c): IEEE
 * 802.11-2020 12.4 as hostapd's sae.c does it, without its side-channel
 * care ------------------------------------------------------------------ */

static struct {
	EC_GROUP *g;
	BN_CTX *ctx;
	BIGNUM *p, *a, *b, *n;
	EC_POINT *pwe, *elem, *peer_elem;
	BIGNUM *rand, *scalar, *peer_scalar;
	u8 kck[32], pmk[32], token[32];
	bool h2e, token_sent, token_done, committed;
	u32 commits, confirms, tokens, h2e_commits;
} as;

static void bn32(u8 o[32], const BIGNUM *x) { BN_bn2binpad(x, o, 32); }

static void as_init(void)
{
	as.g = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
	as.ctx = BN_CTX_new();
	as.p = BN_new(), as.a = BN_new(), as.b = BN_new(), as.n = BN_new();
	EC_GROUP_get_curve(as.g, as.p, as.a, as.b, as.ctx);
	EC_GROUP_get_order(as.g, as.n, as.ctx);
	as.pwe = EC_POINT_new(as.g), as.elem = EC_POINT_new(as.g), as.peer_elem = EC_POINT_new(as.g);
	as.rand = BN_new(), as.scalar = BN_new(), as.peer_scalar = BN_new();
}

static void as_free(void)
{
	EC_POINT_free(as.pwe), EC_POINT_free(as.elem), EC_POINT_free(as.peer_elem);
	BN_free(as.rand), BN_free(as.scalar), BN_free(as.peer_scalar);
	BN_free(as.p), BN_free(as.a), BN_free(as.b), BN_free(as.n);
	BN_CTX_free(as.ctx);
	EC_GROUP_free(as.g);
}

static void max_min(u8 o[12], const u8 *x, const u8 *y)
{
	bool hi = memcmp(x, y, 6) > 0;
	memcpy(o, hi ? x : y, 6);
	memcpy(o + 6, hi ? y : x, 6);
}

/* y^2 = x^3 + ax + b */
static void as_y2(BIGNUM *r, const BIGNUM *x)
{
	BIGNUM *t = BN_new();
	BN_mod_sqr(r, x, as.p, as.ctx);
	BN_mod_mul(r, r, x, as.p, as.ctx);
	BN_mod_mul(t, as.a, x, as.p, as.ctx);
	BN_mod_add(r, r, t, as.p, as.ctx);
	BN_mod_add(r, r, as.b, as.p, as.ctx);
	BN_free(t);
}

/* hunting and pecking: the first pwd-value below p with a square y^2 */
static void as_pwe_hnp(void)
{
	u8 key[12], pb[32];
	max_min(key, ap_mac, ap.sta);
	bn32(pb, as.p);
	BIGNUM *x = BN_new(), *y2 = BN_new(), *y = BN_new();
	for (u32 c = 1; c < 100; c++) {
		u8 buf[80], seed[32], val[32];
		u32 pl = strlen(passphrase);
		memcpy(buf, passphrase, pl);
		buf[pl] = (u8)c;
		unsigned hl = 32;
		HMAC(EVP_sha256(), key, 12, buf, pl + 1, seed, &hl);
		ap_kdf(seed, 32, "SAE Hunting and Pecking", pb, 32, val, 256);
		BN_bin2bn(val, 32, x);
		if (BN_cmp(x, as.p) >= 0) continue;
		as_y2(y2, x);
		if (BN_kronecker(y2, as.p, as.ctx) != 1) continue;
		BN_mod_sqrt(y, y2, as.p, as.ctx);
		if (BN_is_odd(y) != (seed[31] & 1)) BN_sub(y, as.p, y);
		EC_POINT_set_affine_coordinates(as.g, as.pwe, x, y, as.ctx);
		break;
	}
	BN_free(x), BN_free(y2), BN_free(y);
}

/* simplified SWU, z = -10 (IEEE 802.11-2020 12.4.4.2.3) */
static void as_sswu(EC_POINT *P, const BIGNUM *u)
{
	BIGNUM *z = BN_new(), *u2 = BN_new(), *t1 = BN_new(), *m = BN_new(), *x1 = BN_new(), *x2 = BN_new(), *gx = BN_new(),
	       *y = BN_new(), *t2 = BN_new();
	BN_set_word(z, 10);
	BN_sub(z, as.p, z);
	BN_mod_sqr(u2, u, as.p, as.ctx);
	BN_mod_mul(t1, z, u2, as.p, as.ctx);    /* z u^2 */
	BN_mod_sqr(m, t1, as.p, as.ctx);
	BN_mod_add(m, m, t1, as.p, as.ctx);     /* z^2 u^4 + z u^2 */
	if (BN_is_zero(m)) {
		BN_mod_mul(t2, z, as.a, as.p, as.ctx);
		BN_mod_inverse(t2, t2, as.p, as.ctx);
		BN_mod_mul(x1, as.b, t2, as.p, as.ctx);
	} else {
		BN_mod_inverse(t2, m, as.p, as.ctx);
		BN_add_word(t2, 1);
		BN_mod_inverse(x1, as.a, as.p, as.ctx);
		BN_mod_mul(x1, x1, as.b, as.p, as.ctx);
		BN_sub(x1, as.p, x1);               /* -b/a */
		BN_mod_mul(x1, x1, t2, as.p, as.ctx);
	}
	as_y2(gx, x1);
	const BIGNUM *x = x1;
	if (BN_kronecker(gx, as.p, as.ctx) < 0) {
		BN_mod_mul(x2, t1, x1, as.p, as.ctx);
		as_y2(gx, x2);
		x = x2;
	}
	BN_mod_sqrt(y, gx, as.p, as.ctx);
	if (BN_is_odd(y) != BN_is_odd(u)) BN_sub(y, as.p, y);
	EC_POINT_set_affine_coordinates(as.g, P, x, y, as.ctx);
	BN_free(z), BN_free(u2), BN_free(t1), BN_free(m), BN_free(x1), BN_free(x2), BN_free(gx), BN_free(y), BN_free(t2);
}

static void as_pwe_h2e(void)
{
	u8 seed[32], val[48];
	unsigned hl = 32;
	HMAC(EVP_sha256(), ssid, (int)strlen(ssid), (const u8 *)passphrase, strlen(passphrase), seed, &hl);
	EC_POINT *p1 = EC_POINT_new(as.g), *p2 = EC_POINT_new(as.g), *pt = EC_POINT_new(as.g);
	BIGNUM *u = BN_new(), *v = BN_new(), *nm1 = BN_new();
	static const char *const info[2] = { "SAE Hash to Element u1 P1", "SAE Hash to Element u2 P2" };
	for (int i = 0; i < 2; i++) {
		/* HKDF-Expand(seed, info, 48) = T(1) || T(2)[0..15] */
		u8 t[64], in[64];
		u32 il = strlen(info[i]);
		memcpy(in, info[i], il);
		in[il] = 1;
		HMAC(EVP_sha256(), seed, 32, in, il + 1, t, &hl);
		memcpy(in, t, 32);
		memcpy(in + 32, info[i], il);
		in[32 + il] = 2;
		HMAC(EVP_sha256(), seed, 32, in, 33 + il, t + 32, &hl);
		memcpy(val, t, 48);
		BN_bin2bn(val, 48, u);
		BN_mod(u, u, as.p, as.ctx);
		as_sswu(i ? p2 : p1, u);
	}
	EC_POINT_add(as.g, pt, p1, p2, as.ctx);
	/* val = H(0^32, MAX || MIN) mod (n - 1) + 1; PWE = val PT */
	u8 key[12], zero[32] = { 0 }, h[32];
	max_min(key, ap_mac, ap.sta);
	HMAC(EVP_sha256(), zero, 32, key, 12, h, &hl);
	BN_bin2bn(h, 32, v);
	BN_copy(nm1, as.n);
	BN_sub_word(nm1, 1);
	BN_mod(v, v, nm1, as.ctx);
	BN_add_word(v, 1);
	EC_POINT_mul(as.g, as.pwe, NULL, pt, v, as.ctx);
	EC_POINT_free(p1), EC_POINT_free(p2), EC_POINT_free(pt);
	BN_free(u), BN_free(v), BN_free(nm1);
}

static void as_point_bin(u8 o[64], const EC_POINT *P)
{
	BIGNUM *x = BN_new(), *y = BN_new();
	EC_POINT_get_affine_coordinates(as.g, P, x, y, as.ctx);
	bn32(o, x), bn32(o + 32, y);
	BN_free(x), BN_free(y);
}

/* the AP's commit: rand, mask, scalar = rand + mask mod n, element =
 * -(mask PWE) */
static void as_commit(void)
{
	BIGNUM *mask = BN_new();
	do {
		BN_rand_range(as.rand, as.n);
		BN_rand_range(mask, as.n);
		BN_mod_add(as.scalar, as.rand, mask, as.n, as.ctx);
	} while (BN_is_zero(as.rand) || BN_is_one(as.rand) || BN_is_zero(mask) || BN_is_one(mask) || BN_is_zero(as.scalar) ||
		 BN_is_one(as.scalar));
	EC_POINT_mul(as.g, as.elem, NULL, as.pwe, mask, as.ctx);
	EC_POINT_invert(as.g, as.elem, as.ctx);
	BN_free(mask);
}

static void ap_auth(u16 alg, u16 seq, u16 status, const u8 *body, u32 len)
{
	u8 b[200];
	put16(b, alg), put16(b + 2, seq), put16(b + 4, status);
	memcpy(b + 6, body, len);
	ap_mgmt(0x00b0, b, 6 + len);
}

/* a station's SAE frame (its body after algorithm, transaction, status) */
static void ap_sae(u16 seq, u16 status, const u8 *b, u32 len)
{
	if (seq == 1) {
		as.commits++;
		as.h2e = status == 126;
		if (as.h2e) as.h2e_commits++;
		CHECK(len >= 98 && get16(b) == 19, "SAE commit: group 19");
		if (aps.h2e_only) CHECK(as.h2e, "an H2E-only AP: the station's commit with status 126");
		/* the anti-clogging token first (hostapd's sae_check_big_sync /
		 * anti-clogging when busy): as it is (hunting and pecking) or in
		 * its container (hash-to-element) */
		const u8 *sc = b + 2;
		bool has_token = false;
		if (aps.anticlog && !as.token_done) {
			if (!as.h2e && len == 2 + 32 + 96 && !memcmp(b + 2, as.token, 32)) has_token = true, sc = b + 34;
			if (as.h2e && len == 98 + 35 && b[98] == 255 && b[99] == 33 && b[100] == 93 && !memcmp(b + 101, as.token, 32))
				has_token = true;
			as.token_done = has_token;
			if (!has_token) {
				CHECK(!as.token_sent, "the anti-clogging token back with the second commit");
				for (int i = 0; i < 32; i++) as.token[i] = (u8)rand();
				u8 r[40];
				put16(r, 19);
				u32 n = 2;
				if (as.h2e) r[n++] = 255, r[n++] = 33, r[n++] = 93;
				memcpy(r + n, as.token, 32);
				n += 32;
				as.token_sent = true;
				as.tokens++;
				ap_auth(3, 1, 76, r, n);
				return;
			}
		}
		/* the station's scalar and element; our PWE, commit, K, the keys */
		BIGNUM *x = BN_new(), *y = BN_new();
		BN_bin2bn(sc, 32, as.peer_scalar);
		BN_bin2bn(sc + 32, 32, x);
		BN_bin2bn(sc + 64, 32, y);
		CHECK(EC_POINT_set_affine_coordinates(as.g, as.peer_elem, x, y, as.ctx) == 1 &&
		      EC_POINT_is_on_curve(as.g, as.peer_elem, as.ctx) == 1 && BN_cmp(as.peer_scalar, as.n) < 0,
		      "SAE: the station's element on the curve, its scalar below n");
		if (as.h2e) as_pwe_h2e();
		else as_pwe_hnp();
		as_commit();
		EC_POINT *K = EC_POINT_new(as.g);
		EC_POINT_mul(as.g, K, NULL, as.pwe, as.peer_scalar, as.ctx);
		EC_POINT_add(as.g, K, K, as.peer_elem, as.ctx);
		EC_POINT_mul(as.g, K, NULL, K, as.rand, as.ctx);
		u8 k[64], zero[32] = { 0 }, keyseed[32], val[32], keys[64];
		as_point_bin(k, K);
		unsigned hl = 32;
		HMAC(EVP_sha256(), zero, 32, k, 32, keyseed, &hl);
		BIGNUM *sum = BN_new();
		BN_mod_add(sum, as.scalar, as.peer_scalar, as.n, as.ctx);
		bn32(val, sum);
		ap_kdf(keyseed, 32, "SAE KCK and PMK", val, 32, keys, 512);
		memcpy(as.kck, keys, 32);
		memcpy(as.pmk, keys + 32, 32);
		EC_POINT_free(K), BN_free(x), BN_free(y), BN_free(sum);
		u8 r[100];
		put16(r, 19);
		bn32(r + 2, as.scalar);
		as_point_bin(r + 34, as.elem);
		as.committed = true;
		ap_auth(3, 1, as.h2e ? 126 : 0, r, 98);
		return;
	}
	if (seq == 2 && as.committed && len >= 34) {
		as.confirms++;
		/* verifier = CN(KCK, the station's send-confirm, its scalar and
		 * element, ours) */
		u8 d[194], v[32], ps[32], pe[64], os[32], oe[64];
		bn32(ps, as.peer_scalar), as_point_bin(pe, as.peer_elem), bn32(os, as.scalar), as_point_bin(oe, as.elem);
		memcpy(d, b, 2), memcpy(d + 2, ps, 32), memcpy(d + 34, pe, 64), memcpy(d + 98, os, 32), memcpy(d + 130, oe, 64);
		unsigned hl = 32;
		HMAC(EVP_sha256(), as.kck, 32, d, 194, v, &hl);
		if (memcmp(v, b + 2, 32)) {
			ap.bad_mic++;
			return;
		}
		u8 r[34];
		put16(r, 1);
		memcpy(d, r, 2), memcpy(d + 2, os, 32), memcpy(d + 34, oe, 64), memcpy(d + 98, ps, 32), memcpy(d + 130, pe, 64);
		HMAC(EVP_sha256(), as.kck, 32, d, 194, r + 2, &hl);
		memcpy(ap.pmk, as.pmk, 32);
		ap.authed = true;
		as.committed = false;
		ap_auth(3, 2, 0, r, 34);
	}
}

/* protected management frames: unicast ones by CCMP with the TK (the
 * management nonce), group ones with BIP's MMIE (the IGTK) */
static void ap_mgmt_protected(u16 fc, const u8 *body, u32 len)
{
	u8 f[512], e[600];
	put16(f, fc);
	put16(f + 2, 0);
	memcpy(f + 4, ap.sta, 6), memcpy(f + 10, ap_mac, 6), memcpy(f + 16, ap_mac, 6);
	put16(f + 22, 0);
	memcpy(f + 24, body, len);
	u32 n = ccmp_encrypt(ap.ptk + 32, 0, ++ap.mgmt_pn, f, 24 + len, e);
	fw_from_air(e, n);
}

static void ap_mgmt_bip(u16 fc, const u8 *body, u32 len, bool good)
{
	u8 f[512];
	put16(f, fc);
	put16(f + 2, 0);
	memcpy(f + 4, bcast, 6), memcpy(f + 10, ap_mac, 6), memcpy(f + 16, ap_mac, 6);
	put16(f + 22, 0);
	memcpy(f + 24, body, len);
	u8 *mm = f + 24 + len;
	mm[0] = 76, mm[1] = 16, mm[2] = 4, mm[3] = 0;
	ap.ipn++;
	for (int i = 0; i < 6; i++) mm[4 + i] = (u8)(ap.ipn >> (8 * i));
	memset(mm + 10, 0, 8);
	u8 d[512], mic[16];
	put16(d, fc & ~0x3800);
	memcpy(d + 2, f + 4, 18);
	memcpy(d + 20, f + 24, len + 18);
	ap_cmac(ap.igtk, d, 20 + len + 18, mic);
	memcpy(mm + 10, mic, 8);
	if (!good) mm[10] ^= 1;
	fw_from_air(f, 24 + len + 18);
}

static void ap_sa_query_req(void)
{
	for (int i = 0; i < 2; i++) ap.saq_id[i] = (u8)rand();
	u8 b[4] = { 8, 0, ap.saq_id[0], ap.saq_id[1] };
	ap_mgmt_protected(0x00d0, b, 4);
}

/* the router: IPv4 checksums, DHCP, ARP, ICMP */
static const u8 router_ip[4] = { 192, 168, 77, 1 }, sta_ip[4] = { 192, 168, 77, 100 };

static u16 csum(const u8 *p, u32 n)
{
	u32 s = 0;
	for (u32 i = 0; i + 1 < n; i += 2) s += (u32)(p[i] << 8 | p[i + 1]);
	if (n & 1) s += (u32)p[n - 1] << 8;
	while (s >> 16) s = (s & 0xffff) + (s >> 16);
	return (u16)~s;
}

static u32 ip_hdr(u8 *o, u8 proto, const u8 *src, const u8 *dst, u32 payload)
{
	memset(o, 0, 20);
	o[0] = 0x45;
	u32 tl = 20 + payload;
	o[2] = (u8)(tl >> 8), o[3] = (u8)tl;
	o[8] = 64;
	o[9] = proto;
	memcpy(o + 12, src, 4);
	memcpy(o + 16, dst, 4);
	u16 c = csum(o, 20);
	o[10] = c >> 8, o[11] = c & 0xff;
	return 20;
}

static void dhcp_reply(const u8 *req, u8 type)
{
	u8 ip[600], *u = ip + 20, *d = u + 8;
	memset(d, 0, 240);
	d[0] = 2, d[1] = 1, d[2] = 6;
	memcpy(d + 4, req + 4, 4);              /* xid */
	memcpy(d + 16, sta_ip, 4);              /* yiaddr */
	memcpy(d + 20, router_ip, 4);           /* siaddr */
	memcpy(d + 28, req + 28, 16);           /* chaddr */
	d[236] = 99, d[237] = 130, d[238] = 83, d[239] = 99;
	u8 *o = d + 240;
	*o++ = 53, *o++ = 1, *o++ = type;
	*o++ = 54, *o++ = 4, memcpy(o, router_ip, 4), o += 4;
	*o++ = 51, *o++ = 4, *o++ = 0, *o++ = 0, *o++ = 0x0e, *o++ = 0x10;
	*o++ = 1, *o++ = 4, *o++ = 255, *o++ = 255, *o++ = 255, *o++ = 0;
	*o++ = 3, *o++ = 4, memcpy(o, router_ip, 4), o += 4;
	*o++ = 6, *o++ = 4, memcpy(o, router_ip, 4), o += 4;
	*o++ = 255;
	u32 dl = (u32)(o - d);
	u[0] = 0, u[1] = 67, u[2] = 0, u[3] = 68;
	u[4] = (u8)((8 + dl) >> 8), u[5] = (u8)(8 + dl), u[6] = u[7] = 0;
	ip_hdr(ip, 17, router_ip, (const u8 *)"\xff\xff\xff\xff", 8 + dl);
	ap_data(bcast, ap_mac, 0x0800, ip, 28 + dl);
}

static void arp_out(u16 op, const u8 *tmac, const u8 *tip, const u8 *dst)
{
	u8 a[28] = { 0, 1, 8, 0, 6, 4, 0, (u8)op };
	memcpy(a + 8, ap_mac, 6);
	memcpy(a + 14, router_ip, 4);
	memcpy(a + 18, tmac, 6);
	memcpy(a + 24, tip, 4);
	ap_data(dst, ap_mac, 0x0806, a, 28);
}

static void ping_out(u8 tos)
{
	u8 ip[64], *i = ip + 20;
	memset(i, 0, 16);
	i[0] = 8;                               /* echo request */
	i[5] = 1, i[7] = 1;
	memcpy(i + 8, "kolibri!", 8);
	u16 c = csum(i, 16);
	i[2] = c >> 8, i[3] = c & 0xff;
	ip_hdr(ip, 1, router_ip, sta_ip, 16);
	ip[1] = tos;
	u16 c2 = (ip[10] = ip[11] = 0, csum(ip, 20));
	ip[10] = c2 >> 8, ip[11] = c2 & 0xff;
	ap_data(ap.sta, ap_mac, 0x0800, ip, 36);
}

static void ap_ip(const u8 *ip, u32 len)
{
	if (len < 20 || (ip[0] >> 4) != 4) return;
	u32 hl = (ip[0] & 15) * 4u;
	if (ip[9] == 17 && len >= hl + 8 + 240 && ip[hl + 3] == 67) {
		const u8 *d = ip + hl + 8;
		u8 type = 0;
		for (const u8 *o = d + 240; o < ip + len && *o != 255; o += 2 + o[1])
			if (o[0] == 53) type = o[2];
		if (type == 1) {
			ap.dhcp_discover++;
			dhcp_reply(d, 2);
		} else if (type == 3) {
			ap.dhcp_request++;
			dhcp_reply(d, 5);
		}
	} else if (ip[9] == 1 && len >= hl + 8 && ip[hl] == 0) {
		ap.echo_replies++;
		CHECK(!memcmp(ip + hl + 8, "kolibri!", 8), "the echo reply's data");
	}
}

/* a frame from the station (the firmware has encrypted it) */
static void ap_rx(const u8 *f, u32 len)
{
	ap.frames++;
	u16 fc = get16(f);
	if ((fc & 0x000c) == 0) {               /* management */
		u16 sub = fc & 0x00f0;
		if (sub == 0x00b0 && len >= 30 && get16(f + 24) == 3) {
			memcpy(ap.sta, f + 10, 6);
			CHECK(aps.akms & (1u << 8), "SAE only with an SAE AP");
			ap_sae(get16(f + 26), get16(f + 28), f + 30, len - 30);
		} else if (sub == 0x00b0 && len >= 30) {
			memcpy(ap.sta, f + 10, 6);
			CHECK(!memcmp(f + 4, ap_mac, 6) && get16(f + 24) == 0 && get16(f + 26) == 1, "authentication: open system, 1");
			ap.authed = true;
			u8 b[6] = { 0, 0, 2, 0, 0, 0 };
			ap_mgmt(0x00b0, b, 6);
		} else if (sub == 0x00d0 && (fc & 0x4000) && len >= 24 + 8 + 4 + 8) {
			/* a protected action frame: decrypted with the TK, the PN
			 * above the last */
			u8 pl[512];
			int kid;
			const u8 *h = f + 24;
			u64 pn = h[0] | (u64)h[1] << 8 | (u64)h[4] << 16 | (u64)h[5] << 24 | (u64)h[6] << 32 | (u64)h[7] << 40;
			if (!ap.keys || ccmp_decrypt(ap.ptk + 32, f, len, pl, &kid) < 0) {
				ap.decrypt_fail++;
				return;
			}
			if (pn <= ap.sta_mgmt_pn) {
				ap.mgmt_replays++;
				return;
			}
			ap.sta_mgmt_pn = pn;
			if (pl[24] == 8 && pl[25] == 0) {
				ap.saq_requests++;
				if (!ap.saq_silent) {
					u8 b[4] = { 8, 1, pl[26], pl[27] };
					ap_mgmt_protected(0x00d0, b, 4);
				}
			} else if (pl[24] == 8 && pl[25] == 1 && !memcmp(pl + 26, ap.saq_id, 2)) {
				ap.saq_responses++;
			}
		} else if (sub == 0x0000 && ap.authed && len >= 28) {
			/* the association request's elements: the SSID, the RSN */
			const u8 *e = f + 28;
			bool ssid_ok = false;
			ap.sta_ie_len = 0;
			ap.assoc_had_rsnx = false;
			while (e + 2 <= f + len) {
				if (e[0] == 0) ssid_ok = e[1] == strlen(ssid) && !memcmp(e + 2, ssid, e[1]);
				if (e[0] == 48 && e[1] + 2u <= sizeof(ap.sta_ie)) {
					memcpy(ap.sta_ie, e, e[1] + 2u);
					ap.sta_ie_len = e[1] + 2u;
				}
				if (e[0] == 45 && e[1] == 26) memcpy(ap.assoc_ht, e, 28), ap.assoc_had_ht = true;
				if (e[0] == 244 && e[1] == 1) memcpy(ap.assoc_rsnx, e, 3), ap.assoc_had_rsnx = true;
				if (e[0] == 221 && e[1] == 7 && !memcmp(e + 2, "\x00\x50\xf2\x02\x00\x01", 6))
					memcpy(ap.assoc_wmm, e, 9), ap.assoc_had_wmm = true;
				if (e[0] == 1 || e[0] == 50) {
					for (u32 i = 0; i < e[1]; i++) CHECK(!(e[2 + i] & 0x80), "the association's rates: none basic (%x)", e[2 + i]);
					ap.assoc_rates += e[1];
				}
				e += 2 + e[1];
			}
			CHECK(ssid_ok && ap.sta_ie_len >= 22, "the association: the SSID, an RSN element");
			CHECK((get16(f + 24) & 0x0011) == 0x0011, "the association's capabilities: ESS, privacy (%x)", get16(f + 24));
			/* the station's choice: the AKM, MFPC */
			ap.akm = ap.sta_ie[19];
			ap.mfp = aps.mfpc && (ap.sta_ie[20] & 0x80);
			CHECK(aps.akms & (1u << ap.akm), "the association's AKM one of the AP's (%u)", ap.akm);
			if (aps.mfpr) CHECK(ap.mfp, "an AP that requires PMF: the station's MFPC");
			if (ap.mfp) CHECK(ap.sta_ie_len == 28 && ap.sta_ie[27] == 6, "PMF: PMKID count 0, BIP-CMAC-128");
			ap.assocs++;
			if (ap.comeback_armed && ap.mfp) {
				/* the AP still has the old association (PMF): rejected
				 * for now, a comeback of 1000 TUs (its SA Query runs) */
				ap.comeback_armed = false;
				ap.comebacks++;
				u8 b[16] = { 0x11, 0x04, 30, 0, 0, 0, 56, 5, 3 };
				put32(b + 9, 1000);
				ap_mgmt(0x0010, b, 13);
				return;
			}
			u8 b[160] = { 0x11, 0x04, 0, 0, 0x01, 0xc0 };  /* capabilities, status 0, AID 1 */
			ap_mgmt(0x0010, b, 6 + ap_ies(b + 6));
			ap.assoced = true;
			ap.keys = false;
			ap.m1_tries = 0;
			ap.mgmt_pn = ap.sta_mgmt_pn = 0;
			ap.ipn = 0;
			for (int i = 0; i < 16; i++) ap.igtk[i] = (u8)(0xc0 + i);
			if (ap.akm != 8) PKCS5_PBKDF2_HMAC_SHA1(passphrase, (int)strlen(passphrase), (const u8 *)ssid, (int)strlen(ssid), 4096,
							       32, ap.pmk);
			ap_msg1();
		}
		return;
	}
	if ((fc & 0x000c) != 0x0008 || len < 24) return;
	if ((fc & 0x00fc) == 0x0048) {          /* a null data frame */
		CHECK((fc & 0x0300) == 0x0100 && !memcmp(f + 4, ap_mac, 6), "a null frame to the DS, to the BSSID");
		ap.nulls++;
		return;
	}
	u8 plain[2048];
	const u8 *d = f;
	u32 n = len;
	if (fc & 0x4000) {
		int kid;
		int r = ap.keys ? ccmp_decrypt(ap.ptk + 32, f, len, plain, &kid) : -1;
		if (r < 0) {
			ap.decrypt_fail++;
			return;
		}
		d = plain;
		n = (u32)r;
	}
	CHECK((get16(d) & 0x0300) == 0x0100 && !memcmp(d + 4, ap_mac, 6), "a data frame to the DS, to the BSSID");
	u16 type = (u16)(d[30] << 8 | d[31]);
	if (type == 0x888e) {
		CHECK(!(fc & 0x4000) == !ap.keys, "EAPOL encrypted exactly when the keys are in");
		ap_eapol(d + 32, n - 32);
	} else if (type == 0x0800) {
		CHECK(fc & 0x4000, "IP only encrypted");
		ap_ip(d + 32, n - 32);
	} else if (type == 0x0806 && n >= 32 + 28) {
		const u8 *a = d + 32;
		if (a[7] == 1 && !memcmp(a + 24, router_ip, 4)) {
			ap.arp_requests++;
			arp_out(2, a + 8, a + 14, a + 8);
		}
		else if (a[7] == 2) {
			ap.arp_replies++;
			if (ap.rekey_done) ap.arp_after_rekey = true;
		}
	}
}

/* --- the simulated WCN3990 ------------------------------------------------- */

#define FW_CREDIT_SIZE 1792
static const u8 fw_mac[6] = { 0x00, 0x03, 0x7f, 0x12, 0x34, 0x56 };
static struct {
	int next_eid, htt_eid, wmi_eid;
	bool mgmt_wmi;
	u32 inits, chans, scan_chans[64];
	u64 frag_bank, ring_pa, idx_pa;
	u32 ring_len, fw_idx;
	bool rx_ring, vdev, vdev_started, peer, authorized, up;
	u32 vdev_freq, peer_flags, peer_aid, peer_phymode, peer_nrates;
	u8 tk[16], gtk[4][16];
	bool tk_set, gtk_set[4];
	u32 keys_installed, mgmt_tx, htt_tx_frames, rx_delivered, rx_no_buffer, pdev_params, decrypt_fail;
	u16 scan_id;
	bool scanning;
	u32 scan_until;
	u64 pn;
	u32 eapol_unencrypted;
	bool chan_list, regdomain;
	u32 wmm[4][6], wmm_cmds, ps_param[5], ps_modes[16], nps_modes, auto_trig, smps, dummy, peer_stats;
	u32 peer_ht_caps, peer_rate_caps, peer_max_mpdu, peer_density, peer_nss, peer_n_ht, vdev_mode, vdev_center;
	u32 tid_eapol, tid_last_ip, ip_frames, tid_ef;
	/* power save as the radio does it: with PS on and nothing sent for the
	 * inactivity time it dozes; the AP keeps unicast frames for it, they
	 * come with the next beacon's TIM (every 100 TU here) */
	u32 last_tx, tim_held, tim_delivered, protected_mgmt;
	/* the offloads: the keepalive's command, the ARP offload's; ARP
	 * requests the firmware answered, null frames it sent */
	u8 sta_mac[6];
	u32 ka_cmds, ka_method, ka_interval, arp_cmds, arp_answered, nulls_sent;
	bool ka_ok, arp_valid, ns_empty;
	u8 arp_ip[4];
	struct { u8 f[2048]; u32 len; } tim_q[8];
	u32 ntim;
} fw;

static bool fw_dozing(void)
{
	return fw.nps_modes && fw.ps_modes[fw.nps_modes - 1] == WMI_STA_PS_MODE_ENABLED && fw.up &&
	       now - fw.last_tx > fw.ps_param[WMI_STA_PS_PARAM_INACTIVITY_TIME];
}

static void fw_send(u32 pipe, u8 eid, const u8 *payload, u32 len, bool credit)
{
	u8 m[2048];
	u8 t[8] = { HTC_RECORD_CREDITS, 4, 0, 0, (u8)fw.wmi_eid, 1, 0, 0 };
	u32 tl = credit ? 8 : 0;
	htc_put_hdr(m, eid, (u16)(len + tl), credit ? HTC_FLAG_TRAILER_PRESENT : 0, 0);
	m[4] = (u8)tl;
	memcpy(m + HTC_HDR_LEN, payload, len);
	if (tl) memcpy(m + HTC_HDR_LEN + len, t, tl);
	give(pipe, m, HTC_HDR_LEN + len + tl);
}

static void fw_event(u32 id, const u8 *tlvs, u32 len)
{
	u8 m[2048];
	wmi_put_hdr(m, id);
	memcpy(m + 4, tlvs, len);
	fw_send(2, (u8)fw.wmi_eid, m, 4 + len, false);
}

static void fw_htt(const u8 *p, u32 len)
{
	fw_send(1, (u8)fw.htt_eid, p, len, false);
}

static void fw_service_ready(void)
{
	u8 m[512], *p = m;
	memset(m, 0, sizeof(m));
	wmi_put_tlv(p, WMI_TLV_TAG_STRUCT_SERVICE_READY_EVENT, WMI_SVC_RDY_LEN);
	u8 *e = p + 4;
	put32(e, 0x324103d6);
	put32(e + 4, WMI_TLV_ABI_VER0);
	put32(e + 8, WMI_TLV_ABI_VER1);
	put32(e + 12, WMI_TLV_ABI_VER_NS0);
	put32(e + 16, WMI_TLV_ABI_VER_NS1);
	put32(e + 28 + 8, 2);                   /* two RF chains */
	/* HT: enabled, short GI 20/40, dynamic SMPS, TX STBC, RX STBC 1, LDPC */
	put32(e + 28 + 12, IS("legacy") ? 0 : WMI_HT_CAP_ENABLED | WMI_HT_CAP_HT20_SGI | WMI_HT_CAP_HT40_SGI |
	      WMI_HT_CAP_DYNAMIC_SMPS | WMI_HT_CAP_TX_STBC | 0x10 | WMI_HT_CAP_LDPC);
	put32(e + 28 + 44, 2);
	put32(e + 28 + 48, 64);
	p += 4 + WMI_SVC_RDY_LEN;
	wmi_put_tlv(p, WMI_TLV_TAG_STRUCT_HAL_REG_CAPABILITIES, WMI_HAL_REG_LEN);
	put32(p + 4, 0x406c);                   /* the Pro1X's EEPROM (Linux: "ath: EEPROM regdomain: 0x406c") */
	put32(p + 4 + 20, 2312), put32(p + 4 + 24, 2732), put32(p + 4 + 28, 4900), put32(p + 4 + 32, 6100);
	p += 4 + WMI_HAL_REG_LEN;
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_UINT32, 128);
	put32(p + 4 + 4 * 3, 1u << 1);          /* WOW (13) */
	put32(p + 4 + 4 * 16, 1u << 1);         /* RX_FULL_REORDER (65) */
	if (fw.mgmt_wmi) put32(p + 4 + 4 * 19, 1u << 0);   /* MGMT_TX_WMI (76) */
	put32(p + 4 + 4 * 6, 1u << 3);          /* STA_UAPSD_BASIC_AUTO_TRIG (27) */
	if (!IS("legacy")) {
		put32(p + 4 + 4 * 4, 1u << 0);      /* ARPNS_OFFLOAD (16) */
		put32(p + 4 + 4 * 7, 1u << 1);      /* STA_KEEP_ALIVE (29) */
	}
	p += 4 + 128;
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_STRUCT, 2 * (4 + WMI_MEM_REQ_LEN));
	p += 4;
	for (int i = 0; i < 2; i++) {
		wmi_put_tlv(p, WMI_TLV_TAG_STRUCT_WLAN_HOST_MEM_REQ, WMI_MEM_REQ_LEN);
		put32(p + 4, 7 + i);
		put32(p + 8, 1000 + i);         /* unit sizes rounded to 4 by the host */
		put32(p + 12, i ? 2 : 0);       /* the second: NUM_UNITS_IS_NUM_PEERS */
		put32(p + 16, 3);
		p += 4 + WMI_MEM_REQ_LEN;
	}
	fw_event(WMI_TLV_SERVICE_READY_EVENTID, m, (u32)(p - m));
}

/* a beacon of one of the APs as MGMT_RX on its channel */
static u32 beacon_status;               /* MGMT_RX's status of the next beacon */

static void fw_beacon(const u8 *bssid, const char *s, u32 chan, const u8 *rsn, u32 rsn_len, u32 snr)
{
	u8 f[512];
	memset(f, 0, sizeof(f));
	put16(f, 0x0080);
	memcpy(f + 4, bcast, 6);
	memcpy(f + 10, bssid, 6);
	memcpy(f + 16, bssid, 6);
	put16(f + 32, 100);                     /* beacon interval */
	put16(f + 34, 0x0431);                  /* ESS, privacy, short preamble, short slot */
	u32 n = 36;
	f[n++] = 0, f[n++] = (u8)strlen(s);
	memcpy(f + n, s, strlen(s));
	n += strlen(s);
	static const u8 rates[] = { 1, 8, 0x82, 0x84, 0x8b, 0x96, 12, 18, 24, 36, 3, 1, 6, 5, 4, 0, 1, 0, 0,
				    50, 4, 48, 72, 96, 108 };
	memcpy(f + n, rates, sizeof(rates));
	f[n + 12] = (u8)chan;                   /* the DS element's channel */
	n += sizeof(rates);
	if (!memcmp(bssid, ap_mac, 6)) rsn = ap_rsn, rsn_len = ap_rsn_len;
	memcpy(f + n, rsn, rsn_len);
	n += rsn_len;
	if (!memcmp(bssid, ap_mac, 6)) {
		memcpy(f + n, ap_rsnx, ap_rsnx_len);
		n += ap_rsnx_len;
		if (aps.h2e_only) f[n++] = 50, f[n++] = 1, f[n++] = 0x80 | 123;   /* the BSS membership selector */
		n += ap_ies(f + n);
	}
	u8 m[768], *p = m;
	wmi_put_tlv(p, WMI_TLV_TAG_STRUCT_MGMT_RX_HDR, WMI_MGMT_RX_LEN);
	memset(p + 4, 0, WMI_MGMT_RX_LEN);
	put32(p + 4, chan);
	put32(p + 8, snr);
	put32(p + 4 + 16, n);
	put32(p + 4 + 20, beacon_status);
	p += 4 + WMI_MGMT_RX_LEN;
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_BYTE, (u16)((n + 3) & ~3u));
	memset(p + 4, 0, (n + 3) & ~3u);
	memcpy(p + 4, f, n);
	p += 4 + ((n + 3) & ~3u);
	fw_event(WMI_TLV_MGMT_RX_EVENTID, m, (u32)(p - m));
}

static void fw_scan_event(u32 type)
{
	u8 m[64];
	wmi_put_tlv(m, WMI_TLV_TAG_STRUCT_SCAN_EVENT, WMI_SCAN_EVENT_LEN);
	memset(m + 4, 0, WMI_SCAN_EVENT_LEN);
	put32(m + 4, type);
	put32(m + 4 + 16, 0xa000 | fw.scan_id);
	fw_event(WMI_TLV_SCAN_EVENTID, m, 4 + WMI_SCAN_EVENT_LEN);
}

static bool scanned(u32 freq)
{
	for (u32 i = 0; i < fw.chans; i++)
		if (fw.scan_chans[i] == freq) return true;
	return false;
}

/* a management frame from the AP to the host, by MGMT_RX */
static void fw_mgmt_rx(const u8 *f, u32 n)
{
	u8 m[600], *p = m;
	wmi_put_tlv(p, WMI_TLV_TAG_STRUCT_MGMT_RX_HDR, WMI_MGMT_RX_LEN);
	memset(p + 4, 0, WMI_MGMT_RX_LEN);
	put32(p + 4, 6);
	put32(p + 8, 40);
	put32(p + 4 + 16, n);
	p += 4 + WMI_MGMT_RX_LEN;
	wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_BYTE, (u16)((n + 3) & ~3u));
	memset(p + 4, 0, (n + 3) & ~3u);
	memcpy(p + 4, f, n);
	p += 4 + ((n + 3) & ~3u);
	fw_event(WMI_TLV_MGMT_RX_EVENTID, m, (u32)(p - m));
}

/* a data frame from the air: decrypted with the installed key, decapped
 * to native Wi-Fi into the next host RX buffer, RX_IN_ORD_PADDR_IND */
static void fw_to_air(const u8 *f, u32 len);

static void fw_data_rx(const u8 *f, u32 len)
{
	u8 plain[2048];
	const u8 *d = f;
	u32 n = len;
	bool enc = get16(f) & 0x4000;
	if (enc) {
		bool group = f[4] & 1;
		int kid;
		/* the key id from the CCMP header */
		kid = f[24 + 3] >> 6;
		const u8 *k = group ? fw.gtk[kid] : fw.tk;
		if ((group ? !fw.gtk_set[kid] : !fw.tk_set) || ccmp_decrypt(k, f, len, plain, &kid) < 0) {
			fw.decrypt_fail++;
			return;
		}
		d = plain;
		n = len - 16;
	} else if (get16(f + 30) != 0x8e88) {
		return;                         /* only EAPOL in the clear */
	}
	/* an ARP request for the offloaded address: the firmware's answer,
	 * the host not woken */
	if (fw.arp_valid && n >= 32 + 28 && d[30] == 0x08 && d[31] == 0x06 && get16(d + 32 + 6) == 0x0100 &&
	    !memcmp(d + 32 + 24, fw.arp_ip, 4)) {
		u8 r[24 + 8 + 28];
		put16(r, 0x4108);                   /* data, to the DS, protected (the firmware's CCMP) */
		put16(r + 2, 0);
		memcpy(r + 4, ap_mac, 6);
		memcpy(r + 10, fw.sta_mac, 6);
		memcpy(r + 16, d + 32 + 8, 6);      /* the requester */
		put16(r + 22, 0);
		memcpy(r + 24, d + 24, 8);          /* LLC/SNAP, ARP */
		u8 *a = r + 32;
		memcpy(a, d + 32, 6);
		a[6] = 0, a[7] = 2;                 /* a reply */
		memcpy(a + 8, fw.sta_mac, 6);
		memcpy(a + 14, fw.arp_ip, 4);
		memcpy(a + 18, d + 32 + 8, 10);     /* the requester's MAC and address */
		fw.arp_answered++;
		fw_to_air(r, sizeof(r));
		return;
	}
	if (!fw.rx_ring || fw.fw_idx == *(u32 *)va(fw.idx_pa, 4)) {
		fw.rx_no_buffer++;
		return;
	}
	u64 *ring = (u64 *)va(fw.ring_pa, fw.ring_len * 8);
	u64 bpa = ring[fw.fw_idx];
	fw.fw_idx = (fw.fw_idx + 1) % fw.ring_len;
	u8 *b = va(bpa, HTT_RX_BUF_SIZE);
	memset(b, 0, HTT_RX_DESC_LEN);
	put32(b + HTT_RX_OFF_ATTENTION, HTT_RX_ATTN_MSDU_DONE | 3);   /* done, first and last MPDU */
	put32(b + HTT_RX_OFF_MSDU_INFO1, HTT_RX_DECAP_NATIVE_WIFI << HTT_RX_DECAP_SHIFT);
	put32(b + HTT_RX_OFF_MSDU_END_INFO0, HTT_RX_MSDU_END_FIRST | HTT_RX_MSDU_END_LAST);
	put32(b + HTT_RX_OFF_MPDU_INFO0, enc ? HTT_RX_MPDU_ENCRYPTED : 0);
	memcpy(b + HTT_RX_OFF_HDR_STATUS, f, 24);
	memcpy(b + HTT_RX_DESC_LEN, d, n);      /* native Wi-Fi: the 3-address header as it is, LLC/SNAP */
	put16(b + HTT_RX_DESC_LEN, get16(d) & ~0x4000);
	u8 m[32];
	m[0] = HTT_T2H_RX_IN_ORD_PADDR_IND;
	m[1] = 16;                              /* TID 16 */
	put16(m + 2, 3);                        /* peer id */
	m[4] = 0, m[5] = 0;
	put16(m + 6, 1);
	put64(m + 8, bpa);
	put16(m + 16, (u16)n);
	m[18] = m[19] = 0;
	fw_htt(m, 20);
	fw.rx_delivered++;
}

static void fw_from_air(const u8 *f, u32 len)
{
	if ((get16(f) & 0x000c) == 0 && (get16(f) & 0x4000) && (get16(f) & 0x00f0) != 0x00b0) {
		/* a protected unicast management frame (PMF): decrypted with the
		 * pairwise key, handed over with its Protected bit, the CCMP header
		 * and the MIC (ath10k's wmi.c: "in case of PMF it delivers
		 * decrypted frames with Protected Bit set") */
		u8 pl[600], o[600];
		int kid;
		int n = fw.tk_set ? ccmp_decrypt(fw.tk, f, len, pl, &kid) : -1;
		if (n < 0) {
			fw.decrypt_fail++;
			return;
		}
		memcpy(o, f, 32);
		memcpy(o + 32, pl + 24, (u32)n - 24);
		memcpy(o + 32 + n - 24, f + len - 8, 8);
		fw_mgmt_rx(o, (u32)n + 16);
		return;
	}
	if ((get16(f) & 0x000c) == 0) fw_mgmt_rx(f, len);
	else if (fw_dozing() && !(f[4] & 1) && fw.ntim < 8) {
		/* buffered at the AP for the dozing station */
		memcpy(fw.tim_q[fw.ntim].f, f, len);
		fw.tim_q[fw.ntim++].len = len;
		fw.tim_held++;
	} else fw_data_rx(f, len);
}

/* a frame from the host: encrypted (its Protected bit, the installed
 * pairwise key), to the AP */
static void fw_to_air(const u8 *f, u32 len)
{
	u8 e[2048];
	fw.last_tx = now;
	if ((get16(f) & 0x000c) == 0x0008 && (get16(f) & 0x4000)) {
		CHECK(fw.tk_set, "a protected frame only with the pairwise key installed");
		if (!fw.tk_set) return;
		u8 c[2048];
		memcpy(c, f, len);
		put16(c, get16(c) & ~0x4000);
		len = ccmp_encrypt(fw.tk, 0, ++fw.pn, c, len, e);
		f = e;
	} else if ((get16(f) & 0x000c) == 0x0008) {
		fw.eapol_unencrypted++;
	} else if ((get16(f) & 0x000c) == 0 && (get16(f) & 0x4000)) {
		/* a protected management frame from the host: its CCMP header
		 * (the host's PN: GENERATE_IV_MGMT) and the MIC's room; encrypted
		 * here with the pairwise key */
		CHECK(fw.tk_set && len >= 24 + 8 + 8, "a protected management frame only with the pairwise key installed");
		if (!fw.tk_set) return;
		const u8 *h = f + 24;
		u64 pn = h[0] | (u64)h[1] << 8 | (u64)h[4] << 16 | (u64)h[5] << 24 | (u64)h[6] << 32 | (u64)h[7] << 40;
		u8 c[2048];
		memcpy(c, f, 24);
		put16(c, get16(c) & ~0x4000);
		memcpy(c + 24, f + 32, len - 32 - 8);
		len = ccmp_encrypt(fw.tk, 0, pn, c, len - 16, e);
		f = e;
		fw.protected_mgmt++;
	}
	ap_rx(f, len);
}

static void fw_wmi(const u8 *c, u32 len)
{
	u32 id = wmi_id(c);
	const u8 *v = c + WMI_HDR_LEN + WMI_TLV_HDR;
	u16 tag = get16(c + WMI_HDR_LEN + 2), tl = get16(c + WMI_HDR_LEN);
	u8 m[256];
	CHECK(tl + WMI_HDR_LEN + WMI_TLV_HDR <= len, "a command's first TLV fits (%x)", id);
	switch (id) {
	case WMI_TLV_INIT_CMDID: {
		fw.inits++;
		/* the chunks: 1000 bytes x 3 (rounded: 1000), 1001 x 34 peers (1004) */
		const u8 *ch = v + WMI_INIT_LEN + WMI_TLV_HDR + WMI_RES_CFG_LEN + WMI_TLV_HDR;
		CHECK(get32(ch + 4) == 7 && get32(ch + 12) == 3000 && get32(ch + 16) == 0, "INIT's chunk 7: 3 x 1000, below 4 GiB");
		CHECK(get32(ch + 20 + 4) == 8 && get32(ch + 20 + 12) == 34 * 1004, "INIT's chunk 8: 34 peers x 1004 (%u)",
		      get32(ch + 20 + 12));
		CHECK(get32(ch + 8) >= LOW_PA && get32(ch + 8) < LOW_PA + LOW, "a chunk in the low memory");
		wmi_put_tlv(m, WMI_TLV_TAG_STRUCT_READY_EVENT, WMI_RDY_LEN);
		memset(m + 4, 0, WMI_RDY_LEN);
		put32(m + 4, WMI_TLV_ABI_VER0);
		put32(m + 8, WMI_TLV_ABI_VER1);
		/* the normal scenario as the phone's firmware: no MAC address
		 * (Linux: "invalid MAC address; choosing random") */
		if (!IS("normal")) memcpy(m + 4 + 24, fw_mac, 6);
		fw_event(WMI_TLV_READY_EVENTID, m, 4 + WMI_RDY_LEN);
		break;
	}
	case WMI_TLV_PDEV_SET_PARAM_CMDID:
		CHECK(tag == WMI_TLV_TAG_STRUCT_PDEV_SET_PARAM_CMD && get32(v) == 0, "PDEV_SET_PARAM's tag, pdev 0");
		if (get32(v + 4) == WMI_TLV_PDEV_PARAM_PEER_STATS_INFO_ENABLE) fw.peer_stats++;
		else fw.pdev_params++;
		break;
	case WMI_TLV_SCAN_CHAN_LIST_CMDID: {
		/* the regulatory domain: the world domain 0x6c with AT on top
		 * (tests/wifi/regd.c has the channels one by one) */
		u32 n = get32(v);
		const u8 *a = v + 4 + WMI_TLV_HDR;
		CHECK(n == 39 && get16(v + 4) == n * 28, "the channel list: 39 channels (%u)", n);
		u32 f0 = get32(a + 4 + 12);
		CHECK(get16(a + 2) == WMI_TLV_TAG_STRUCT_CHANNEL && get32(a + 4) == 2412 && (f0 & 0x3f) == MODE_11G &&
		      a[4 + 17] == 40 && (f0 & (WMI_CHAN_FLAG_ALLOW_HT | WMI_CHAN_FLAG_ALLOW_VHT | WMI_CHAN_FLAG_HT40_PLUS |
		      WMI_CHAN_FLAG_ADHOC_ALLOWED)) == (WMI_CHAN_FLAG_ALLOW_HT | WMI_CHAN_FLAG_ALLOW_VHT | WMI_CHAN_FLAG_HT40_PLUS |
		      WMI_CHAN_FLAG_ADHOC_ALLOWED) && !(f0 & WMI_CHAN_FLAG_PASSIVE),
		      "the first channel: 2412 MHz, 11g, 20 dBm, active, HT and HT40+ (%x)", f0);
		const u8 *c13 = a + 28 * 12, *c36 = a + 28 * 13, *c52 = a + 28 * 17, *l = a + 28 * 38;
		CHECK(get32(c13 + 4) == 2472 && (get32(c13 + 16) & WMI_CHAN_FLAG_PASSIVE) && !(get32(c13 + 16) & WMI_CHAN_FLAG_HT40_PLUS),
		      "13: passive, no HT40+");
		CHECK(get32(c36 + 4) == 5180 && (c36[4 + 12] & 0x3f) == MODE_11A && (get32(c36 + 16) & WMI_CHAN_FLAG_PASSIVE) &&
		      !(get32(c36 + 16) & WMI_CHAN_FLAG_DFS) && c36[4 + 17] == 46, "36: 11a, passive, 23 dBm");
		CHECK(get32(c52 + 4) == 5260 && (get32(c52 + 16) & (WMI_CHAN_FLAG_DFS | WMI_CHAN_FLAG_PASSIVE)) ==
		      (WMI_CHAN_FLAG_DFS | WMI_CHAN_FLAG_PASSIVE) && c52[4 + 17] == 40, "52: DFS, passive, 20 dBm");
		CHECK(get32(l + 4) == 5845 && (get32(l + 16) & WMI_CHAN_FLAG_PASSIVE) && !(get32(l + 16) & WMI_CHAN_FLAG_DFS) &&
		      l[4 + 17] == 26, "the last: 5845 MHz (169), passive, 13 dBm");
		fw.chan_list = true;
		break;
	}
	case WMI_TLV_PDEV_SET_REGDOMAIN_CMDID:
		CHECK(tag == WMI_TLV_TAG_STRUCT_PDEV_SET_REGDOMAIN_CMD && fw.chan_list && get32(v + 4) == 0x6c &&
		      get32(v + 8) == 0x6c && get32(v + 12) == 0x6c && get32(v + 16) == 0xff && get32(v + 20) == 0xff,
		      "PDEV_SET_REGDOMAIN after the channel list: 0x6c, no CTL");
		fw.regdomain = true;
		break;
	case WMI_TLV_VDEV_CREATE_CMDID:
		CHECK(get32(v) == 0 && get32(v + 4) == WMI_VDEV_TYPE_STA && !memcmp(v + 12, wifi_mac(), 6) &&
		      (IS("normal") ? (v[12] & 3) == 2 : !memcmp(v + 12, fw_mac, 6)),
		      "VDEV_CREATE: a station, our MAC (READY's, or a random local one when it has none)");
		fw.vdev = true;
		memcpy(fw.sta_mac, v + 12, 6);
		break;
	case WMI_TLV_STA_KEEPALIVE_CMDID:
		/* ath10k_wmi_tlv_op_gen_sta_keepalive: the command, then the
		 * ARP response's TLV */
		fw.ka_cmds++;
		fw.ka_ok = tag == WMI_TLV_TAG_STRUCT_STA_KEEPALIVE_CMD && tl == 16 && get32(v) == 0 && get32(v + 4) == 1 &&
			   get16(v + 16 + 2) == WMI_TLV_TAG_STRUCT_STA_KEEPALVE_ARP_RESPONSE && get16(v + 16) == 16 &&
			   len == WMI_HDR_LEN + 4 + 16 + 4 + 16;
		fw.ka_method = get32(v + 8);
		fw.ka_interval = get32(v + 12);
		break;
	case WMI_TLV_SET_ARP_NS_OFFLOAD_CMDID: {
		/* ath11k_wmi_arp_ns_offload: flags, vdev, NS ext tuples; two NS
		 * tuples, two ARP tuples, each an array of structures */
		const u8 *t = v + 12;
		bool ok = tag == WMI_TLV_TAG_STRUCT_SET_ARP_NS_OFFLOAD_CMD && tl == 12 && get32(v + 4) == 0 && get32(v + 8) == 0 &&
			  get16(t + 2) == WMI_TLV_TAG_ARRAY_STRUCT && get16(t) == 2 * 80;
		fw.ns_empty = true;
		for (int i = 0; i < 2 && ok; i++) {
			const u8 *u = t + 4 + 80 * i;
			ok = get16(u + 2) == WMI_TLV_TAG_STRUCT_NS_OFFLOAD_TUPLE && get16(u) == 76;
			fw.ns_empty &= get32(u + 4) == 0;
		}
		t += 4 + 160;
		ok = ok && get16(t + 2) == WMI_TLV_TAG_ARRAY_STRUCT && get16(t) == 2 * 24 && len == WMI_HDR_LEN + 4 + 12 + 4 + 160 + 4 + 48;
		for (int i = 0; i < 2 && ok; i++) ok = get16(t + 4 + 24 * i + 2) == WMI_TLV_TAG_STRUCT_ARP_OFFLOAD_TUPLE &&
						  get16(t + 4 + 24 * i) == 20;
		CHECK(ok, "SET_ARP_NS_OFFLOAD: its layout (ath11k's)");
		fw.arp_cmds++;
		fw.arp_valid = ok && (get32(t + 8) & 1) && get32(t + 4 + 24 + 4) == 0;
		memcpy(fw.arp_ip, t + 4 + 8, 4);
		break;
	}
	case WMI_TLV_STA_POWERSAVE_PARAM_CMDID:
		CHECK(tag == WMI_TLV_TAG_STRUCT_STA_POWERSAVE_PARAM_CMD && get32(v) == 0 && get32(v + 4) < 5, "STA_POWERSAVE_PARAM");
		if (get32(v + 4) < 5) fw.ps_param[get32(v + 4)] = get32(v + 8);
		break;
	case WMI_TLV_STA_POWERSAVE_MODE_CMDID:
		CHECK(tag == WMI_TLV_TAG_STRUCT_STA_POWERSAVE_MODE_CMD && get32(v) == 0, "STA_POWERSAVE_MODE");
		if (fw.nps_modes < 16) fw.ps_modes[fw.nps_modes++] = get32(v + 4);
		break;
	case WMI_TLV_VDEV_SET_WMM_PARAMS_CMDID:
		CHECK(tag == WMI_TLV_TAG_STRUCT_VDEV_SET_WMM_PARAMS_CMD && tl == WMI_VDEV_WMM_LEN && get32(v) == 0, "VDEV_SET_WMM_PARAMS: 116 bytes");
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 6; j++) fw.wmm[i][j] = get32(v + 4 + 28 * i + 4 + 4 * j);
		fw.wmm_cmds++;
		break;
	case WMI_TLV_STA_UAPSD_AUTO_TRIG_CMDID:
		CHECK(get32(v + 12) == 1 && get16(v + WMI_UAPSD_TRIG_LEN + 2) == WMI_TLV_TAG_ARRAY_STRUCT &&
		      get16(v + WMI_UAPSD_TRIG_LEN + 6) == WMI_TLV_TAG_STRUCT_STA_UAPSD_AUTO_TRIG_PARAM &&
		      get32(v + WMI_UAPSD_TRIG_LEN + 8 + 8) == 0 && get32(v + WMI_UAPSD_TRIG_LEN + 8 + 12) == 0xffffffffu,
		      "STA_UAPSD_AUTO_TRIG: one AC, the auto trigger off");
		fw.auto_trig++;
		break;
	case WMI_TLV_START_SCAN_CMDID: {
		CHECK(fw.vdev && !fw.vdev_started, "a scan on the created vdev, not started");
		fw.scan_id = get32(v) & 0xfff;
		CHECK((get32(v) & 0xf000) == 0xa000 && get32(v + 20) == 50 && get32(v + 24) == 150 &&
		      get32(v + 56) == (WMI_SCAN_CHAN_STAT_EVENT | WMI_SCAN_FILTER_PROBE_REQ), "START_SCAN's values");
		const u8 *p = v + WMI_START_SCAN_LEN;
		fw.chans = get16(p) / 4;
		for (u32 i = 0; i < fw.chans && i < 64; i++) fw.scan_chans[i] = get32(p + 4 + 4 * i);
		p += 4 + get16(p);
		CHECK(get16(p) == WMI_SSID_LEN && get32(p + 4) == strlen(ssid) && !memcmp(p + 8, ssid, strlen(ssid)),
		      "START_SCAN: the SSID to probe for");
		fw_scan_event(WMI_SCAN_EVENT_STARTED);
		fw.scanning = true;
		fw.scan_until = now + 300;
		break;
	}
	case WMI_TLV_VDEV_START_REQUEST_CMDID: {
		const u8 *ch = v + WMI_VDEV_START_LEN + WMI_TLV_HDR;
		fw.vdev_freq = get32(ch);
		fw.vdev_mode = get32(ch + 12);
		fw.vdev_center = get32(ch + 4);
		CHECK(get32(v + 8) == 100 && get32(v + 12) == 1 && fw.vdev_mode <= MODE_11NG_HT40 && ch[17] == 40,
		      "VDEV_START: beacon 100, DTIM 1, a mode alone (no flags: a station's), 20 dBm (%x)", fw.vdev_mode);
		fw.vdev_started = true;
		wmi_put_tlv(m, WMI_TLV_TAG_STRUCT_VDEV_START_RESPONSE_EVENT, WMI_VDEV_START_RESP_LEN);
		memset(m + 4, 0, WMI_VDEV_START_RESP_LEN);
		fw_event(WMI_TLV_VDEV_START_RESP_EVENTID, m, 4 + WMI_VDEV_START_RESP_LEN);
		break;
	}
	case WMI_TLV_PEER_CREATE_CMDID:
		CHECK(!memcmp(v + 4, ap_mac, 6) && fw.vdev_started, "PEER_CREATE: the AP, on the started vdev");
		fw.peer = true;
		m[0] = HTT_T2H_PEER_MAP;
		m[1] = 0;
		put16(m + 2, 3);
		memcpy(m + 4, v + 4, 6);
		m[10] = m[11] = 0;
		fw_htt(m, 12);
		break;
	case WMI_TLV_PEER_DELETE_CMDID:
		fw.peer = false;
		break;
	case WMI_TLV_MGMT_TX_SEND_CMD: {
		u32 desc = get32(v + 4), flen = get32(v + 20), blen = get32(v + 24);
		u64 pa = get64(v + 12);
		const u8 *f = va(pa, flen);
		CHECK(blen == ((flen < 64 ? flen : 64) + 3) / 4 * 4 && !memcmp(v + WMI_MGMT_TX_LEN + WMI_TLV_HDR, f, flen < blen ? flen : blen),
		      "MGMT_TX_SEND: the frame's start copied");
		fw.mgmt_tx++;
		fw_to_air(f, flen);
		/* the completion, bundled (the host asked: WMI_TLV_FLAG_MGMT_BUNDLE_TX_COMPL) */
		u8 *p = m;
		wmi_put_tlv(p, WMI_TLV_TAG_STRUCT_MGMT_TX_COMPL_BUNDLE_EVENT, 4);
		put32(p + 4, 1);
		p += 8;
		wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_UINT32, 4);
		put32(p + 4, desc);
		p += 8;
		wmi_put_tlv(p, WMI_TLV_TAG_ARRAY_UINT32, 4);
		put32(p + 4, 0);
		p += 8;
		fw_event(WMI_TLV_MGMT_TX_BUNDLE_COMPLETION_EVENTID, m, (u32)(p - m));
		break;
	}
	case WMI_TLV_PEER_ASSOC_CMDID:
		fw.peer_flags = get32(v + 20);
		fw.peer_aid = get32(v + 16);
		fw.peer_phymode = get32(v + 56);
		fw.peer_nrates = get32(v + 68);
		fw.peer_ht_caps = get32(v + 32);
		fw.peer_max_mpdu = get32(v + 36);
		fw.peer_density = get32(v + 40);
		fw.peer_rate_caps = get32(v + 44);
		fw.peer_nss = get32(v + 48);
		fw.peer_n_ht = get32(v + 72);
		{
			/* the HT rates' array after the legacy rates' */
			const u8 *t = v + WMI_PEER_ASSOC_LEN + WMI_TLV_HDR + ((fw.peer_nrates + 3) & ~3u);
			CHECK(get16(t) == ((fw.peer_n_ht + 3) & ~3u) && get16(t + 2) == WMI_TLV_TAG_ARRAY_BYTE &&
			      (!fw.peer_n_ht || (t[4] == 0 && t[4 + fw.peer_n_ht - 1] == fw.peer_n_ht - 1)), "PEER_ASSOC: the HT rates' array");
		}
		CHECK(!memcmp(v, ap_mac, 6) && get32(v + 12) == 1 && get32(v + 28) == 1,
		      "PEER_ASSOC: the AP, new, listen interval 1");
		{
			const u8 *r = v + WMI_PEER_ASSOC_LEN + WMI_TLV_HDR;
			CHECK(r[0] == 0x82 && r[3] == 0x96 && r[4] == 12 && r[11] == 108, "PEER_ASSOC's rates: CCK with bit 7");
		}
		break;
	case WMI_TLV_VDEV_UP_CMDID:
		CHECK(get32(v + 4) == 1 && !memcmp(v + 8, ap_mac, 6), "VDEV_UP: AID 1, the BSSID");
		fw.up = true;
		break;
	case WMI_TLV_VDEV_INSTALL_KEY_CMDID: {
		u32 idx = get32(v + 12), flags = get32(v + 16), cipher = get32(v + 20), kl = get32(v + 80);
		const u8 *k = v + WMI_INSTALL_KEY_LEN + WMI_TLV_HDR;
		CHECK(cipher == WMI_TLV_CIPHER_AES_CCM && kl == 16 && !memcmp(v + 4, ap_mac, 6), "INSTALL_KEY: CCMP, 16, the AP");
		if (flags == WMI_KEY_PAIRWISE) {
			memcpy(fw.tk, k, 16);
			fw.tk_set = true;
		} else if (idx < 4) {
			memcpy(fw.gtk[idx], k, 16);
			fw.gtk_set[idx] = true;
		}
		fw.keys_installed++;
		m[0] = HTT_T2H_SEC_IND;
		m[1] = (u8)(6 | (flags == WMI_KEY_PAIRWISE ? 0x80 : 0));
		put16(m + 2, 3);
		memset(m + 4, 0, 24);
		fw_htt(m, 28);
		break;
	}
	case WMI_TLV_PEER_SET_PARAM_CMDID:
		if (get32(v + 12) == WMI_TLV_PEER_AUTHORIZE) fw.authorized = get32(v + 16) == 1;
		if (get32(v + 12) == WMI_TLV_PEER_SMPS_STATE) fw.smps = get32(v + 16) + 1;
		if (get32(v + 12) == WMI_TLV_PEER_DUMMY_VAR) fw.dummy++;
		break;
	case WMI_TLV_VDEV_DOWN_CMDID:
		fw.up = false;
		break;
	case WMI_TLV_VDEV_STOP_CMDID:
		fw.vdev_started = false;
		fw.tk_set = false;
		memset(fw.gtk_set, 0, sizeof(fw.gtk_set));
		break;
	default:
		CHECK(0, "a WMI command the simulation does not know: %x", id);
	}
}

static void fw_htt_cmd(const u8 *p, u32 len)
{
	u8 m[16];
	switch (p[0]) {
	case HTT_H2T_VERSION_REQ:
		CHECK(len == 4, "VERSION_REQ: 4 bytes");
		m[0] = HTT_T2H_VERSION_CONF, m[1] = 4, m[2] = 3, m[3] = 0;
		fw_htt(m, 4);
		break;
	case HTT_H2T_FRAG_DESC_BANK_CFG:
		CHECK(len == 64 && p[2] == 1 && p[3] == HTT_EXT_DESC_LEN, "FRAG_DESC_BANK_CFG: one bank of 72-byte descriptors");
		fw.frag_bank = get64(p + 4);
		break;
	case HTT_H2T_RX_RING_CFG:
		CHECK(len == 48 && p[1] == 1, "RX_RING_CFG: one ring, 48 bytes");
		fw.idx_pa = get64(p + 4);
		fw.ring_pa = get64(p + 12);
		fw.ring_len = get16(p + 20);
		/* fw_idx_init_val: how far the host has filled the ring
		 * (ath10k_htt_send_rx_ring_cfg_64 sends the alloc index after
		 * ath10k_htt_rx_ring_refill); the hardware takes from 0 */
		CHECK(get16(p + 26) == *(u32 *)va(fw.idx_pa, 4), "fw_idx_init_val: the host's fill index");
		fw.fw_idx = 0;
		CHECK(fw.ring_len == HTT_RX_RING_SIZE && get16(p + 22) == 2048 && get16(p + 24) == 0xffff,
		      "RX_RING_CFG: 2048 buffers of 2048 bytes, all flags");
		CHECK(get16(p + 28) == 74 && get16(p + 30) == 90 && get16(p + 40) == 7 && get16(p + 46) == 2,
		      "RX_RING_CFG: the WCN3990's descriptor offsets (header 74, payload 90, msdu start 7, frag 2)");
		CHECK(*(u32 *)va(fw.idx_pa, 4) == 256, "the host filled 256 buffers before RX_RING_CFG (%u)", *(u32 *)va(fw.idx_pa, 4));
		fw.rx_ring = true;
		break;
	case HTT_H2T_AGGR_CFG:
		CHECK(len == 4 && p[1] == 64 && p[2] == 3, "AGGR_CFG v2: 64, 3");
		break;
	case HTT_H2T_TX_FRM: {
		const u8 *d = p + 1;
		u32 mode = d[0] >> 5, flen = get16(d + 3), id = get16(d + 5);
		u64 frags = get64(d + 7);
		CHECK(len == 1 + HTT_TX_DESC_LEN + (((flen < 46 ? flen : 46) + 3) & ~3u), "TX_FRM: the descriptor and the prefetch (%u)", len);
		CHECK(get16(d + 1) & HTT_TX_FLAGS1_POSTPONED, "TX_FRM: postponed set");
		const u8 *f;
		if (mode == HTT_TX_MODE_MGMT) {
			f = va(frags, flen);
			CHECK((get16(d + 1) >> 6 & 0x1f) == HTT_TID_MGMT, "management by HTT: TID 17");
		} else {
			CHECK(frags >= fw.frag_bank && frags < fw.frag_bank + 64 * HTT_EXT_DESC_LEN, "the fragment list in the bank");
			const u8 *e = va(frags, HTT_EXT_DESC_LEN);
			u64 fpa = get32(e + 24) | (u64)get16(e + 28) << 32;
			CHECK(get16(e + 30) == flen, "the fragment's length");
			f = va(fpa, flen);
			CHECK(!memcmp(f, p + 1 + HTT_TX_DESC_LEN, flen < 46 ? flen : 46), "the prefetch is the frame's start");
		}
		fw.htt_tx_frames++;
		if (mode == HTT_TX_MODE_NATIVE_WIFI && flen >= 32 + 20) {
			u32 tid = get16(d + 1) >> 6 & 0x1f;
			if (f[30] == 0x88 && f[31] == 0x8e) fw.tid_eapol = tid;
			else if (f[30] == 0x08 && f[31] == 0x00) {
				fw.ip_frames++;
				fw.tid_last_ip = tid;
				if ((f[32 + 1] & 0xfc) == 0xb8) fw.tid_ef = tid;     /* DSCP 46 (EF) */
			}
		}
		fw_to_air(f, flen);
		m[0] = HTT_T2H_TX_COMPL_IND, m[1] = 0, m[2] = 1, m[3] = 0;
		put16(m + 4, (u16)id);
		fw_htt(m, 6);
		break;
	}
	default:
		CHECK(0, "an HTT message the simulation does not know: %u", p[0]);
	}
}

static void fw_take(u32 ce, u32 tid, const u8 *d, u32 len)
{
	u16 plen = get16(d + 2);
	u8 eid = d[0];
	const u8 *p = d + HTC_HDR_LEN;
	CHECK(plen + HTC_HDR_LEN == len, "HTC's length is the transfer's (%u %u)", plen + HTC_HDR_LEN, len);
	if (ce == 0) {
		CHECK(eid == 0 && tid == 0, "HTC control on endpoint 0");
		u8 r[8] = { 0 };
		if (get16(p) == HTC_MSG_CONNECT_SERVICE) {
			u16 svc = get16(p + 2);
			put16(r, HTC_MSG_CONNECT_SERVICE_RESP);
			put16(r + 2, svc);
			r[4] = HTC_CONN_STATUS_SUCCESS;
			r[5] = (u8)fw.next_eid++;
			put16(r + 6, FW_CREDIT_SIZE);
			if (svc == HTC_SVC_HTT_DATA_MSG) fw.htt_eid = r[5];
			if (svc == HTC_SVC_WMI_CONTROL) {
				fw.wmi_eid = r[5];
				CHECK(fw.htt_eid == 1, "HTT connected before WMI (ath10k_core_start)");
			}
			fw_send(2, 0, r, 8, false);
		} else if (get16(p) == HTC_MSG_SETUP_COMPLETE_EX) {
			fw_service_ready();
		}
	} else if (ce == 3) {
		CHECK(eid == fw.wmi_eid && tid == eid && (d[1] & HTC_FLAG_NEED_CREDIT_UPDATE), "WMI on its endpoint, a credit asked for");
		fw_wmi(p, plen);
		/* the credit back, in a credit report on endpoint 0 */
		fw_send(2, 0, (const u8 *)"", 0, true);
	} else if (ce == 4) {
		CHECK(eid == fw.htt_eid, "HTT on its endpoint");
		fw_htt_cmd(p, plen);
	}
}

static void fw_poll(void)
{
	give_pending();
	target_take(0, fw_take);
	target_take(3, fw_take);
	target_take(4, fw_take);
	if (fw.scanning && (s32)(now - fw.scan_until) >= 0) {
		fw.scanning = false;
		if (!IS("noap") && scanned(2437)) {
			/* a CRC error (dropped, even with more SNR), the others with
			 * EXT_INFO (0x40: taken, as Linux) */
			beacon_status = 0x01;
			fw_beacon(other_mac, ssid, 6, rsn_ccmp, sizeof(rsn_ccmp), 90);
			beacon_status = 0x40;
			fw_beacon(other_mac, "Neighbours", 6, rsn_ccmp, sizeof(rsn_ccmp), 30);
			fw_beacon(tkip_mac, ssid, 6, rsn_tkip, sizeof(rsn_tkip), 60);
			fw_beacon(ap_mac, ssid, 6, rsn_ccmp, sizeof(rsn_ccmp), 45);
			beacon_status = 0;
		}
		fw_scan_event(WMI_SCAN_EVENT_COMPLETED);
	}
	/* the keepalive: a null frame after its interval without a frame out */
	if (fw.up && fw.ka_cmds && fw.ka_interval && now - fw.last_tx >= fw.ka_interval * 1000) {
		u8 nf[24] = { 0x48, 0x01 };         /* null data, to the DS */
		memcpy(nf + 4, ap_mac, 6);
		memcpy(nf + 10, fw.sta_mac, 6);
		memcpy(nf + 16, ap_mac, 6);
		fw.last_tx = now;
		fw.nulls_sent++;
		ap_rx(nf, 24);
	}
	/* the beacon's TIM: what the AP kept, fetched (PS-Poll or a null
	 * frame: the firmware's) */
	if (fw.ntim && now % 100 == 0) {
		for (u32 i = 0; i < fw.ntim; i++) fw_data_rx(fw.tim_q[i].f, fw.tim_q[i].len);
		fw.tim_delivered += fw.ntim;
		fw.ntim = 0;
	}
	give_pending();
}

/* --- WLFW (tests/modem has the QMI messages) --------------------------------- */

static int enable_calls;
static u32 enable_at;
static void wlan_enable(void) { enable_calls++; enable_at = now + 30; }
static u32 now_ms(void) { return now; }
static void rnd(void *p, u32 n) { for (u32 i = 0; i < n; i++) ((u8 *)p)[i] = (u8)rand(); }

/* --- lwIP above the driver ------------------------------------------------------ */

u32_t sys_now(void) { return now; }
static struct netif nif;
static bool nif_added;
static int links_up, links_down;

static err_t nif_out(struct netif *n, struct pbuf *p)
{
	u8 b[1600];
	pbuf_copy_partial(p, b, p->tot_len, 0);
	return wifi_send(b, p->tot_len) ? ERR_MEM : ERR_OK;
}

static err_t nif_init(struct netif *n)
{
	n->name[0] = 'w', n->name[1] = 'l';
	n->hwaddr_len = 6;
	memcpy(n->hwaddr, wifi_mac(), 6);
	n->mtu = 1500;
	n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
	n->output = etharp_output;
	n->linkoutput = nif_out;
	return ERR_OK;
}

static void rx_eth(const u8 *eth, u32 len)
{
	struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16)len, PBUF_POOL);
	if (!p) return;
	pbuf_take(p, eth, (u16)len);
	if (nif.input(p, &nif) != ERR_OK) pbuf_free(p);
}

static void on_link(bool up, const u8 *m)
{
	if (up) {
		links_up++;
		if (!nif_added) {
			netif_add(&nif, NULL, NULL, NULL, NULL, nif_init, ethernet_input);
			netif_set_default(&nif);
			netif_set_up(&nif);
			nif_added = true;
		}
		netif_set_link_up(&nif);
		dhcp_start(&nif);
	} else {
		links_down++;
		netif_set_link_down(&nif);
	}
}

/* --- the run ----------------------------------------------------------------------- */

static void run_until(u32 end, bool (*done)(void))
{
	while (now < end && !(done && done())) {
		now += 10;
		if (enable_at && (s32)(now - enable_at) >= 0) {
			enable_at = 0;
			wifi_wlan_enabled(0);
		}
		fw_poll();
		wifi_poll();
		fw_poll();
		wifi_poll();
		sys_check_timeouts();
		/* lwIP's address to the firmware's ARP offload (wifi_dev.c's poll) */
		static u32 seen;
		u32 a = nif_added ? ip4_addr_get_u32(netif_ip4_addr(&nif)) : 0;
		if (a != seen) {
			seen = a;
			u8 ip[4] = { (u8)a, (u8)(a >> 8), (u8)(a >> 16), (u8)(a >> 24) };
			wifi_set_ipv4(ip);
		}
		/* the AP's own doings: message 1 again if message 2 does not come
		 * (hostapd: 4 tries a second apart) */
		if (ap.msg == 2 && now - ap.eapol_at > 1000 && ap.m1_tries < 4) ap_msg1();
	}
}

static bool is_up(void) { return wifi_state() == WIFI_UP; }
static bool leased(void) { return nif_added && ip4_addr_get_u32(netif_ip4_addr(&nif)) != 0; }
static bool echoed(void) { return ap.echo_replies > 0; }
static bool rekeyed(void) { return ap.arp_after_rekey; }

int main(int argc, char **argv)
{
	if (argc > 1) scenario = argv[1];
	arena = calloc(1, ARENA);
	low = calloc(1, LOW);
	srand(7);
	fw.next_eid = 1;
	fw.mgmt_wmi = !IS("wrongpsk");
	/* the AP: HT40 with WMM (normal), none of it (legacy), HT20 with
	 * U-APSD and admission control on VO (uapsd) */
	apc.ht = !IS("legacy");
	apc.ht40 = IS("normal");
	apc.wmm = !IS("legacy");
	apc.uapsd = !IS("legacy");
	apc.acm_vo = IS("uapsd");
	/* the AP's security: WPA2-PSK (normal and the others), WPA3-SAE only
	 * with PMF required and the anti-clogging token (sae), SAE by
	 * hash-to-element only (h2e), the transition mode: PSK and SAE, PMF
	 * capable, H2E offered (mixed), PSK and PSK-SHA256 with PMF required
	 * (psk256) */
	bool wpa3 = IS("sae") || IS("h2e") || IS("mixed") || IS("psk256");
	aps.akms = IS("sae") || IS("h2e") ? 1u << 8 : IS("mixed") ? (1u << 2 | 1u << 8) : IS("psk256") ? (1u << 2 | 1u << 6) : 1u << 2;
	aps.mfpc = wpa3;
	aps.mfpr = IS("sae") || IS("h2e") || IS("psk256");
	aps.h2e = IS("h2e") || IS("mixed");
	aps.h2e_only = IS("h2e");
	aps.anticlog = IS("sae") || IS("h2e");
	ap_build_rsn();
	as_init();
	PKCS5_PBKDF2_HMAC_SHA1(passphrase, (int)strlen(passphrase), (const u8 *)ssid, (int)strlen(ssid), 4096, 32, ap.pmk);
	for (int i = 0; i < 16; i++) ap.gtk[i] = (u8)(0x40 + i), ap.gtk2[i] = (u8)(0x80 + i);
	/* the engines as Linux left them (indices at 5, swaps on) */
	for (u32 id = 0; id < CE_COUNT; id++) {
		cset(id, CE_SR_WR_INDEX, 5), cset(id, CE_CURRENT_SRRI, 5);
		cset(id, CE_DST_WR_INDEX, 5), cset(id, CE_CURRENT_DRRI, 5);
		cset(id, CE_CTRL1, 0x00060000u);
		shadow[id] = 5;
	}
	lwip_init();

	struct wifi_conf c;
	const char *why;
	char text[200];
	snprintf(text, sizeof(text), "# the device test's network\nssid=%s\r\npsk=%s\n%s", ssid,
		 IS("wrongpsk") ? "not the passphrase" : passphrase,
		 IS("uapsd") ? "uapsd=on\ncountry=AT\n" : IS("legacy") ? "powersave=off\n" : "");
	CHECK(wifi_conf_parse(text, strlen(text), &c, &why) == 0 && c.ssid_len == 10 && !c.have_bssid, "the configuration file");
	CHECK(wifi_conf_parse("psk=x\n", 6, &c, &why) == -1, "no SSID: refused (%s)", why);
	CHECK(wifi_conf_parse("ssid=a\npsk=12345678\nbssid=02:a9:00:00:00:06\n", 44, &c, &why) == 0 && c.have_bssid &&
	      !memcmp(c.bssid, ap_mac, 6) && !strcmp(c.country, "AT") && !c.no_ps && !c.uapsd_queues,
	      "a BSSID; AT, power save on, no U-APSD by default");
	CHECK(wifi_conf_parse("ssid=a\npsk=12345678\ncountry=de\n", 32, &c, &why) == -1 &&
	      wifi_conf_parse("ssid=a\npsk=12345678\ncountry=00\nuapsd=on\npowersave=off\n", 54, &c, &why) == 0 &&
	      !strcmp(c.country, "00") && c.no_ps && c.uapsd_queues == 0x0f, "country=de refused; 00, uapsd=on, powersave=off");
	CHECK(wifi_conf_parse("ssid=a\npsk=12345678\noffload=off\n", 31, &c, &why) == 0 && c.no_offload &&
	      wifi_conf_parse("ssid=a\npsk=12345678\noffload=no\n", 30, &c, &why) == -1, "offload=off; offload=no refused");
	wifi_conf_parse(text, strlen(text), &c, &why);
	struct wifi_hw h = { { rd, wr, dma_alloc }, dma_alloc32, wlan_enable, now_ms, rnd, rx_eth, on_link };
	wifi_start(&h, &c);
	CHECK(enable_calls == 1 && wifi_state() == WIFI_ENABLE, "WLAN_CFG and WLAN_MODE asked for");
	/* the target: HTC READY once the buffers are posted */
	run_until(now + 50, NULL);
	CHECK(wifi_state() == WIFI_HTC_READY, "the engines up, waiting for READY (%s)", wifi_state_name(wifi_state()));
	{
		u8 p[12] = { 0 };
		put16(p, HTC_MSG_READY);
		put16(p + 2, 3);
		put16(p + 4, FW_CREDIT_SIZE);
		p[6] = 8, p[8] = 1;
		fw.wmi_eid = 0;
		fw_send(2, 0, p, sizeof(p), false);
	}

	if (IS("noap")) {
		run_until(now + 40000, NULL);
		CHECK(wifi_state() != WIFI_UP && wifi_stats()->scans >= 3, "no AP: scans again and again (%u)", wifi_stats()->scans);
		CHECK(bad_access == 0, "no access outside the memory (%u)", bad_access);
		printf("%d passed, %d failed (noap)\n", passed, failed);
		return failed != 0;
	}
	if (IS("wrongpsk")) {
		run_until(now + 30000, NULL);
		CHECK(!ap.keys && ap.bad_mic >= 2 && ap.m2 == 0, "a wrong PSK: message 2's MIC wrong at the AP (%u)", ap.bad_mic);
		CHECK(wifi_state() != WIFI_UP && wifi_stats()->restarts >= 1, "never up; tried again (%u)", wifi_stats()->restarts);
		CHECK(fw.mgmt_tx == 0 && fw.htt_tx_frames > 0, "management frames by HTT without MGMT_TX_WMI");
		CHECK(bad_access == 0, "no access outside the memory (%u)", bad_access);
		printf("%d passed, %d failed (wrongpsk)\n", passed, failed);
		return failed != 0;
	}

	run_until(now + 20000, is_up);
	CHECK(is_up(), "up (%s)", wifi_state_name(wifi_state()));
	run_until(now + 100, NULL);             /* the commands of the way up through the firmware */
	CHECK(fw.inits == 1 && fw.pdev_params == 5 && fw.vdev_freq == 2437 && fw.mgmt_tx == (wpa3 ? fw.mgmt_tx : 2) && fw.regdomain,
	      "INIT, 5 parameters, the regulatory domain, 2437 MHz, auth and assoc by WMI");
	CHECK(!memcmp(wifi_stats()->bssid, ap_mac, 6), "the CCMP AP: not the TKIP one with more SNR, nor the other SSID, "
	      "nor a beacon with a CRC error; one with EXT_INFO taken");
	if (IS("normal")) {
		/* HT40 above channel 6 (mac80211's chandef, ath10k's vdev and peer) */
		CHECK(fw.vdev_mode == MODE_11NG_HT40 && fw.vdev_center == 2447, "VDEV_START: 11ng HT40, center 2447 (%u %u)",
		      fw.vdev_mode, fw.vdev_center);
		CHECK(ap.assoc_had_ht && get16(ap.assoc_ht + 2) == 0x11ef && ap.assoc_ht[4] == 0x1b && ap.assoc_ht[5] == 0xff &&
		      ap.assoc_ht[6] == 0xff && ap.assoc_ht[7] == 0 && ap.assoc_ht[5 + 12] == 0x01,
		      "the association's HT capabilities: ath10k's (LDPC, 20/40, SM PS off, SGI, STBC), 64K/8 us, MCS 0-15 (%x)",
		      get16(ap.assoc_ht + 2));
		CHECK(ap.assoc_had_wmm && ap.assoc_wmm[8] == 0 && ap.assoc_rates == 12, "the WMM information element, QoS info 0; 12 rates");
		CHECK(fw.peer_flags == (WMI_TLV_PEER_AUTH | WMI_TLV_PEER_NEED_PTK_4_WAY | WMI_TLV_PEER_QOS | WMI_TLV_PEER_HT |
		      WMI_TLV_PEER_40MHZ | WMI_TLV_PEER_LDPC | WMI_TLV_PEER_STBC) && fw.peer_aid == 1 && fw.peer_phymode == MODE_11NG_HT40 &&
		      fw.peer_nrates == 12, "PEER_ASSOC: auth, 4-way, QoS, HT, 40 MHz, LDPC, STBC; AID 1, 11ng HT40, 12 rates (%x)",
		      fw.peer_flags);
		CHECK(fw.peer_ht_caps == AP_HT_CAP && fw.peer_rate_caps == (WMI_RC_HT_FLAG | WMI_RC_CW40_FLAG | WMI_RC_SGI_FLAG |
		      1u << WMI_RC_RX_STBC_FLAG_S | WMI_RC_DS_FLAG) && fw.peer_max_mpdu == 65535 && fw.peer_density == 4 &&
		      fw.peer_nss == 2 && fw.peer_n_ht == 16, "PEER_ASSOC's HT: the AP's caps, HT/40/SGI/STBC/2 streams, 64K, 4 us, "
		      "MCS 0-15 (%x %x)", fw.peer_ht_caps, fw.peer_rate_caps);
		CHECK(fw.smps == 1 + WMI_PEER_SMPS_PS_NONE && fw.dummy >= 1 && fw.peer_stats >= 1,
		      "after PEER_ASSOC: SMPS none, peer stats, the dummy peer parameter");
		/* the AP's WMM parameters (BE, BK, VI, VO as the command has them) */
		static const u32 want[4][4] = { { 15, 1023, 3, 0 }, { 15, 1023, 7, 0 }, { 7, 15, 2, 3008 }, { 3, 7, 2, 1504 } };
		bool ok = true;
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++) ok &= fw.wmm[i][j] == want[i][j];
		CHECK(ok && fw.wmm_cmds == 8 && fw.auto_trig == 8, "VDEV_SET_WMM_PARAMS: the AP's (BE 15/1023/3, BK 7, VI 7/15/2/3008 us, "
		      "VO 3/7/2/1504 us), 4 at the start and 4 at the association; the U-APSD auto trigger off (%u %u)", fw.wmm_cmds, fw.auto_trig);
		CHECK(fw.ps_param[WMI_STA_PS_PARAM_RX_WAKE_POLICY] == WMI_STA_PS_RX_WAKE_POLICY_WAKE &&
		      fw.ps_param[WMI_STA_PS_PARAM_TX_WAKE_THRESHOLD] == WMI_STA_PS_TX_WAKE_THRESHOLD_ALWAYS &&
		      fw.ps_param[WMI_STA_PS_PARAM_PSPOLL_COUNT] == WMI_STA_PS_PSPOLL_COUNT_NO_MAX && fw.ps_param[WMI_STA_PS_PARAM_UAPSD] == 0 &&
		      fw.ps_param[WMI_STA_PS_PARAM_INACTIVITY_TIME] == 102, "power save: PS-Poll (no U-APSD), inactivity 102 ms");
		CHECK(fw.nps_modes == 2 && fw.ps_modes[0] == WMI_STA_PS_MODE_DISABLED && fw.ps_modes[1] == WMI_STA_PS_MODE_ENABLED &&
		      wifi_stats()->ps, "power save: off at the vdev's start, on once authorized (%u)", fw.nps_modes);
		CHECK(fw.tid_eapol == 7, "EAPOL with TID 7 (control port)");
	} else if (IS("legacy")) {
		CHECK(fw.vdev_mode == MODE_11G && fw.peer_flags == (WMI_TLV_PEER_AUTH | WMI_TLV_PEER_NEED_PTK_4_WAY) &&
		      fw.peer_phymode == MODE_11G && fw.peer_nrates == 12 && fw.peer_n_ht == 0 && !ap.assoc_had_ht && !ap.assoc_had_wmm,
		      "legacy: 11g, no HT, no WMM, auth + 4-way, 12 rates");
		CHECK(fw.tid_eapol == HTT_TID_NON_QOS && fw.nps_modes == 1 && fw.ps_modes[0] == WMI_STA_PS_MODE_DISABLED,
		      "legacy: the non-QoS TID; powersave=off: off at the start, never on");
		static const u32 want[4] = { 15, 1023, 3, 0 };
		CHECK(fw.wmm[0][0] == want[0] && fw.wmm[0][1] == want[1] && fw.wmm[0][2] == want[2] && fw.wmm[2][0] == 7,
		      "legacy: the defaults with QoS (mac80211: still enable QoS)");
	} else if (wpa3) {
		/* WPA3 and PMF: what the station chose, how SAE went */
		const struct wifi_stats *ws = wifi_stats();
		u8 want_akm = IS("psk256") ? 6 : 8;
		CHECK(ws->akm == want_akm && ap.akm == want_akm && ws->pmf && ap.mfp, "%s: AKM %u, PMF (%u)", scenario, want_akm, ws->akm);
		CHECK(ws->h2e == (IS("h2e") || IS("mixed")) && (!ap.assoc_had_rsnx || ap.assoc_rsnx[2] == 0x20) &&
		      ap.assoc_had_rsnx == (want_akm == 8), "%s: hash-to-element as the AP offers it; our RSNX element with SAE", scenario);
		if (want_akm == 8) {
			CHECK(as.commits == (aps.anticlog ? 2u : 1u) && as.tokens == (aps.anticlog ? 1u : 0u) && as.confirms == 1 &&
			      as.h2e_commits == (ws->h2e ? as.commits : 0), "%s: SAE: %u commits (%u with the token), one confirm",
			      scenario, as.commits, as.tokens);
		} else {
			CHECK(as.commits == 0, "psk256: open authentication, no SAE");
		}
		CHECK(!(fw.peer_flags & WMI_TLV_PEER_PMF), "PEER_ASSOC without the firmware's PMF flag (pmf=on: as ath10k without MFP_SUPPORT)");
	} else {
		CHECK(fw.vdev_mode == MODE_11NG_HT20 && fw.peer_phymode == MODE_11NG_HT20 && !(fw.peer_flags & WMI_TLV_PEER_40MHZ),
		      "uapsd: HT20 (no secondary channel)");
		CHECK(ap.assoc_had_wmm && ap.assoc_wmm[8] == 0x0f, "uapsd: QoS info 0x0f (all ACs, all of the service period)");
		CHECK(fw.ps_param[WMI_STA_PS_PARAM_UAPSD] == 0xff && fw.ps_param[WMI_STA_PS_PARAM_RX_WAKE_POLICY] ==
		      WMI_STA_PS_RX_WAKE_POLICY_POLL_UAPSD && fw.ps_param[WMI_STA_PS_PARAM_TX_WAKE_THRESHOLD] ==
		      WMI_STA_PS_TX_WAKE_THRESHOLD_NEVER && fw.ps_param[WMI_STA_PS_PARAM_PSPOLL_COUNT] == WMI_STA_PS_PSPOLL_COUNT_UAPSD,
		      "uapsd: all ACs delivery and trigger, RX wake by U-APSD poll, TX never wakes, PS-Poll count 3");
		CHECK(fw.tid_eapol == 5, "uapsd: EAPOL downgraded from 7 to 5 (admission control on VO)");
	}
	CHECK(ap.m2 == 1 && ap.m4 == 1 && ap.bad_mic == 0, "the 4-way handshake at the AP: messages 2 and 4, their MICs right");
	CHECK(fw.tk_set && !memcmp(fw.tk, ap.ptk + 32, 16), "the installed pairwise key is the AP's TK");
	CHECK(fw.gtk_set[1] && !memcmp(fw.gtk[1], ap.gtk, 16), "the installed group key 1 is the AP's GTK");
	CHECK(fw.authorized && fw.up, "the peer authorized, the vdev up");
	CHECK(fw.eapol_unencrypted == 2, "messages 2 and 4 in the clear (%u)", fw.eapol_unencrypted);
	CHECK(links_up == 1, "the link up once");

	run_until(now + 20000, leased);
	CHECK(leased(), "a DHCP lease");
	u32 a = ip4_addr_get_u32(netif_ip4_addr(&nif));
	CHECK(a == (100u << 24 | 77 << 16 | 168 << 8 | 192), "192.168.77.100 (%x)", a);
	CHECK(ap.dhcp_discover >= 1 && ap.dhcp_request >= 1 && fw.decrypt_fail == 0 && ap.decrypt_fail == 0,
	      "DISCOVER and REQUEST at the AP (CCMP with the installed key), the broadcast replies decrypted (GTK)");
	run_until(now + 100, NULL);
	if (IS("legacy")) {
		CHECK(fw.ka_cmds == 0 && fw.arp_cmds == 0 && !wifi_offloaded(),
		      "without the services: no keepalive, no ARP offload asked for");
	} else {
		CHECK(fw.ka_cmds == 1 && fw.ka_ok && fw.ka_method == WMI_STA_KEEPALIVE_METHOD_NULL_FRAME && fw.ka_interval == 30,
		      "STA_KEEPALIVE: vdev 0, on, null frames, 30 s, the ARP response's TLV (%u %u %u)", fw.ka_cmds,
		      fw.ka_method, fw.ka_interval);
		CHECK(fw.arp_cmds == 1 && fw.arp_valid && !memcmp(fw.arp_ip, sta_ip, 4) && fw.ns_empty && wifi_offloaded(),
		      "SET_ARP_NS_OFFLOAD: 192.168.77.100 in the first ARP tuple, no NS; offloaded (%u)", fw.arp_cmds);
	}

	ping_out(0);
	run_until(now + 3000, echoed);
	CHECK(ap.echo_replies == 1, "a ping answered (%u; ARP requests answered %u)", ap.echo_replies, ap.arp_requests);
	CHECK(fw.tid_last_ip == (IS("legacy") ? HTT_TID_NON_QOS : 0), "the reply's TID: 0 (best effort), non-QoS without WMM");
	ap.echo_replies = 0;
	ping_out(0xb8);
	run_until(now + 3000, echoed);
	CHECK(ap.echo_replies == 1 && fw.tid_ef == (IS("legacy") ? HTT_TID_NON_QOS : IS("uapsd") ? 5u : 6u),
	      "an EF ping (DSCP 46): TID 6 (voice), 5 under admission control (%u)", fw.tid_ef);
	if (IS("normal")) {
		/* idle: the host looks less often, the radio dozes; a ping then
		 * comes with the next beacon and is answered */
		CHECK(wifi_busy() && wifi_poll_ms() == 20, "frames just came: busy, polled every 20 ms");
		run_until(now + 1500, NULL);
		CHECK(!wifi_busy() && wifi_poll_ms() == 200, "1.5 s without frames: not busy, every 200 ms (%u)", wifi_poll_ms());
		run_until(now + 10000, NULL);
		CHECK(wifi_poll_ms() == 1000 && fw_dozing(), "11.5 s: every second; the radio dozes");
		ap.echo_replies = 0;
		u32 held = fw.tim_held;
		ping_out(0);
		run_until(now + 3000, echoed);
		CHECK(ap.echo_replies == 1 && fw.tim_held > held && fw.tim_delivered >= fw.tim_held - fw.ntim,
		      "a ping to the dozing station: kept by the AP, fetched at the beacon, answered (%u kept)", fw.tim_held - held);
		CHECK(wifi_busy() && wifi_poll_ms() == 20, "busy again after it");
		/* the router asks for us: the firmware answers, the host sees
		 * nothing of it */
		u32 rx = fw.rx_delivered, req = ap.arp_replies, ans = fw.arp_answered;
		run_until(now + 2000, NULL);
		rx = fw.rx_delivered;
		arp_out(1, (const u8 *)"\0\0\0\0\0\0", sta_ip, bcast);
		run_until(now + 300, NULL);
		CHECK(ap.arp_replies == req + 1 && fw.arp_answered == ans + 1 && fw.rx_delivered == rx,
		      "an ARP request for our address answered by the firmware, nothing to the host (%u %u)",
		      fw.arp_answered - ans, fw.rx_delivered - rx);
		/* a minute of silence: the firmware's null frames keep the
		 * association, the host gets nothing */
		u32 nulls = ap.nulls;
		rx = fw.rx_delivered;
		run_until(now + 65000, NULL);
		CHECK(ap.nulls >= nulls + 2 && fw.rx_delivered == rx && is_up(),
		      "65 s of silence: the firmware's keepalive (%u null frames), nothing to the host, still up", ap.nulls - nulls);
	}
	if (wpa3) {
		/* PMF: the AP's SA Query answered (protected, our PN); an
		 * unprotected deauthentication ignored, an SA Query to the AP
		 * answered; a group one with a bad BIP MIC ignored */
		const struct wifi_stats *ws = wifi_stats();
		u32 restarts = ws->restarts;
		ap_sa_query_req();
		run_until(now + 300, NULL);
		CHECK(ap.saq_responses == 1 && ws->saq_answered == 1 && fw.protected_mgmt >= 1 && ap.mgmt_replays == 0,
		      "the AP's SA Query answered, protected (%u)", ap.saq_responses);
		u8 rs[2] = { 7, 0 };
		ap_mgmt(0x00c0, rs, 2);
		run_until(now + 1500, NULL);
		CHECK(is_up() && ws->unprot_dropped == 1 && ap.saq_requests >= 1 && ws->restarts == restarts,
		      "an unprotected deauthentication ignored; our SA Query answered: still up (%u queries)", ap.saq_requests);
		ap_mgmt_bip(0x00c0, rs, 2, false);
		run_until(now + 300, NULL);
		CHECK(is_up() && ws->bip_dropped == 1, "a group deauthentication with a bad BIP MIC ignored");
		if (IS("sae")) {
			/* the SA Query unanswered while the AP keeps the association:
			 * down after a second, then SAE again and the association's
			 * comeback (status 30, 1000 TUs) */
			ap.saq_silent = true;
			ap.comeback_armed = true;
			u32 q = ap.saq_requests;
			ap_mgmt(0x00c0, rs, 2);
			run_until(now + 1500, NULL);
			CHECK(!is_up() && ws->restarts == restarts + 1 && ap.saq_requests >= q + 4,
			      "no answer to our SA Queries (%u tries): down after a second", ap.saq_requests - q);
			ap.saq_silent = false;
			run_until(now + 20000, is_up);
			CHECK(is_up() && ap.comebacks == 1 && as.confirms == 2, "up again: SAE, the association's comeback after 1000 TUs");
			restarts = ws->restarts;
		}
		/* a valid group deauthentication (BIP), then a protected unicast
		 * one: down, and up again each time */
		u32 a0 = ap.assocs;
		ap_mgmt_bip(0x00c0, rs, 2, true);
		run_until(now + 300, NULL);
		CHECK(!is_up() && ws->restarts == restarts + 1, "a group deauthentication with a valid BIP MIC (the IGTK): down");
		run_until(now + 20000, is_up);
		CHECK(is_up() && ap.assocs > a0, "up again");
		u32 m4b = ap.m4;
		ap_mgmt_protected(0x00c0, rs, 2);
		run_until(now + 300, NULL);
		CHECK(!is_up() && ws->restarts == restarts + 2, "a protected deauthentication (CCMP, the PN): down");
		run_until(now + 20000, is_up);
		CHECK(is_up() && ap.m4 == m4b + 1, "up again (4-way handshakes %u)", ap.m4);
		ap.echo_replies = 0;
		ping_out(0);
		run_until(now + 3000, echoed);
		CHECK(ap.echo_replies == 1 && fw.decrypt_fail == 0, "a ping after it all");
	}
	if (!IS("normal")) {
		const struct wifi_stats *s = wifi_stats();
		CHECK(s->tx_done == s->tx_frames && bad_access == 0, "every TX_FRM completed, no access outside the memory");
		printf("%d passed, %d failed (%s)\n", passed, failed, scenario);
		as_free();
		return failed != 0;
	}

	/* the group key handshake: a new GTK (id 2), then an ARP request
	 * with it: answered only if the new key is in */
	{
		u8 m[256], w[160];
		u32 wl = ap_key_data(w, ap.gtk2, 2, false);
		eapol_hdr(m, 0x1380 | ap_ver(), ++ap.replay, NULL, wl);   /* encr, secure, MIC, ack, group */
		memcpy(m + 99, w, wl);
		ap_mic(ap.ptk, m, 99 + wl, m + 81);
		ap.rekey_sent = true;
		ap_data(ap.sta, ap_mac, 0x888e, m, 99 + wl);
		run_until(now + 2000, NULL);
		CHECK(ap.group2 == 1 && fw.gtk_set[2] && !memcmp(fw.gtk[2], ap.gtk2, 16), "the group key handshake: GTK 2 installed");
		arp_out(1, (const u8 *)"\0\0\0\0\0\0", sta_ip, bcast);
		run_until(now + 2000, rekeyed);
		CHECK(ap.arp_after_rekey, "a broadcast with the new group key answered");
	}

	/* deauthenticated: down, the vdev stopped, joined again */
	{
		u8 b[2] = { 3, 0 };
		ap_mgmt(0x00c0, b, 2);
		ap.keys = false;
		ap.msg = 0;
		run_until(now + 200, NULL);
		CHECK(links_down == 1 && wifi_state() != WIFI_UP && !fw.vdev_started && !fw.peer, "down after the deauthentication");
		run_until(now + 20000, is_up);
		CHECK(is_up() && links_up == 2 && ap.m4 == 2 && wifi_stats()->restarts == 1, "up again (%s, %d)",
		      wifi_state_name(wifi_state()), links_up);
		run_until(now + 100, NULL);
		CHECK(fw.nps_modes == 5 && fw.ps_modes[2] == WMI_STA_PS_MODE_DISABLED && fw.ps_modes[3] == WMI_STA_PS_MODE_DISABLED &&
		      fw.ps_modes[4] == WMI_STA_PS_MODE_ENABLED, "power save: off when disconnected, off at the new start, on again (%u)",
		      fw.nps_modes);
		CHECK(fw.ka_cmds == 2 && fw.arp_cmds >= 2 && fw.arp_valid && wifi_offloaded(),
		      "the keepalive and the ARP offload again for the new association (%u %u)", fw.ka_cmds, fw.arp_cmds);
	}

	const struct wifi_stats *s = wifi_stats();
	CHECK(s->tx_done == s->tx_frames, "every TX_FRM completed (%u of %u)", s->tx_done, s->tx_frames);
	CHECK(fw.rx_no_buffer == 0, "the RX ring never empty");
	CHECK(bad_access == 0, "no access outside the memory (%u)", bad_access);
	printf("wifi: %u WMI commands, %u events, %u HTT messages, %u frames out, %u in, %u dropped\n", s->wmi_cmds,
	       s->wmi_events, s->htt_msgs, s->tx_frames, s->rx_frames, s->rx_dropped);
	printf("%d passed, %d failed\n", passed, failed);
	return failed != 0;
}
