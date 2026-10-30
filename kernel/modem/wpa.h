/* KolibriOS for ARM: the supplicant's side of WPA2-PSK (wpa.c;
 * docs/pro1x-network.md, stage 5), after wpa_supplicant's
 * src/rsn_supp/wpa.c and src/common/wpa_common.c: the PMK from the
 * passphrase, the 4-way handshake (EAPOL-Key messages 1-4: the PTK from
 * the PMK, both addresses and both nonces; message 3's GTK unwrapped with
 * the KEK), the group key handshake (a new GTK). CCMP only (pairwise and
 * group), AKM PSK (00-0f-ac:2), key descriptor version 2 (HMAC-SHA1-128
 * MICs, the AES key wrap). The keys go to the firmware (WMI), which
 * encrypts the frames. */
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
#define WPA_KEY_INFO_TYPE_HMAC_SHA1_AES 2
#define WPA_KEY_INFO_KEY_TYPE (1 << 3)  /* pairwise */
#define WPA_KEY_INFO_INSTALL (1 << 6)
#define WPA_KEY_INFO_ACK (1 << 7)
#define WPA_KEY_INFO_MIC (1 << 8)
#define WPA_KEY_INFO_SECURE (1 << 9)
#define WPA_KEY_INFO_ERROR (1 << 10)
#define WPA_KEY_INFO_REQUEST (1 << 11)
#define WPA_KEY_INFO_ENCR_KEY_DATA (1 << 12)

/* what the caller does after wpa_rx: send *out_len bytes (if any), then
 * install the keys */
#define WPA_DO_PTK 1                    /* the pairwise key (after message 4 is out) */
#define WPA_DO_GTK 2                    /* the group key */
#define WPA_DO_DONE 4                   /* the port open: the handshake complete */

struct wpa_sm {
	u8 pmk[WPA_PMK_LEN];
	u8 own[6], aa[6];
	u8 own_ie[WPA_MAX_IE], ap_ie[WPA_MAX_IE];   /* RSN IEs: ours (the association's), the AP's (its beacon) */
	u8 own_ie_len, ap_ie_len;
	u8 anonce[WPA_NONCE_LEN], snonce[WPA_NONCE_LEN];
	bool snonce_set, tptk_set, ptk_set;
	u8 tptk[WPA_PTK_LEN], ptk[WPA_PTK_LEN];
	u8 replay[8];
	bool replay_set;
	u8 gtk[32], rsc[8];
	u32 gtk_len;
	int gtk_idx;
	bool done;
	const char *why;                    /* the last frame dropped: why */
	void (*random)(void *p, u32 n);
};

/* the PMK: a passphrase of 8-63 characters through PBKDF2 with the SSID,
 * or 64 hex digits as they are: 0, -1 */
int wpa_pmk(const char *psk, const u8 *ssid, u32 ssid_len, u8 pmk[WPA_PMK_LEN]);
/* our RSN IE: CCMP for both, PSK, no capabilities (out: 22 bytes) */
u32 wpa_own_ie(u8 *out);
/* an AP's RSN IE checked for what this supports: 0, or -1 with *why */
int wpa_check_ap_ie(const u8 *ie, u32 len, const char **why);

void wpa_init(struct wpa_sm *s, const u8 pmk[WPA_PMK_LEN], const u8 own[6], const u8 aa[6], const u8 *own_ie,
	      u32 own_ie_len, const u8 *ap_ie, u32 ap_ie_len, void (*random)(void *p, u32 n));
/* an EAPOL frame from the AP (from the 802.1X header on): the reply into
 * out (cap bytes, *out_len 0 if none), WPA_DO_* in the result; -1 when it
 * was dropped (s->why) */
int wpa_rx(struct wpa_sm *s, const u8 *p, u32 len, u8 *out, u32 cap, u32 *out_len);
const u8 *wpa_tk(const struct wpa_sm *s);

/* the PTK (wpa_pmk_to_ptk: PRF-384 over both addresses and both nonces,
 * the smaller first), the MIC (HMAC-SHA1-128 over the frame with its MIC
 * zeroed); tests/wifi checks them against Python's and OpenSSL's */
void wpa_ptk(const u8 pmk[WPA_PMK_LEN], const u8 aa[6], const u8 spa[6], const u8 *anonce, const u8 *snonce,
	     u8 ptk[WPA_PTK_LEN]);
void wpa_mic(const u8 kck[16], const u8 *frame, u32 len, u8 mic[16]);

#endif
