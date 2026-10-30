/* KolibriOS for ARM: SHA-1, SHA-256, HMAC, PBKDF2, the 802.11 PRF and
 * KDF, HKDF-Expand, AES-128, its key wrap and CMAC (crypto.h). Byte-wise
 * and plain: the handshakes need a few thousand hash blocks (the PMK, SAE's
 * hunting and pecking) and a few AES blocks. */
#include "crypto.h"

/* --- SHA-1 (FIPS 180-4) -------------------------------------------------- */

static inline u32 rol(u32 x, int n) { return x << n | x >> (32 - n); }

static void sha1_block(u32 h[5], const u8 *p)
{
	u32 w[80];
	for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
	for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
	for (int i = 0; i < 80; i++) {
		u32 f, k;
		if (i < 20) f = (b & c) | (~b & d), k = 0x5a827999;
		else if (i < 40) f = b ^ c ^ d, k = 0x6ed9eba1;
		else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
		else f = b ^ c ^ d, k = 0xca62c1d6;
		u32 t = rol(a, 5) + f + e + k + w[i];
		e = d, d = c, c = rol(b, 30), b = a, a = t;
	}
	h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

void sha1_init(struct sha1 *s)
{
	s->h[0] = 0x67452301, s->h[1] = 0xefcdab89, s->h[2] = 0x98badcfe, s->h[3] = 0x10325476;
	s->h[4] = 0xc3d2e1f0;
	s->n = 0;
	s->used = 0;
}

void sha1_update(struct sha1 *s, const void *data, u32 len)
{
	const u8 *p = data;
	s->n += len;
	while (len) {
		u32 k = MIN(len, 64 - s->used);
		mcopy(s->buf + s->used, p, k);
		s->used += k, p += k, len -= k;
		if (s->used == 64) {
			sha1_block(s->h, s->buf);
			s->used = 0;
		}
	}
}

void sha1_final(struct sha1 *s, u8 out[SHA1_LEN])
{
	u64 bits = s->n * 8;
	u8 pad = 0x80;
	sha1_update(s, &pad, 1);
	pad = 0;
	while (s->used != 56) sha1_update(s, &pad, 1);
	u8 l[8];
	for (int i = 0; i < 8; i++) l[i] = (u8)(bits >> (56 - 8 * i));
	sha1_update(s, l, 8);
	for (int i = 0; i < 5; i++) {
		out[4 * i] = (u8)(s->h[i] >> 24), out[4 * i + 1] = (u8)(s->h[i] >> 16);
		out[4 * i + 2] = (u8)(s->h[i] >> 8), out[4 * i + 3] = (u8)s->h[i];
	}
	crypto_wipe(s, sizeof(*s));
}

void sha1(const void *p, u32 len, u8 out[SHA1_LEN])
{
	struct sha1 s;
	sha1_init(&s);
	sha1_update(&s, p, len);
	sha1_final(&s, out);
}

/* --- HMAC-SHA1 (RFC 2104) ------------------------------------------------ */

void hmac_sha1_vec(const u8 *key, u32 key_len, int n, const u8 *const p[], const u32 len[], u8 out[SHA1_LEN])
{
	u8 k[64], tk[SHA1_LEN], pad[64];
	if (key_len > 64) {
		sha1(key, key_len, tk);
		key = tk;
		key_len = SHA1_LEN;
	}
	mzero(k, 64);
	mcopy(k, key, key_len);
	struct sha1 s;
	for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
	sha1_init(&s);
	sha1_update(&s, pad, 64);
	for (int i = 0; i < n; i++) sha1_update(&s, p[i], len[i]);
	u8 inner[SHA1_LEN];
	sha1_final(&s, inner);
	for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
	sha1_init(&s);
	sha1_update(&s, pad, 64);
	sha1_update(&s, inner, SHA1_LEN);
	sha1_final(&s, out);
	crypto_wipe(k, sizeof(k));
	crypto_wipe(pad, sizeof(pad));
	crypto_wipe(tk, sizeof(tk));
	crypto_wipe(inner, sizeof(inner));
}

void hmac_sha1(const u8 *key, u32 key_len, const u8 *p, u32 len, u8 out[SHA1_LEN])
{
	hmac_sha1_vec(key, key_len, 1, &p, &len, out);
}

/* --- SHA-256 (FIPS 180-4, 6.2) ---------------------------------------------- */

static const u32 k256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline u32 ror(u32 x, int n) { return x >> n | x << (32 - n); }

static void sha256_block(u32 h[8], const u8 *p)
{
	u32 w[64];
	for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
	for (int i = 16; i < 64; i++) {
		u32 s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
		u32 s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
	for (int i = 0; i < 64; i++) {
		u32 t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k256[i] + w[i];
		u32 t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
	}
	h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

void sha256_init(struct sha256 *s)
{
	static const u32 iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
				   0x5be0cd19 };
	mcopy(s->h, iv, sizeof(iv));
	s->n = 0;
	s->used = 0;
}

void sha256_update(struct sha256 *s, const void *data, u32 len)
{
	const u8 *p = data;
	s->n += len;
	while (len) {
		u32 k = MIN(len, 64 - s->used);
		mcopy(s->buf + s->used, p, k);
		s->used += k, p += k, len -= k;
		if (s->used == 64) {
			sha256_block(s->h, s->buf);
			s->used = 0;
		}
	}
}

void sha256_final(struct sha256 *s, u8 out[SHA256_LEN])
{
	u64 bits = s->n * 8;
	u8 pad = 0x80;
	sha256_update(s, &pad, 1);
	pad = 0;
	while (s->used != 56) sha256_update(s, &pad, 1);
	u8 l[8];
	for (int i = 0; i < 8; i++) l[i] = (u8)(bits >> (56 - 8 * i));
	sha256_update(s, l, 8);
	for (int i = 0; i < 8; i++) {
		out[4 * i] = (u8)(s->h[i] >> 24), out[4 * i + 1] = (u8)(s->h[i] >> 16);
		out[4 * i + 2] = (u8)(s->h[i] >> 8), out[4 * i + 3] = (u8)s->h[i];
	}
	crypto_wipe(s, sizeof(*s));
}

void sha256(const void *p, u32 len, u8 out[SHA256_LEN])
{
	struct sha256 s;
	sha256_init(&s);
	sha256_update(&s, p, len);
	sha256_final(&s, out);
}

void hmac_sha256_vec(const u8 *key, u32 key_len, int n, const u8 *const p[], const u32 len[], u8 out[SHA256_LEN])
{
	u8 k[64], tk[SHA256_LEN], pad[64], inner[SHA256_LEN];
	if (key_len > 64) {
		sha256(key, key_len, tk);
		key = tk;
		key_len = SHA256_LEN;
	}
	mzero(k, 64);
	mcopy(k, key, key_len);
	struct sha256 s;
	for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
	sha256_init(&s);
	sha256_update(&s, pad, 64);
	for (int i = 0; i < n; i++) sha256_update(&s, p[i], len[i]);
	sha256_final(&s, inner);
	for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
	sha256_init(&s);
	sha256_update(&s, pad, 64);
	sha256_update(&s, inner, SHA256_LEN);
	sha256_final(&s, out);
	crypto_wipe(k, sizeof(k));
	crypto_wipe(pad, sizeof(pad));
	crypto_wipe(tk, sizeof(tk));
	crypto_wipe(inner, sizeof(inner));
}

void hmac_sha256(const u8 *key, u32 key_len, const u8 *p, u32 len, u8 out[SHA256_LEN])
{
	hmac_sha256_vec(key, key_len, 1, &p, &len, out);
}

void sha256_kdf_bits(const u8 *key, u32 key_len, const char *label, const u8 *data, u32 data_len, u8 *out, u32 bits)
{
	u32 n = (bits + 7) / 8, pos = 0;
	u8 ctr[2], blen[2] = { (u8)bits, (u8)(bits >> 8) }, h[SHA256_LEN];
	const u8 *v[4] = { ctr, (const u8 *)label, data, blen };
	u32 l[4] = { 2, mstrlen(label), data_len, 2 };
	for (u16 i = 1; pos < n; i++) {
		ctr[0] = (u8)i, ctr[1] = (u8)(i >> 8);
		hmac_sha256_vec(key, key_len, 4, v, l, h);
		u32 k = MIN(n - pos, (u32)SHA256_LEN);
		mcopy(out + pos, h, k);
		pos += k;
	}
	if (bits % 8) out[n - 1] &= (u8)(0xff << (8 - bits % 8));
	crypto_wipe(h, sizeof(h));
}

void hkdf_expand_sha256(const u8 *prk, u32 prk_len, const u8 *info, u32 info_len, u8 *out, u32 out_len)
{
	u8 t[SHA256_LEN], iter = 1;
	const u8 *v[3] = { t, info, &iter };
	u32 l[3] = { 0, info_len, 1 }, pos = 0;
	while (pos < out_len) {
		/* T(1) = HMAC(PRK, info || 1), T(i) = HMAC(PRK, T(i-1) || info || i) */
		hmac_sha256_vec(prk, prk_len, 3, v, l, t);
		l[0] = SHA256_LEN;
		u32 k = MIN(out_len - pos, (u32)SHA256_LEN);
		mcopy(out + pos, t, k);
		pos += k;
		iter++;
	}
	crypto_wipe(t, sizeof(t));
}

/* --- PBKDF2-SHA1 (sha1-pbkdf2.c) ------------------------------------------ */

void pbkdf2_sha1(const char *pass, const u8 *salt, u32 salt_len, u32 iterations, u8 *out, u32 out_len)
{
	u32 plen = mstrlen(pass);
	for (u32 blk = 1, done = 0; done < out_len; blk++) {
		u8 cnt[4] = { (u8)(blk >> 24), (u8)(blk >> 16), (u8)(blk >> 8), (u8)blk };
		const u8 *v[2] = { salt, cnt };
		u32 l[2] = { salt_len, 4 };
		u8 u[SHA1_LEN], t[SHA1_LEN];
		hmac_sha1_vec((const u8 *)pass, plen, 2, v, l, u);
		mcopy(t, u, SHA1_LEN);
		for (u32 i = 1; i < iterations; i++) {
			hmac_sha1((const u8 *)pass, plen, u, SHA1_LEN, u);
			for (int j = 0; j < SHA1_LEN; j++) t[j] ^= u[j];
		}
		u32 k = MIN(out_len - done, (u32)SHA1_LEN);
		mcopy(out + done, t, k);
		done += k;
		crypto_wipe(u, sizeof(u));
		crypto_wipe(t, sizeof(t));
	}
}

/* --- the 802.11 PRF (sha1-prf.c) ------------------------------------------ */

void sha1_prf(const u8 *key, u32 key_len, const char *label, const u8 *data, u32 data_len, u8 *out, u32 out_len)
{
	u8 counter = 0, zero = 0, h[SHA1_LEN];
	const u8 *v[4] = { (const u8 *)label, &zero, data, &counter };
	u32 l[4] = { mstrlen(label), 1, data_len, 1 };
	for (u32 pos = 0; pos < out_len; counter++) {
		hmac_sha1_vec(key, key_len, 4, v, l, h);
		u32 k = MIN(out_len - pos, (u32)SHA1_LEN);
		mcopy(out + pos, h, k);
		pos += k;
	}
	crypto_wipe(h, sizeof(h));
}

/* --- AES-128 (FIPS 197) --------------------------------------------------- */

static const u8 sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static u8 inv_sbox[256];
static bool inv_ready;

static u8 xtime(u8 x) { return (u8)(x << 1 ^ (x & 0x80 ? 0x1b : 0)); }

static u8 gmul(u8 a, u8 b)
{
	u8 r = 0;
	while (b) {
		if (b & 1) r ^= a;
		a = xtime(a);
		b >>= 1;
	}
	return r;
}

void aes128_init(struct aes128 *a, const u8 key[16])
{
	if (!inv_ready) {
		for (int i = 0; i < 256; i++) inv_sbox[sbox[i]] = (u8)i;
		inv_ready = true;
	}
	u8 rcon = 1;
	for (int i = 0; i < 4; i++) a->rk[i] = (u32)key[4 * i] << 24 | key[4 * i + 1] << 16 | key[4 * i + 2] << 8 | key[4 * i + 3];
	for (int i = 4; i < 44; i++) {
		u32 t = a->rk[i - 1];
		if (i % 4 == 0) {
			t = (u32)sbox[t >> 16 & 0xff] << 24 | (u32)sbox[t >> 8 & 0xff] << 16 | (u32)sbox[t & 0xff] << 8 |
			    sbox[t >> 24];
			t ^= (u32)rcon << 24;
			rcon = xtime(rcon);
		}
		a->rk[i] = a->rk[i - 4] ^ t;
	}
}

static void add_key(u8 s[16], const u32 *k)
{
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) s[4 * c + r] ^= (u8)(k[c] >> (24 - 8 * r));
}

void aes128_encrypt(const struct aes128 *a, const u8 in[16], u8 out[16])
{
	u8 s[16], t[16];
	mcopy(s, in, 16);
	add_key(s, a->rk);
	for (int round = 1; round <= 10; round++) {
		for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
		/* ShiftRows: row r moves left by r (the state column-major) */
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++) t[4 * c + r] = s[4 * ((c + r) % 4) + r];
		if (round < 10)
			for (int c = 0; c < 4; c++) {
				u8 *x = t + 4 * c, a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
				s[4 * c] = (u8)(xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3);
				s[4 * c + 1] = (u8)(a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3);
				s[4 * c + 2] = (u8)(a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3);
				s[4 * c + 3] = (u8)(xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3));
			}
		else
			mcopy(s, t, 16);
		add_key(s, a->rk + 4 * round);
	}
	mcopy(out, s, 16);
}

