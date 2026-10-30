/* KolibriOS for ARM: the WPA2 cryptography (kernel/modem/crypto.c,
 * wpa.c) against published test vectors and OpenSSL (make -C tests/wifi):
 * SHA-1 (FIPS 180-2 "abc"), HMAC-SHA1 (RFC 2202 case 1), PBKDF2 (RFC
 * 6070), the PMK (IEEE 802.11-2016 J.4.2: "password"/"IEEE",
 * "ThisIsAPassword"/"ThisIsASSID"), the PRF (IEEE 802.11-2016 J.3.2 test
 * case 1, PRF-512), AES-128 (FIPS 197 C.1), the key wrap (RFC 3394 4.1),
 * and random cases against OpenSSL's HMAC, PBKDF2, AES and key wrap. */
#include <stdio.h>
#include <stdlib.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include "../../kernel/modem/crypto.h"
#include "../../kernel/modem/wpa.h"

int crypto_failed, crypto_passed;
#define CHECK(c, ...) do { if (c) crypto_passed++; else { crypto_failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void hex(const char *h, u8 *o, u32 n)
{
	for (u32 i = 0; i < n; i++) {
		unsigned v;
		sscanf(h + 2 * i, "%2x", &v);
		o[i] = (u8)v;
	}
}

static bool eq(const u8 *a, const char *h, u32 n)
{
	u8 b[128];
	hex(h, b, n);
	return !memcmp(a, b, n);
}

