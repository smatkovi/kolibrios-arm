/* kfar_arc for KolibriOS for ARM: SHA-256 (sha256.inc), for 7z's key
 * derivation (7zaes.c).
 *
 * Translated to C from KolibriOS's programs/fs/kfar/kfar_arc/
 * sha256.inc, diamond (Evgeny Grechnikov), 2007, GPL-2.0 (kfar_arc.h):
 * one hash at a time in the original's globals (sha256_buf,
 * sha256_digest, sha256_count, sha256_size). */
#include "kfar_arc.h"
#include "sha256.h"

static const u32 sha256_start_digest[8] = {
	0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19,
};
static const u32 sha256_const[64] = {
	0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
	0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3, 0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
	0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
	0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
	0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13, 0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
	0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
	0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
	0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208, 0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
};

static u8 sha256_buf[64];
static u32 sha256_digest[8];
static u32 sha256_count;                /* bytes in buffer */
static u64 sha256_size;                 /* bytes processed */

void sha256_init(void)
{
	memcpy(sha256_digest, sha256_start_digest, sizeof(sha256_digest));
	sha256_count = 0;
	sha256_size = 0;
}

#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))

/* core of SHA-256: transform 64-byte 'sha256_buf' to updated
 * 'sha256_digest' */
static void sha256_transform(void)
{
	u32 w[64];
	for (int i = 0; i < 16; i++)
		w[i] = (u32)sha256_buf[4 * i] << 24 | sha256_buf[4 * i + 1] << 16 | sha256_buf[4 * i + 2] << 8 |
		       sha256_buf[4 * i + 3];
	for (int i = 16; i < 64; i++) {
		u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ w[i - 15] >> 3;
		u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ w[i - 2] >> 10;
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	u32 a = sha256_digest[0], b = sha256_digest[1], c = sha256_digest[2], d = sha256_digest[3];
	u32 e = sha256_digest[4], f = sha256_digest[5], g = sha256_digest[6], h = sha256_digest[7];
	for (int i = 0; i < 64; i++) {
		u32 t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + sha256_const[i] + w[i];
		u32 t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	sha256_digest[0] += a;
	sha256_digest[1] += b;
	sha256_digest[2] += c;
	sha256_digest[3] += d;
	sha256_digest[4] += e;
	sha256_digest[5] += f;
	sha256_digest[6] += g;
	sha256_digest[7] += h;
}

/* sha256_update: in: esi -> data, edx = size */
void sha256_update(const void *esi, u32 edx)
{
	const u8 *p = esi;
	sha256_size += edx;
	while (edx) {
		u32 n = 64 - sha256_count;
		if (n > edx) n = edx;
		memcpy(sha256_buf + sha256_count, p, n);
		sha256_count += n;
		p += n;
		edx -= n;
		if (sha256_count == 64) {
			sha256_transform();
			sha256_count = 0;
		}
	}
}

/* sha256_final: in: edi -> the digest's 32 bytes */
void sha256_final(u8 *edi)
{
	u64 bits = sha256_size << 3;
	u8 pad = 0x80;
	sha256_update(&pad, 1);
	pad = 0;
	while (sha256_count != 56) sha256_update(&pad, 1);
	u8 len[8];
	for (int i = 0; i < 8; i++) len[i] = bits >> (56 - 8 * i);
	sha256_update(len, 8);
	for (int i = 0; i < 8; i++) {
		edi[4 * i] = sha256_digest[i] >> 24;
		edi[4 * i + 1] = sha256_digest[i] >> 16;
		edi[4 * i + 2] = sha256_digest[i] >> 8;
		edi[4 * i + 3] = sha256_digest[i];
	}
}