void aes128_decrypt(const struct aes128 *a, const u8 in[16], u8 out[16])
{
	u8 s[16], t[16];
	mcopy(s, in, 16);
	add_key(s, a->rk + 40);
	for (int round = 9; round >= 0; round--) {
		/* InvShiftRows, InvSubBytes */
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++) t[4 * ((c + r) % 4) + r] = s[4 * c + r];
		for (int i = 0; i < 16; i++) s[i] = inv_sbox[t[i]];
		add_key(s, a->rk + 4 * round);
		if (round > 0)
			for (int c = 0; c < 4; c++) {
				u8 *x = s + 4 * c, a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
				x[0] = (u8)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
				x[1] = (u8)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
				x[2] = (u8)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
				x[3] = (u8)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
			}
	}
	mcopy(out, s, 16);
}

/* --- the key wrap (RFC 3394, aes-wrap.c, aes-unwrap.c) --------------------- */

void aes_wrap(const u8 kek[16], u32 n, const u8 *plain, u8 *cipher)
{
	struct aes128 a;
	u8 b[16], *r = cipher + 8;
	aes128_init(&a, kek);
	for (int i = 0; i < 8; i++) cipher[i] = 0xa6;
	mcopy(r, plain, 8 * n);
	for (u32 j = 0; j <= 5; j++)
		for (u32 i = 1; i <= n; i++) {
			mcopy(b, cipher, 8);
			mcopy(b + 8, r + 8 * (i - 1), 8);
			aes128_encrypt(&a, b, b);
			u32 t = n * j + i;
			mcopy(cipher, b, 8);
			cipher[7] ^= (u8)t, cipher[6] ^= (u8)(t >> 8), cipher[5] ^= (u8)(t >> 16), cipher[4] ^= (u8)(t >> 24);
			mcopy(r + 8 * (i - 1), b + 8, 8);
		}
	crypto_wipe(&a, sizeof(a));
	crypto_wipe(b, sizeof(b));
}