void crypto_tests(void)
{
	u8 o[128], k[64];
	sha1("abc", 3, o);
	CHECK(eq(o, "a9993e364706816aba3e25717850c26c9cd0d89d", 20), "SHA-1 of abc");
	u8 m[1000];
	memset(m, 'a', sizeof(m));
	struct sha1 s;
	sha1_init(&s);
	for (int i = 0; i < 1000; i++) sha1_update(&s, m, 1000);
	sha1_final(&s, o);
	CHECK(eq(o, "34aa973cd4c4daa4f61eeb2bdbad27316534016f", 20), "SHA-1 of a million a");
	memset(k, 0x0b, 20);
	hmac_sha1(k, 20, (const u8 *)"Hi There", 8, o);
	CHECK(eq(o, "b617318655057264e28bc0b6fb378c8ef146be00", 20), "HMAC-SHA1, RFC 2202 1");
	pbkdf2_sha1("password", (const u8 *)"salt", 4, 1, o, 20);
	CHECK(eq(o, "0c60c80f961f0e71f3a9b524af6012062fe037a6", 20), "PBKDF2, RFC 6070 c=1");
	pbkdf2_sha1("password", (const u8 *)"salt", 4, 4096, o, 20);
	CHECK(eq(o, "4b007901b765489abead49d926f721d065a429c1", 20), "PBKDF2, RFC 6070 c=4096");
	u8 pmk[32];
	CHECK(wpa_pmk("password", (const u8 *)"IEEE", 4, pmk) == 0 &&
	      eq(pmk, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", 32), "the PMK, J.4.2 1");
	CHECK(wpa_pmk("ThisIsAPassword", (const u8 *)"ThisIsASSID", 11, pmk) == 0 &&
	      eq(pmk, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af", 32), "the PMK, J.4.2 2");
	CHECK(wpa_pmk("0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af", (const u8 *)"x", 1, o) == 0 &&
	      !memcmp(o, pmk, 32), "a PSK of 64 hex digits taken as it is");
	CHECK(wpa_pmk("short", (const u8 *)"x", 1, o) == -1, "a passphrase of 5 refused");
	sha1_prf(k, 20, "prefix", (const u8 *)"Hi There", 8, o, 64);
	CHECK(eq(o, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
		    "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a", 64), "PRF-512, J.3.2 1");
	u8 key[16], pt[16];
	hex("000102030405060708090a0b0c0d0e0f", key, 16);
	hex("00112233445566778899aabbccddeeff", pt, 16);
	struct aes128 a;
	aes128_init(&a, key);
	aes128_encrypt(&a, pt, o);
	CHECK(eq(o, "69c4e0d86a7b0430d8cdb78070b4c55a", 16), "AES-128, FIPS 197 C.1");
	aes128_decrypt(&a, o, o + 16);
	CHECK(!memcmp(o + 16, pt, 16), "AES-128 decrypts it back");
	aes_wrap(key, 2, pt, o);
	CHECK(eq(o, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", 24), "the key wrap, RFC 3394 4.1");
	CHECK(aes_unwrap(key, 2, o, o + 32) == 0 && !memcmp(o + 32, pt, 16), "the key unwrap");
	o[3] ^= 1;
	CHECK(aes_unwrap(key, 2, o, o + 32) == -1, "a broken wrap refused");

	/* random cases against OpenSSL */
	int bad = 0;
	for (int t = 0; t < 200; t++) {
		u8 kk[80], d[300], x[64], y[64];
		u32 kl = 1 + rand() % 80, dl = rand() % 300;
		for (u32 i = 0; i < kl; i++) kk[i] = rand();
		for (u32 i = 0; i < dl; i++) d[i] = rand();
		hmac_sha1(kk, kl, d, dl, x);
		unsigned yl = 20;
		HMAC(EVP_sha1(), kk, kl, d, dl, y, &yl);
		if (memcmp(x, y, 20)) bad++;
		aes128_init(&a, kk);
		aes128_encrypt(&a, d, x);
		EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
		int ol;
		EVP_EncryptInit_ex(c, EVP_aes_128_ecb(), NULL, kk, NULL);
		EVP_CIPHER_CTX_set_padding(c, 0);
		EVP_EncryptUpdate(c, y, &ol, d, 16);
		EVP_CIPHER_CTX_free(c);
		if (memcmp(x, y, 16)) bad++;
		/* the key wrap of 2..6 blocks */
		u32 nb = 2 + rand() % 5;
		aes_wrap(kk, nb, d, x);
		c = EVP_CIPHER_CTX_new();
		EVP_EncryptInit_ex(c, EVP_aes_128_wrap(), NULL, kk, NULL);
		EVP_EncryptUpdate(c, y, &ol, d, 8 * nb);
		EVP_CIPHER_CTX_free(c);
		if (ol != (int)(8 * nb + 8) || memcmp(x, y, 8 * nb + 8)) bad++;
	}
	CHECK(bad == 0, "HMAC-SHA1, AES-128 and the key wrap as OpenSSL's, 200 random cases (%d differ)", bad);
	char pass[64];
	for (int i = 0; i < 20; i++) pass[i] = 'a' + rand() % 26;
	pass[20] = 0;
	PKCS5_PBKDF2_HMAC_SHA1(pass, 20, (const u8 *)"KolibriNet", 10, 4096, 32, o);
	CHECK(wpa_pmk(pass, (const u8 *)"KolibriNet", 10, pmk) == 0 && !memcmp(o, pmk, 32), "the PMK as OpenSSL's PBKDF2");

	/* the RSN elements */
	u8 ie[64];
	const char *why;
	u32 il = wpa_own_ie(ie);
	CHECK(il == 22 && wpa_check_ap_ie(ie, il, &why) == 0, "our RSN element is one we take");
	u8 tkip[64];
	memcpy(tkip, ie, il);
	tkip[7] = 2;
	CHECK(wpa_check_ap_ie(tkip, il, &why) == -1, "a TKIP group cipher refused: %s", why);
	memcpy(tkip, ie, il);
	tkip[19] = 8;
	CHECK(wpa_check_ap_ie(tkip, il, &why) == -1, "SAE only refused: %s", why);
	memcpy(tkip, ie, il);
	tkip[20] = 0xc0;
	CHECK(wpa_check_ap_ie(tkip, il, &why) == -1, "MFP required refused: %s", why);
	memcpy(tkip, ie, il);
	tkip[20] = 0x80;
	CHECK(wpa_check_ap_ie(tkip, il, &why) == 0, "MFP capable (optional) taken");
}
