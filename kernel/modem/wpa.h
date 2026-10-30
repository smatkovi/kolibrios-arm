/* KolibriOS for ARM: the supplicant's side of WPA2 and WPA3 personal
 * (wpa.c; docs/pro1x-network.md, stage 5 and WPA3), after wpa_supplicant
 * 2.11: src/rsn_supp/wpa.c (the 4-way and group key handshakes, the IGTK),
 * src/rsn_supp/wpa_ie.c (our RSN and RSNX elements), src/common/wpa_common.c
 * (wpa_parse_wpa_ie_rsn, wpa_pmk_to_ptk, wpa_eapol_key_mic) and
 * wpa_supplicant/wpa_supplicant.c (wpa_supplicant_set_suites: the choice).
 *
 * The network as a profile "key_mgmt=SAE WPA-PSK-SHA256 WPA-PSK,
 * ieee80211w=1, sae_pwe=2" would have it: the best AKM the AP offers (SAE
 * 00-0f-ac:8, then PSK-SHA256 :6, then PSK :2), management frame protection
 * whenever the AP is capable (BIP-CMAC-128, the IGTK from the handshakes),
 * SAE's PWE by hash-to-element when the AP has it in its RSNX element,
 * else hunting and pecking. CCMP-128 only (pairwise and group). The EAPOL-
 * Key descriptors: version 2 (HMAC-SHA1-128, the SHA-1 PRF) for PSK, 3
 * (AES-128-CMAC, the SHA-256 KDF) for PSK-SHA256, 0 ("AKM-defined": AES-
 * 128-CMAC, the SHA-256 KDF) for SAE. The keys go to the firmware (WMI),
 * which encrypts the data frames; the IGTK stays here (ath10k: BIP in
 * software). */
#ifndef WPA_H
#define WPA_H
#include "modem.h"

#define WPA_PMK_LEN 32
#define WPA_NONCE_LEN 32
#define WPA_PTK_LEN 48                  /* KCK 16, KEK 16, TK 16 (CCMP) */
#define WPA_TK_LEN 16
#define WPA_MAX_IE 64
#define WPA_EAPOL_HDR 99                /* 802.1X header 4, the key descriptor 95 */
#define ETH_P_EAPOL 0x888e

/* key info (wpa_common.h) */
#define WPA_KEY_INFO_TYPE_AKM_DEFINED 0
#define WPA_KEY_INFO_TYPE_HMAC_SHA1_AES 2
#define WPA_KEY_INFO_TYPE_AES_128_CMAC 3
#define WPA_KEY_INFO_KEY_TYPE (1 << 3)  /* pairwise */
#define WPA_KEY_INFO_INSTALL (1 << 6)
#define WPA_KEY_INFO_ACK (1 << 7)
#define WPA_KEY_INFO_MIC (1 << 8)
#define WPA_KEY_INFO_SECURE (1 << 9)
#define WPA_KEY_INFO_ERROR (1 << 10)
#define WPA_KEY_INFO_REQUEST (1 << 11)
#define WPA_KEY_INFO_ENCR_KEY_DATA (1 << 12)

/* the AKMs (00-0f-ac:N) and the RSN capabilities */
#define WPA_AKM_PSK 2
#define WPA_AKM_PSK_SHA256 6
#define WPA_AKM_SAE 8
#define WPA_CAP_MFPR (1 << 6)
#define WPA_CAP_MFPC (1 << 7)
#define WPA_RSNX_SAE_H2E (1 << 5)       /* WLAN_RSNX_CAPAB_SAE_H2E */

/* what the AP's RSN element offers (wpa_parse_wpa_ie_rsn, as far as used) */
struct wpa_ap {
	bool group_ccmp;
	bool pairwise_ccmp;
	u32 akms;                           /* 1 << N for each 00-0f-ac:N */
	u16 caps;
	bool bip;                           /* the group management cipher BIP-CMAC-128 (its default) */
	bool h2e;                           /* the RSNX element's SAE hash-to-element */
};
/* the choice for an AP */
struct wpa_choice {
	u8 akm;                             /* WPA_AKM_* */
	bool mfp;                           /* management frame protection (MFPC both, BIP) */
	bool h2e;                           /* SAE: hash-to-element */
};
int wpa_parse_ap(const u8 *rsn, u32 rsn_len, const u8 *rsnx, u32 rsnx_len, struct wpa_ap *o, const char **why);
/* wpa_supplicant_set_suites for our profile: 0, or -1 with *why (an AP
 * of SAE hash-to-element only without H2E in its RSNX element: refused) */
