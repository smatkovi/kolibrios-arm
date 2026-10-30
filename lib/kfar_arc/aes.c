/* kfar_arc for KolibriOS for ARM: AES (aes.inc), decryption only, for
 * 7z's 7zAES filter (7zaes.c).
 *
 * Translated to C from KolibriOS's programs/fs/kfar/kfar_arc/aes.inc,
 * diamond (Evgeny Grechnikov), 2007, GPL-2.0 (kfar_arc.h): the tables
 * computed at the start from GF(2^8)'s powers of x+1 (init_aes), the key
 * expanded (aes_setkey), blocks of 16 bytes decrypted (aes_decode) with
 * the reverse MixColumns table. */
#include "kfar_arc.h"
#include "sha256.h"

#define ROL32(x, n) ((x) << (n) | (x) >> (32 - (n)))

static u8 aes_pow_table[256];           /* pow[a] = 3^a */
static u8 aes_log_table[256];           /* log[3^a] = a */
static u8 aes_sbox[256];                /* SubBytes(a) */
static u8 aes_sbox_rev[256];            /* SubBytes^{-1}(a) */
static u32 aes_mcrtable[256];           /* MixColumns^{-1}(a,0,0,0) */

static u8 rol8(u8 v, int n) { return v << n | v >> (8 - n); }

/* aes.inc: init_aes: byte values in SubBytes transform are items of
 * GF(2^8) = F_2[x]/(x^8+x^4+x^3+x+1); x+1 is a primitive item */
void init_aes(void)
{
	u8 al = 1;
	for (u32 ebx = 0; ebx < 256; ebx++) {
		aes_pow_table[ebx] = al;
		aes_log_table[al] = ebx;
		/* multiplication by x+1: by x mod 0x11B, plus the operand */
		al ^= (u8)(al << 1) ^ (al & 0x80 ? 0x1B : 0);
	}
	/* the table for the SubBytes transform */
	aes_sbox[0] = 0x63;
	aes_sbox_rev[0x63] = 0;
	for (u32 ebx = 1; ebx < 256; ebx++) {
		/* the inverse in GF(2^8), then the linear transform */
		u8 cl = aes_pow_table[(u8)(aes_log_table[ebx] ^ 0xFF)];
		u8 v = cl ^ rol8(cl, 1) ^ rol8(cl, 2) ^ rol8(cl, 3) ^ rol8(cl, 4) ^ 0x63;
		aes_sbox[ebx] = v;
		aes_sbox_rev[v] = ebx;
	}
	/* the reverse MixColumn transform: 0x0E, 0x09, 0x0D, 0x0B times a
	 * (log_table[9]=0xC7, [0xB]=0x68, [0xD]=0xEE, [0xE]=0xDF) */
	aes_mcrtable[0] = 0;
	for (u32 ebx = 1; ebx < 256; ebx++) {
		u32 l = aes_log_table[ebx];
		u8 m[4];
		static const u8 logs[4] = { 0xDF, 0xC7, 0xEE, 0x68 };
		for (int k = 0; k < 4; k++) {
			u32 s = l + logs[k];
			m[k] = aes_pow_table[s >= 255 ? s - 255 : s];
		}
		aes_mcrtable[ebx] = m[0] | m[1] << 8 | m[2] << 16 | (u32)m[3] << 24;
	}
}

/* aes.inc: aes_setkey: in: esi -> key, edx = key size in dwords, edi ->
 * AES data struc */
void aes_setkey(const u8 *esi, u32 edx, aes_data *edi)
{
	u32 nr = edx + 6;                       /* number of rounds (buffer size=4) */
	edi->nr = nr;
	u8 *w = edi->KeyExpanded;
	u32 total = 4 * (nr + 1);               /* words */
	memcpy(w, esi, edx * 4);
	u8 rcon = 1;
	for (u32 i = edx; i < total; i++) {
		u8 t[4];
		memcpy(t, w + 4 * (i - 1), 4);
		if (i % edx == 0) {
			/* RotWord, SubWord, Rcon */
			u8 t0 = t[0];
			t[0] = aes_sbox[t[1]] ^ rcon;
			t[1] = aes_sbox[t[2]];
			t[2] = aes_sbox[t[3]];
			t[3] = aes_sbox[t0];
			rcon = (u8)(rcon << 1) ^ (rcon & 0x80 ? 0x1B : 0);
		} else if (edx == 8 && i % edx == 4) {
			for (int k = 0; k < 4; k++) t[k] = aes_sbox[t[k]];
		}
		for (int k = 0; k < 4; k++) w[4 * i + k] = w[4 * (i - edx) + k] ^ t[k];
	}
}

/* aes.inc: aes_decode: in: esi -> in, ebx -> out, edi -> AES state */
void aes_decode(const u8 *esi, u8 *ebx, const aes_data *edi)
{
	u8 s[16];
	u32 ebp = edi->nr;
	const u8 *rk = edi->KeyExpanded + 16 * ebp;    /* the last round key */
	/* (AddRoundKey) */
	for (int i = 0; i < 16; i++) s[i] = esi[i] ^ rk[i];
	for (;;) {
		/* .loop0: (ShiftRows) back: row r right by r */
		u8 t[16];
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++) t[4 * ((c + r) & 3) + r] = s[4 * c + r];
		/* (SubBytes) back */
		for (int i = 0; i < 16; i++) s[i] = aes_sbox_rev[t[i]];
		rk -= 16;
		/* reverse normal rounds */
		if (!--ebp) break;
		/* (AddRoundKey), (MixColumns) back */
		for (int c = 0; c < 4; c++) {
			u8 a[4];
			for (int r = 0; r < 4; r++) a[r] = s[4 * c + r] ^ rk[4 * c + r];
			u32 v = aes_mcrtable[a[0]] ^ ROL32(aes_mcrtable[a[1]], 8) ^ ROL32(aes_mcrtable[a[2]], 16) ^
				ROL32(aes_mcrtable[a[3]], 24);
			for (int r = 0; r < 4; r++) s[4 * c + r] = v >> (8 * r);
		}
	}
	/* .done: (AddRoundKey) */
	for (int i = 0; i < 16; i++) ebx[i] = s[i] ^ rk[i];
}
