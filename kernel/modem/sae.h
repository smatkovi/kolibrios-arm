/* KolibriOS for ARM: SAE, the authentication of WPA3-Personal (sae.c;
 * docs/pro1x-network.md, WPA3), for group 19 (P-256, ecc.c). After
 * hostapd/wpa_supplicant 2.11 (BSD licence; Jouni Malinen and
 * contributors): src/common/sae.c (sae_derive_pwe_ecc,
 * sae_test_pwd_seed_ecc, sswu, sae_derive_pt_ecc,
 * sae_derive_pwe_from_pt_ecc, sae_derive_commit, sae_derive_k_ecc,
 * sae_derive_keys, sae_write_commit, sae_parse_commit, sae_cn_confirm,
 * sae_write_confirm, sae_check_confirm) and src/common/dragonfly.c (the
 * blinded quadratic residue test, the random scalars); IEEE 802.11-2020
 * 12.4 and RFC 7664 (Dragonfly). Both ways to the password element:
 * hunting and pecking (IEEE 802.11-2020 12.4.4.2.2) and hash-to-element
 * (12.4.4.2.3: the simplified SWU map, z = -10), without a password
 * identifier. */
#ifndef SAE_H
#define SAE_H
#include "modem.h"
#include "ecc.h"

#define SAE_GROUP 19
#define SAE_KCK_LEN 32
#define SAE_PMK_LEN 32
#define SAE_PMKID_LEN 16
#define SAE_COMMIT_LEN (2 + 32 + 64)         /* group, scalar, element */
#define SAE_CONFIRM_LEN (2 + 32)             /* send-confirm, confirm */
/* the authentication frame's status codes (IEEE 802.11-2020 Table 9-50) */
#define SAE_STATUS_ANTI_CLOGGING 76
#define SAE_STATUS_GROUP_UNSUPPORTED 77
#define SAE_STATUS_H2E 126
#define SAE_SILENT 0xffff                    /* a reflection: dropped silently */

struct sae {
	u8 own[6], peer[6];
	bool h2e;
	struct ecp pwe;
	u8 rand[32], scalar[32], elem[64];   /* ours */
	u8 peer_scalar[32], peer_elem[64];
	u8 kck[SAE_KCK_LEN], pmk[SAE_PMK_LEN], pmkid[SAE_PMKID_LEN];
	u16 send_confirm;
	u8 token[256];                       /* the AP's anti-clogging token */
	u32 token_len;
	void (*random)(void *p, u32 n);
};

/* hunting and pecking: the PWE from the password and both addresses
 * (40 rounds at least, the test blinded): 0, or -1 */
int sae_pwe_hnp(struct sae *s, const u8 *pw, u32 pw_len);
/* hash-to-element: PT from the SSID and the password (sae_derive_pt_ecc;
 * a password identifier only for the test vectors: id NULL), then the PWE
 * from PT and both addresses */
int sae_pt(u8 pt[64], const u8 *ssid, u32 ssid_len, const u8 *pw, u32 pw_len, const char *id);
int sae_pwe_from_pt(struct sae *s, const u8 pt[64]);
/* the commit's scalar and element from new rand and mask (or the given
 * ones: the test vectors): 0, or -1 */
int sae_commit(struct sae *s, const u8 *rand, const u8 *mask);
/* the commit's body after the authentication's status: group, the token
 * (hunting and pecking: as it is; hash-to-element: in its container),
 * scalar, element */
u32 sae_write_commit(const struct sae *s, u8 *o);
/* the AP's commit (its body): 0, a status code to answer with, or
 * SAE_SILENT; then sae_process for the keys */
u16 sae_parse_commit(struct sae *s, const u8 *b, u32 len);
int sae_process(struct sae *s);
u32 sae_write_confirm(struct sae *s, u8 *o);
int sae_check_confirm(const struct sae *s, const u8 *b, u32 len);
/* the AP's anti-clogging request (the body after the status): the token
 * kept for the next commit: 0, or -1 */
int sae_take_token(struct sae *s, const u8 *b, u32 len);

#endif
