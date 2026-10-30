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
#include "../../kernel/modem/ecc.h"
#include "../../kernel/modem/sae.h"
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/core_names.h>

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

static void test_random(void *p, u32 n) { for (u32 i = 0; i < n; i++) ((u8 *)p)[i] = (u8)rand(); }

static bool eq(const u8 *a, const char *h, u32 n)
{
	u8 b[256];
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

	/* --- WPA3: SHA-256, HMAC-SHA256, HKDF, the 802.11 KDF, AES-CMAC ---- */
	sha256("abc", 3, o);
	CHECK(eq(o, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32), "SHA-256 of abc (FIPS 180-2)");
	sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, o);
	CHECK(eq(o, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", 32), "SHA-256 of the 448-bit message");
	memset(k, 0x0b, 20);
	hmac_sha256(k, 20, (const u8 *)"Hi There", 8, o);
	CHECK(eq(o, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32), "HMAC-SHA256, RFC 4231 1");
	hmac_sha256((const u8 *)"Jefe", 4, (const u8 *)"what do ya want for nothing?", 28, o);
	CHECK(eq(o, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32), "HMAC-SHA256, RFC 4231 2");
	{
		/* RFC 5869 A.1: PRK = HMAC(salt, IKM), OKM = HKDF-Expand(PRK, info, 42) */
		u8 ikm[22], salt[13], info[10], prk[32];
		memset(ikm, 0x0b, 22);
		for (int i = 0; i < 13; i++) salt[i] = (u8)i;
		for (int i = 0; i < 10; i++) info[i] = (u8)(0xf0 + i);
		hmac_sha256(salt, 13, ikm, 22, prk);
		CHECK(eq(prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", 32), "HKDF-Extract, RFC 5869 A.1");
		hkdf_expand_sha256(prk, 32, info, 10, o, 42);
		CHECK(eq(o, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", 42),
		      "HKDF-Expand, RFC 5869 A.1");
	}
	{
		/* RFC 4493 4: AES-CMAC with 2b7e1516..., 0, 16, 40 and 64 octets */
		u8 ck[16], msg[64];
		hex("2b7e151628aed2a6abf7158809cf4f3c", ck, 16);
		hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52ef"
		    "f69f2445df4f9b17ad2b417be66c3710", msg, 64);
		aes_cmac(ck, msg, 0, o);
		CHECK(eq(o, "bb1d6929e95937287fa37d129b756746", 16), "AES-CMAC, RFC 4493 example 1 (empty)");
		aes_cmac(ck, msg, 16, o);
		CHECK(eq(o, "070a16b46b4d4144f79bdd9dd04a287c", 16), "AES-CMAC, example 2 (16)");
		aes_cmac(ck, msg, 40, o);
		CHECK(eq(o, "dfa66747de9ae63030ca32611497c827", 16), "AES-CMAC, example 3 (40)");
		aes_cmac(ck, msg, 64, o);
		CHECK(eq(o, "51f0bebf7e3b9d92fc49741779363cfe", 16), "AES-CMAC, example 4 (64)");
	}
	bad = 0;
	for (int t = 0; t < 200; t++) {
		u8 kk[80], d[300], x[80], y[80];
		u32 kl = 1 + rand() % 80, dl = rand() % 300;
		for (u32 i = 0; i < kl; i++) kk[i] = rand();
		for (u32 i = 0; i < dl; i++) d[i] = rand();
		hmac_sha256(kk, kl, d, dl, x);
		unsigned yl = 32;
		HMAC(EVP_sha256(), kk, kl, d, dl, y, &yl);
		if (memcmp(x, y, 32)) bad++;
		/* the KDF: HMAC(key, i || label || data || bits) by OpenSSL */
		u32 bits = 8 * (1 + rand() % 70);
		sha256_kdf_bits(kk, kl, "Pairwise key expansion", d, dl, x, bits);
		for (u32 pos = 0, i = 1; pos < bits / 8; i++) {
			u8 buf[400], h[32];
			buf[0] = (u8)i, buf[1] = 0;
			memcpy(buf + 2, "Pairwise key expansion", 22);
			memcpy(buf + 24, d, dl);
			buf[24 + dl] = (u8)bits, buf[25 + dl] = (u8)(bits >> 8);
			HMAC(EVP_sha256(), kk, kl, buf, 26 + dl, h, &yl);
			u32 n = bits / 8 - pos < 32 ? bits / 8 - pos : 32;
			memcpy(y + pos, h, n);
			pos += n;
		}
		if (memcmp(x, y, bits / 8)) bad++;
		/* CMAC by OpenSSL (EVP_MAC) */
		aes_cmac(kk, d, dl, x);
		size_t ml = 16;
		EVP_MAC *mac = EVP_MAC_fetch(NULL, "CMAC", NULL);
		EVP_MAC_CTX *mc = EVP_MAC_CTX_new(mac);
		OSSL_PARAM prm[2] = { OSSL_PARAM_construct_utf8_string("cipher", "AES-128-CBC", 0), OSSL_PARAM_construct_end() };
		EVP_MAC_init(mc, kk, 16, prm);
		EVP_MAC_update(mc, d, dl);
		EVP_MAC_final(mc, y, &ml, 16);
		EVP_MAC_CTX_free(mc);
		EVP_MAC_free(mac);
		if (memcmp(x, y, 16)) bad++;
	}
	CHECK(bad == 0, "HMAC-SHA256, the SHA-256 KDF and AES-CMAC as OpenSSL's, 200 random cases (%d differ)", bad);

	/* --- P-256 against OpenSSL's prime256v1 -------------------------------- */
	{
		EC_GROUP *g = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
		BN_CTX *ctx = BN_CTX_new();
		BIGNUM *bp = BN_new(), *ba = BN_new(), *bb = BN_new(), *bn = BN_new(), *bx = BN_new(), *by = BN_new(), *bk = BN_new();
		EC_GROUP_get_curve(g, bp, ba, bb, ctx);
		EC_GROUP_get_order(g, bn, ctx);
		EC_POINT_get_affine_coordinates(g, EC_GROUP_get0_generator(g), bx, by, ctx);
		u8 t1[32], t2[32], t3[32], t4[32], t5[32];
		BN_bn2binpad(bp, t1, 32), BN_bn2binpad(bb, t2, 32), BN_bn2binpad(bn, t3, 32);
		BN_bn2binpad(bx, t4, 32), BN_bn2binpad(by, t5, 32);
		CHECK(!memcmp(t1, p256_p, 32) && !memcmp(t2, p256_b, 32) && !memcmp(t3, p256_n, 32) && !memcmp(t4, p256_gx, 32) &&
		      !memcmp(t5, p256_gy, 32), "P-256: p, b, n and G are OpenSSL's prime256v1");
		EC_POINT *R = EC_POINT_new(g);
		struct ecp gg, rr;
		ecp_gen(&gg);
		bad = 0;
		for (int t = 0; t < 100; t++) {
			u8 kb[32], rx[32], ry[32], ox[32], oy[32];
			for (int j = 0; j < 32; j++) kb[j] = rand();
			BN_bin2bn(kb, 32, bk);
			EC_POINT_mul(g, R, bk, NULL, NULL, ctx);
			ecp_mul(&rr, &gg, kb);
			EC_POINT_get_affine_coordinates(g, R, bx, by, ctx);
			BN_bn2binpad(bx, ox, 32), BN_bn2binpad(by, oy, 32);
			if (ecp_to_xy(&rr, rx, ry) || memcmp(rx, ox, 32) || memcmp(ry, oy, 32)) bad++;
		}
		ecp_mul(&rr, &gg, p256_n);
		CHECK(bad == 0 && ecp_is_inf(&rr), "P-256: k*G as OpenSSL's, 100 random k (%d differ); n*G the point at infinity", bad);
		u8 off[32];
		memcpy(off, p256_gx, 32);
		off[31] ^= 1;
		CHECK(ecp_from_xy(&rr, off, p256_gy) == -1 && ecp_from_xy(&rr, p256_p, p256_gy) == -1,
		      "P-256: a point off the curve and a coordinate of p refused");
		EC_POINT_free(R);
		BN_free(bp), BN_free(ba), BN_free(bb), BN_free(bn), BN_free(bx), BN_free(by), BN_free(bk);
		BN_CTX_free(ctx);
		EC_GROUP_free(g);
	}

	/* --- SAE: IEEE 802.11-2020 Annex J.10 (hostapd's sae_tests) ------------ */
	{
		static const u8 a1[6] = { 0x4d, 0x3f, 0x2f, 0xff, 0xe3, 0x87 }, a2[6] = { 0xa5, 0xd8, 0xaa, 0x95, 0x8e, 0x3c };
		u8 rnd_v[32], msk[32], peer[98];
		hex("992465fd3daa3c60aa6565b7f62a2a7f2e12dd12f198faf4fbed89d7ff1ace94", rnd_v, 32);
		hex("9507a90f777a044d6a0830b91ea3d5dd70bece44e1acffb86983b5e1bf9fb322", msk, 32);
		hex("1300591b96f3397fb945100848e7b550543b6720d88337ee93fc49fd6df7e08b5223e71b9bb048d3873f20556953a96c91536fd8"
		    "ee6ca9b4a68a148b056a909be03e83ae208f60f8ef5537858074db0668703239986299" "9b511e0a1552a5fea317c2", peer, 98);
		struct sae sa;
		memset(&sa, 0, sizeof(sa));
		memcpy(sa.own, a1, 6), memcpy(sa.peer, a2, 6);
		sa.random = test_random;
		CHECK(sae_pwe_hnp(&sa, (const u8 *)"mekmitasdigoat", 14) == 0 && sae_commit(&sa, rnd_v, msk) == 0, "SAE: PWE, commit");
		u8 cm[200];
		u32 cl = sae_write_commit(&sa, cm);
		CHECK(cl == 98 && eq(cm, "13002e2c0f0db52440ad146d967114ce005ce1eab0aa2c2e5c2871b774f6c2575c65d5ad9e00829707aa36ba8b8597"
					 "38fc961d08243505f47c035376d7ac4bc8d7b95083bf43827d0fc31ed778dd3671fd21a46d1091d64b6f9a1e"
					 "1272621325dbe1", 98), "SAE: the commit of J.10 (hunting and pecking)");
		CHECK(sae_parse_commit(&sa, peer, 98) == 0 && sae_process(&sa) == 0, "SAE: the peer's commit taken");
		CHECK(eq(sa.kck, "1e733f6d9bd53256287304338831b09a39406d121017073a5c30db36f36cb81a", 32), "SAE: KCK of J.10");
		CHECK(eq(sa.pmk, "4e4dfab1a2dd8ac1a91790f953faaa452ae5c6873ab75b63605ba663f8a7fe59", 32), "SAE: PMK of J.10");
		CHECK(eq(sa.pmkid, "8747a600eea3f9f22475df58ca1e5498", 16), "SAE: PMKID of J.10");
		CHECK(sae_parse_commit(&sa, cm, 98) == SAE_SILENT, "SAE: our own commit back: a reflection, dropped silently");
		peer[0] = 20;
		CHECK(sae_parse_commit(&sa, peer, 98) == SAE_STATUS_GROUP_UNSUPPORTED, "SAE: group 20 refused");
		/* hash-to-element: PT, then the PWE for another pair (with the
		 * password identifier of J.10) */
		u8 ptv[64];
		static const u8 b1[6] = { 0x00, 0x09, 0x5b, 0x66, 0xec, 0x1e }, b2[6] = { 0x00, 0x0b, 0x6b, 0xd9, 0x02, 0x46 };
		memset(&sa, 0, sizeof(sa));
		memcpy(sa.own, b1, 6), memcpy(sa.peer, b2, 6);
		CHECK(sae_pt(ptv, (const u8 *)"byteme", 6, (const u8 *)"mekmitasdigoat", 14, "psk4internet") == 0 &&
		      sae_pwe_from_pt(&sa, ptv) == 0, "SAE: PT and its PWE");
		u8 px[32], py[32];
		ecp_to_xy(&sa.pwe, px, py);
		CHECK(eq(px, "c93049b9e64000f848201649e999f2b5c22dea69b5632c9df4d633b8aa1f6c1e", 32) &&
		      eq(py, "73634e94b53d82e7383a8d258199d9dc1a5ee8269d060382ccbf33e614ff59a0", 32), "SAE: the PWE of J.10 (hash-to-element)");
		/* both sides of an exchange agree; a wrong password does not */
		struct sae x1, x2;
		memset(&x1, 0, sizeof(x1)), memset(&x2, 0, sizeof(x2));
		memcpy(x1.own, a1, 6), memcpy(x1.peer, a2, 6), memcpy(x2.own, a2, 6), memcpy(x2.peer, a1, 6);
		x1.random = x2.random = test_random;
		sae_pwe_hnp(&x1, (const u8 *)"a password", 10);
		sae_pwe_hnp(&x2, (const u8 *)"a password", 10);
		sae_commit(&x1, NULL, NULL), sae_commit(&x2, NULL, NULL);
		u8 c1[98], c2[98], f1[34], f2[34];
		sae_write_commit(&x1, c1), sae_write_commit(&x2, c2);
		sae_parse_commit(&x1, c2, 98), sae_parse_commit(&x2, c1, 98);
		sae_process(&x1), sae_process(&x2);
		sae_write_confirm(&x1, f1), sae_write_confirm(&x2, f2);
		CHECK(!memcmp(x1.pmk, x2.pmk, 32) && sae_check_confirm(&x1, f2, 34) == 0 && sae_check_confirm(&x2, f1, 34) == 0,
		      "SAE: two sides, one PMK, each confirm taken");
		sae_pwe_hnp(&x2, (const u8 *)"another one", 11);
		sae_commit(&x2, NULL, NULL);
		sae_write_commit(&x2, c2);
		sae_parse_commit(&x1, c2, 98), sae_parse_commit(&x2, c1, 98);
		sae_process(&x1), sae_process(&x2);
		sae_write_confirm(&x2, f2);
		CHECK(sae_check_confirm(&x1, f2, 34) == -1, "SAE: another password: the confirm refused");
	}

	/* --- the RSN elements and the choice ------------------------------------ */
	u8 ie[64], rx[8];
	const char *why;
	struct wpa_ap ap;
	struct wpa_choice ch = { WPA_AKM_PSK, false, false }, cc;
	u32 il = wpa_own_ie(ie, &ch);
	CHECK(il == 22 && wpa_parse_ap(ie, il, NULL, 0, &ap, &why) == 0 && wpa_choose(&ap, false, false, &cc, &why) == 0 &&
	      cc.akm == WPA_AKM_PSK && !cc.mfp, "our WPA2 RSN element: PSK, no PMF");
	u8 t[64];
	memcpy(t, ie, il);
	t[7] = 2;
	CHECK(wpa_parse_ap(t, il, NULL, 0, &ap, &why) == 0 && wpa_choose(&ap, false, false, &cc, &why) == -1, "a TKIP group cipher refused: %s",
	      why);
	/* a transition AP: PSK and SAE, MFP capable, H2E in its RSNX element */
	static const u8 mixed[] = { 48, 28, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 3, 0, 0, 0x0f, 0xac, 2, 0, 0x0f, 0xac, 6,
				    0, 0x0f, 0xac, 8, 0x80, 0 };
	static const u8 rsnx_h2e[] = { 244, 1, 0x20 };
	CHECK(wpa_parse_ap(mixed, sizeof(mixed), rsnx_h2e, 3, &ap, &why) == 0 && wpa_choose(&ap, false, false, &cc, &why) == 0 &&
	      cc.akm == WPA_AKM_SAE && cc.mfp && cc.h2e, "transition mode: SAE (before PSK-SHA256 and PSK), PMF, hash-to-element");
	il = wpa_own_ie(ie, &cc);
	u32 xl = wpa_own_rsnx(rx, &cc);
	CHECK(il == 28 && ie[19] == 8 && ie[20] == 0x80 && ie[21] == 0 && ie[22] == 0 && ie[23] == 0 && ie[27] == 6 && xl == 3 &&
	      rx[2] == 0x20, "our WPA3 elements: SAE, MFPC, no PMKID, BIP-CMAC-128; RSNX with H2E");
	CHECK(wpa_parse_ap(mixed, sizeof(mixed), NULL, 0, &ap, &why) == 0 && wpa_choose(&ap, false, false, &cc, &why) == 0 &&
	      cc.akm == WPA_AKM_SAE && !cc.h2e && wpa_choose(&ap, true, false, &cc, &why) == -1,
	      "no RSNX element: hunting and pecking; an H2E-only BSS without it refused (%s)", why);
	static const u8 sha[] = { 48, 24, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 2, 0, 0, 0x0f, 0xac, 2, 0, 0x0f, 0xac, 6,
				  0xc0, 0 };   /* PSK, PSK-SHA256; MFP required */
	CHECK(wpa_parse_ap(sha, sizeof(sha), NULL, 0, &ap, &why) == 0 && wpa_choose(&ap, false, false, &cc, &why) == 0 &&
	      cc.akm == WPA_AKM_PSK_SHA256 && cc.mfp, "PSK and PSK-SHA256 with PMF required: PSK-SHA256 with PMF");
	CHECK(wpa_choose(&ap, false, true, &cc, &why) == -1 && strstr(why, "pmf=off"), "PMF required and pmf=off: refused (%s)", why);
	CHECK(wpa_parse_ap(mixed, sizeof(mixed), NULL, 0, &ap, &why) == 0 && wpa_choose(&ap, false, true, &cc, &why) == 0 &&
	      cc.akm == WPA_AKM_SAE && !cc.mfp, "PMF capable and pmf=off: without PMF");

	/* --- the keys of AKMs 6 and 8: the SHA-256 KDF, AES-CMAC ------------- */
	{
		u8 pk[32], an[32], sn[32], ptk[48], ptk2[48];
		static const u8 aa[6] = { 2, 0, 0, 0, 0, 1 }, sp[6] = { 2, 0, 0, 0, 0, 2 };
		for (int i = 0; i < 32; i++) pk[i] = (u8)i, an[i] = (u8)(0x40 + i), sn[i] = (u8)(0x80 + i);
		wpa_ptk(pk, WPA_AKM_SAE, aa, sp, an, sn, ptk);
		u8 d[76];
		memcpy(d, aa, 6), memcpy(d + 6, sp, 6), memcpy(d + 12, an, 32), memcpy(d + 44, sn, 32);
		sha256_kdf_bits(pk, 32, "Pairwise key expansion", d, 76, ptk2, 384);
		CHECK(!memcmp(ptk, ptk2, 48), "the PTK of SAE: the SHA-256 KDF-384");
		wpa_ptk(pk, WPA_AKM_PSK, aa, sp, an, sn, ptk);
		sha1_prf(pk, 32, "Pairwise key expansion", d, 76, ptk2, 48);
		CHECK(!memcmp(ptk, ptk2, 48), "the PTK of PSK: the SHA-1 PRF-384");
		u8 m1[16], m2[16];
		wpa_mic(ptk, WPA_AKM_PSK_SHA256, d, 76, m1);
		aes_cmac(ptk, d, 76, m2);
		CHECK(!memcmp(m1, m2, 16), "the MIC of PSK-SHA256: AES-128-CMAC");
	}

	/* --- BIP: a group deauthentication with its MMIE ------------------------ */
	{
		struct wpa_sm sm;
		memset(&sm, 0, sizeof(sm));
		sm.igtk_idx = 4;
		for (int i = 0; i < 16; i++) sm.igtk[i] = (u8)(0x20 + i);
		u8 f[64] = { 0xc0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 2, 0, 0, 0, 0, 1, 2, 0, 0, 0, 0, 1, 0x10, 0 };
		f[24] = 3, f[25] = 0;                    /* reason 3 */
		u8 *mm = f + 26;
		mm[0] = 76, mm[1] = 16, mm[2] = 4, mm[3] = 0;
		mm[4] = 5;                               /* IPN 5 */
		u8 aad[20], mac[16];
		aad[0] = 0xc0, aad[1] = 0;
		memcpy(aad + 2, f + 4, 18);
		/* AES-CMAC(IGTK, AAD || body with the MIC zeroed) by OpenSSL */
		u8 buf[64];
		memcpy(buf, aad, 20);
		memcpy(buf + 20, f + 24, 20);
		size_t ml = 16;
		EVP_MAC *m = EVP_MAC_fetch(NULL, "CMAC", NULL);
		EVP_MAC_CTX *mc = EVP_MAC_CTX_new(m);
		OSSL_PARAM prm[2] = { OSSL_PARAM_construct_utf8_string("cipher", "AES-128-CBC", 0), OSSL_PARAM_construct_end() };
		EVP_MAC_init(mc, sm.igtk, 16, prm);
		EVP_MAC_update(mc, buf, 40);
		EVP_MAC_final(mc, mac, &ml, 16);
		EVP_MAC_CTX_free(mc);
		EVP_MAC_free(m);
		memcpy(mm + 10, mac, 8);
		CHECK(wpa_bip_check(&sm, f, 44) == 0 && sm.ipn[0] == 5, "BIP: an MMIE as OpenSSL's CMAC makes it taken, the IPN kept");
		CHECK(wpa_bip_check(&sm, f, 44) == -1, "BIP: the same IPN again: a replay");
		mm[4] = 6;
		CHECK(wpa_bip_check(&sm, f, 44) == -1, "BIP: another IPN with the old MIC refused");
	}
}
