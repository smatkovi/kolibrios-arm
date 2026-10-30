/* KolibriOS for ARM: the cryptography of WPA2-PSK (crypto.c;
 * docs/pro1x-network.md, stage 5), as wpa_supplicant has it in
 * src/crypto: SHA-1 (FIPS 180-4), HMAC-SHA1 (RFC 2104), PBKDF2-SHA1
 * (RFC 8018: the PMK from the passphrase, sha1-pbkdf2.c), the PRF of IEEE
 * 802.11 (sha1-prf.c: the PTK), AES-128 (FIPS 197) and its key wrap
 * (RFC 3394: aes-wrap.c, aes-unwrap.c: the GTK in message 3). The
 * encryption of the frames themselves (CCMP) is the WLAN firmware's. */
#ifndef CRYPTO_H
#define CRYPTO_H
#include "modem.h"

#define SHA1_LEN 20

struct sha1 {
	u32 h[5];
	u64 n;                          /* bytes so far */
	u8 buf[64];
	u32 used;
};
void sha1_init(struct sha1 *s);
void sha1_update(struct sha1 *s, const void *p, u32 len);
void sha1_final(struct sha1 *s, u8 out[SHA1_LEN]);
void sha1(const void *p, u32 len, u8 out[SHA1_LEN]);

/* HMAC-SHA1 over n pieces (as wpa_supplicant's hmac_sha1_vector) */
void hmac_sha1_vec(const u8 *key, u32 key_len, int n, const u8 *const p[], const u32 len[], u8 out[SHA1_LEN]);
void hmac_sha1(const u8 *key, u32 key_len, const u8 *p, u32 len, u8 out[SHA1_LEN]);

/* PBKDF2-SHA1: out_len bytes (the PMK: 32, 4096 iterations, the SSID as
 * the salt) */
void pbkdf2_sha1(const char *pass, const u8 *salt, u32 salt_len, u32 iterations, u8 *out, u32 out_len);

/* IEEE 802.11's PRF (sha1_prf): HMAC-SHA1(key, label || 0 || data || i) */
void sha1_prf(const u8 *key, u32 key_len, const char *label, const u8 *data, u32 data_len, u8 *out, u32 out_len);

/* AES-128 */
struct aes128 { u32 rk[44]; };
void aes128_init(struct aes128 *a, const u8 key[16]);
void aes128_encrypt(const struct aes128 *a, const u8 in[16], u8 out[16]);
void aes128_decrypt(const struct aes128 *a, const u8 in[16], u8 out[16]);

/* RFC 3394: n 64-bit blocks of plain text (cipher: n + 1); unwrap 0, or
 * -1 when the integrity check fails */
void aes_wrap(const u8 kek[16], u32 n, const u8 *plain, u8 *cipher);
int aes_unwrap(const u8 kek[16], u32 n, const u8 *cipher, u8 *plain);

/* constant time comparison: 0 when equal */
int crypto_memcmp(const void *a, const void *b, u32 n);
/* the key material gone */
void crypto_wipe(void *p, u32 n);

#endif
