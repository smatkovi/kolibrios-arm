/* KolibriOS for ARM: SHA-1, HMAC-SHA1, PBKDF2, the 802.11 PRF, AES-128
 * and its key wrap (crypto.h). Byte-wise and plain: the handshake needs
 * a few thousand SHA-1 blocks (the PMK) and a few AES blocks (the GTK). */
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