int aes_unwrap(const u8 kek[16], u32 n, const u8 *cipher, u8 *plain)
{
	struct aes128 a;
	u8 av[8], b[16];
	aes128_init(&a, kek);
	mcopy(av, cipher, 8);
	mcopy(plain, cipher + 8, 8 * n);
	for (int j = 5; j >= 0; j--)
		for (u32 i = n; i >= 1; i--) {
			u32 t = n * (u32)j + i;
			mcopy(b, av, 8);
			b[7] ^= (u8)t, b[6] ^= (u8)(t >> 8), b[5] ^= (u8)(t >> 16), b[4] ^= (u8)(t >> 24);
			mcopy(b + 8, plain + 8 * (i - 1), 8);
			aes128_decrypt(&a, b, b);
			mcopy(av, b, 8);
			mcopy(plain + 8 * (i - 1), b + 8, 8);
		}
	crypto_wipe(&a, sizeof(a));
	crypto_wipe(b, sizeof(b));
	for (int i = 0; i < 8; i++)
		if (av[i] != 0xa6) return -1;
	return 0;
}

/* --- AES-CMAC (RFC 4493; aes-omac1.c) ------------------------------------ */

static void gf_double(u8 b[16])
{
	u8 carry = b[0] & 0x80;
	for (int i = 0; i < 15; i++) b[i] = (u8)(b[i] << 1 | b[i + 1] >> 7);
	b[15] = (u8)(b[15] << 1);
	if (carry) b[15] ^= 0x87;
}