int wpa_choose(const struct wpa_ap *ap, bool h2e_only, struct wpa_choice *c, const char **why);
const char *wpa_akm_name(u8 akm);
/* our RSN element (wpa_gen_wpa_ie_rsn: CCMP, the AKM, MFPC, PMKID count
 * 0 and BIP-CMAC-128 with PMF) and RSNX element (wpa_gen_rsnxe: SAE with
 * sae_pwe 2: H2E): their lengths */
u32 wpa_own_ie(u8 *out, const struct wpa_choice *c);
u32 wpa_own_rsnx(u8 *out, const struct wpa_choice *c);

/* what the caller does after wpa_rx: send *out_len bytes (if any), then
 * install the keys */
#define WPA_DO_PTK 1                    /* the pairwise key (after message 4 is out) */
#define WPA_DO_GTK 2                    /* the group key */
#define WPA_DO_DONE 4                   /* the port open: the handshake complete */
#define WPA_DO_IGTK 8                   /* a new IGTK (kept here: wpa_igtk) */

struct wpa_sm {
	u8 pmk[WPA_PMK_LEN];
	u8 akm;
	bool mfp;
	u8 own[6], aa[6];
	u8 own_ie[WPA_MAX_IE], ap_ie[WPA_MAX_IE];   /* RSN IEs: ours (the association's), the AP's (its beacon) */
	u8 own_ie_len, ap_ie_len;
	u8 ap_rsnx[16], ap_rsnx_len;        /* the AP's RSNX element (its beacon) */
	u8 anonce[WPA_NONCE_LEN], snonce[WPA_NONCE_LEN];
	bool snonce_set, tptk_set, ptk_set;
	u8 tptk[WPA_PTK_LEN], ptk[WPA_PTK_LEN];
	u8 replay[8];
	bool replay_set;
	u8 gtk[32], rsc[8];
	u32 gtk_len;
	int gtk_idx;
	u8 igtk[16], ipn[6];                /* BIP's key, its packet number (the next one must be above) */
	int igtk_idx;                       /* 4 or 5; -1 none */
	bool done;
	const char *why;                    /* the last frame dropped: why */
	void (*random)(void *p, u32 n);
};

/* the PMK: a passphrase of 8-63 characters through PBKDF2 with the SSID,
 * or 64 hex digits as they are: 0, -1 */
int wpa_pmk(const char *psk, const u8 *ssid, u32 ssid_len, u8 pmk[WPA_PMK_LEN]);

/* pmk: PBKDF2's or SAE's; the elements: our RSN element (the
 * association's), the AP's RSN and RSNX elements (its beacon) */
void wpa_init(struct wpa_sm *s, const u8 pmk[WPA_PMK_LEN], const struct wpa_choice *c, const u8 own[6], const u8 aa[6],
	      const u8 *own_ie, u32 own_ie_len, const u8 *ap_ie, u32 ap_ie_len, const u8 *ap_rsnx, u32 ap_rsnx_len,
	      void (*random)(void *p, u32 n));
/* an EAPOL frame from the AP (from the 802.1X header on): the reply into
 * out (cap bytes, *out_len 0 if none), WPA_DO_* in the result; -1 when it
 * was dropped (s->why) */
int wpa_rx(struct wpa_sm *s, const u8 *p, u32 len, u8 *out, u32 cap, u32 *out_len);
const u8 *wpa_tk(const struct wpa_sm *s);
/* BIP-CMAC-128 (IEEE 802.11-2020 12.5.4; mac80211's
 * ieee80211_crypto_aes_cmac_decrypt, bip_aad): a group-addressed robust
 * management frame (the whole frame, the MMIE last: key id, IPN, MIC 8)
 * checked with the IGTK, its IPN above the last one: 0 (the IPN taken),
 * or -1 */
int wpa_bip_check(struct wpa_sm *s, const u8 *f, u32 len);

/* the PTK (wpa_pmk_to_ptk: the SHA-1 PRF-384 for PSK, the SHA-256 KDF-384
 * for PSK-SHA256 and SAE; over both addresses and both nonces, the
 * smaller first), the MIC (HMAC-SHA1-128 for version 2, AES-128-CMAC for
 * 3 and SAE's 0, over the frame with its MIC zeroed); tests/wifi checks
 * them against OpenSSL */
void wpa_ptk(const u8 pmk[WPA_PMK_LEN], u8 akm, const u8 aa[6], const u8 spa[6], const u8 *anonce, const u8 *snonce,
	     u8 ptk[WPA_PTK_LEN]);
void wpa_mic(const u8 kck[16], u8 akm, const u8 *frame, u32 len, u8 mic[16]);

#endif