void aes_cmac_vec(const u8 key[16], int n, const u8 *const p[], const u32 len[], u8 mac[16])
{
	struct aes128 a;
	aes128_init(&a, key);
	u8 k1[16] = { 0 }, cbc[16] = { 0 }, blk[16];
	aes128_encrypt(&a, k1, k1);                 /* L = AES(K, 0) */
	gf_double(k1);                              /* K1 */
	u32 total = 0;
	for (int i = 0; i < n; i++) total += len[i];
	u32 used = 0, seen = 0;
	int e = 0;
	u32 off = 0;
	/* every block but the last through the CBC chain */
	while (total - seen > 16 || (total - seen == 16 && used)) {
		while (used < 16) {
			while (e < n && off == len[e]) e++, off = 0;
			blk[used++] = p[e][off++];
			seen++;
		}
		for (int i = 0; i < 16; i++) cbc[i] ^= blk[i];
		aes128_encrypt(&a, cbc, cbc);
		used = 0;
	}
	/* the last block: whole (with K1) or padded with 10* (with K2) */
	u32 rest = total - seen;
	for (u32 i = 0; i < rest; i++) {
		while (e < n && off == len[e]) e++, off = 0;
		blk[i] = p[e][off++];
	}
	if (rest == 16 && total) {
		for (int i = 0; i < 16; i++) cbc[i] ^= blk[i] ^ k1[i];
	} else {
		u8 k2[16];
		mcopy(k2, k1, 16);
		gf_double(k2);
		blk[rest] = 0x80;
		for (u32 i = rest + 1; i < 16; i++) blk[i] = 0;
		for (int i = 0; i < 16; i++) cbc[i] ^= blk[i] ^ k2[i];
		crypto_wipe(k2, sizeof(k2));
	}
	aes128_encrypt(&a, cbc, mac);
	crypto_wipe(&a, sizeof(a));
	crypto_wipe(k1, sizeof(k1));
	crypto_wipe(cbc, sizeof(cbc));
	crypto_wipe(blk, sizeof(blk));
}

void aes_cmac(const u8 key[16], const u8 *p, u32 len, u8 mac[16])
{
	aes_cmac_vec(key, 1, &p, &len, mac);
}

int crypto_memcmp(const void *a, const void *b, u32 n)
{
	const u8 *x = a, *y = b;
	u8 d = 0;
	for (u32 i = 0; i < n; i++) d |= x[i] ^ y[i];
	return d;
}

void crypto_wipe(void *p, u32 n)
{
	volatile u8 *v = p;
	while (n--) *v++ = 0;
}
