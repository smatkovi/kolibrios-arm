/* KolibriOS for ARM: a QR code encoder for programs (header only): bytes
 * in byte mode, error correction level L, versions 1-40, mask 0. After
 * the QR code specification (ISO/IEC 18004); the tables and the layout
 * steps as in Project Nayuki's qrcodegen.
 *
 *   int size = qr_encode(text, len, modules);   0 if it does not fit
 *   qr_module(modules, size, x, y)               dark?
 *
 * modules: QR_MAX_SIZE * QR_MAX_SIZE bytes. */
#ifndef QR_H
#define QR_H

#define QR_MAX_SIZE 177
#define QR_MAX_CODEWORDS 3706

static const signed char qr_ecc_per_block[41] = {
	-1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28,
	28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30,
};
static const signed char qr_blocks[41] = {
	-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8,
	8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25,
};

static int qr_raw_modules(int ver)
{
	int r = (16 * ver + 128) * ver + 64;
	if (ver >= 2) {
		int n = ver / 7 + 2;
		r -= (25 * n - 10) * n - 55;
		if (ver >= 7) r -= 36;
	}
	return r;
}

static int qr_data_codewords(int ver)
{
	return qr_raw_modules(ver) / 8 - qr_ecc_per_block[ver] * qr_blocks[ver];
}

static unsigned char qr_gf_mul(unsigned char x, unsigned char y)
{
	int z = 0;
	for (int i = 7; i >= 0; i--) {
		z = (z << 1) ^ ((z >> 7) * 0x11d);
		z ^= ((y >> i) & 1) * x;
	}
	return (unsigned char)z;
}

static void qr_rs_divisor(int degree, unsigned char *out)
{
	for (int i = 0; i < degree; i++) out[i] = 0;
	out[degree - 1] = 1;
	unsigned char root = 1;
	for (int i = 0; i < degree; i++) {
		for (int j = 0; j < degree; j++) {
			out[j] = qr_gf_mul(out[j], root);
			if (j + 1 < degree) out[j] ^= out[j + 1];
		}
		root = qr_gf_mul(root, 0x02);
	}
}

static void qr_rs_remainder(const unsigned char *data, int len, const unsigned char *div, int degree,
			    unsigned char *out)
{
	for (int i = 0; i < degree; i++) out[i] = 0;
	for (int i = 0; i < len; i++) {
		unsigned char f = data[i] ^ out[0];
		for (int j = 0; j + 1 < degree; j++) out[j] = out[j + 1];
		out[degree - 1] = 0;
		for (int j = 0; j < degree; j++) out[j] ^= qr_gf_mul(div[j], f);
	}
}

/* modules[y * QR_MAX_SIZE + x]: bit 0 dark, bit 1 a function module */
static void qr_set(unsigned char *m, int x, int y, int dark, int func)
{
	m[y * QR_MAX_SIZE + x] = (dark ? 1 : 0) | (func ? 2 : 0);
}

static int qr_module(const unsigned char *m, int size, int x, int y)
{
	return x >= 0 && y >= 0 && x < size && y < size && (m[y * QR_MAX_SIZE + x] & 1);
}

static void qr_finder(unsigned char *m, int size, int cx, int cy)
{
	for (int dy = -4; dy <= 4; dy++)
		for (int dx = -4; dx <= 4; dx++) {
			int d = dx < 0 ? -dx : dx, e = dy < 0 ? -dy : dy, dist = d > e ? d : e;
			int x = cx + dx, y = cy + dy;
			if (x >= 0 && x < size && y >= 0 && y < size) qr_set(m, x, y, dist != 2 && dist != 4, 1);
		}
}

static void qr_format(unsigned char *m, int size, int mask)
{
	int data = 1 << 3 | mask;               /* level L: 01 */
	int rem = data;
	for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
	int bits = (data << 10 | rem) ^ 0x5412;
	for (int i = 0; i <= 5; i++) qr_set(m, 8, i, (bits >> i) & 1, 1);
	qr_set(m, 8, 7, (bits >> 6) & 1, 1);
	qr_set(m, 8, 8, (bits >> 7) & 1, 1);
	qr_set(m, 7, 8, (bits >> 8) & 1, 1);
	for (int i = 9; i < 15; i++) qr_set(m, 14 - i, 8, (bits >> i) & 1, 1);
	for (int i = 0; i < 8; i++) qr_set(m, size - 1 - i, 8, (bits >> i) & 1, 1);
	for (int i = 8; i < 15; i++) qr_set(m, 8, size - 15 + i, (bits >> i) & 1, 1);
	qr_set(m, 8, size - 8, 1, 1);           /* the dark module */
}

static int qr_encode(const char *text, int len, unsigned char *m)
{
	static unsigned char data[QR_MAX_CODEWORDS], all[QR_MAX_CODEWORDS], ecc[32], div[32];
	int ver;
	for (ver = 1; ver <= 40; ver++) {
		int bits = 4 + (ver < 10 ? 8 : 16) + 8 * len;
		if (bits <= qr_data_codewords(ver) * 8) break;
	}
	if (ver > 40) return 0;
	int size = ver * 4 + 17, cap = qr_data_codewords(ver);
	/* the bit stream: byte mode, the length, the bytes, a terminator, pads */
	int nbits = 0;
	for (int i = 0; i < cap; i++) data[i] = 0;
#define PUT(v, n) for (int _i = (n) - 1; _i >= 0; _i--, nbits++) \
		if (((v) >> _i) & 1) data[nbits >> 3] |= 0x80 >> (nbits & 7)
	PUT(4, 4);
	PUT(len, ver < 10 ? 8 : 16);
	for (int i = 0; i < len; i++) PUT((unsigned char)text[i], 8);
	int term = cap * 8 - nbits < 4 ? cap * 8 - nbits : 4;
	nbits += term;
	nbits = (nbits + 7) & ~7;
	for (int pad = 0xec; nbits < cap * 8; pad ^= 0xec ^ 0x11) PUT(pad, 8);
#undef PUT
	/* blocks with their error correction, interleaved */
	int nblocks = qr_blocks[ver], ecl = qr_ecc_per_block[ver];
	int raw = qr_raw_modules(ver) / 8, nshort = nblocks - raw % nblocks, shortlen = raw / nblocks;
	qr_rs_divisor(ecl, div);
	int k = 0;
	for (int i = 0; i < nblocks; i++) {
		int dlen = shortlen - ecl + (i < nshort ? 0 : 1);
		qr_rs_remainder(data + k, dlen, div, ecl, ecc);
		for (int j = 0; j < dlen; j++) {
			/* column j of the data, short blocks lack the last one */
			int pos = j < shortlen - ecl ? j * nblocks + i : (shortlen - ecl) * nblocks + (i - nshort);
			all[pos] = data[k + j];
		}
		for (int j = 0; j < ecl; j++) all[(shortlen - ecl) * nblocks + (nblocks - nshort) + j * nblocks + i] = ecc[j];
		k += dlen;
	}
	/* the function patterns */
	for (int i = 0; i < size * QR_MAX_SIZE; i++) m[i] = 0;
	for (int i = 0; i < size; i++) {
		qr_set(m, 6, i, i % 2 == 0, 1);
		qr_set(m, i, 6, i % 2 == 0, 1);
	}
	qr_finder(m, size, 3, 3);
	qr_finder(m, size, size - 4, 3);
	qr_finder(m, size, 3, size - 4);
	if (ver >= 2) {
		int n = ver / 7 + 2, step = ver == 32 ? 26 : (ver * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
		int pos[7];
		pos[0] = 6;
		for (int i = n - 1, p = ver * 4 + 10; i >= 1; i--, p -= step) pos[i] = p;
		for (int i = 0; i < n; i++)
			for (int j = 0; j < n; j++) {
				if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;
				for (int dy = -2; dy <= 2; dy++)
					for (int dx = -2; dx <= 2; dx++) {
						int d = dx < 0 ? -dx : dx, e = dy < 0 ? -dy : dy;
						qr_set(m, pos[i] + dx, pos[j] + dy, (d > e ? d : e) != 1, 1);
					}
			}
	}
	qr_format(m, size, 0);                  /* reserves the format areas */
	if (ver >= 7) {
		int rem = ver;
		for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1f25);
		long bits = (long)ver << 12 | rem;
		for (int i = 0; i < 18; i++) {
			int a = size - 11 + i % 3, b = i / 3, bit = (bits >> i) & 1;
			qr_set(m, a, b, bit, 1);
			qr_set(m, b, a, bit, 1);
		}
	}
	/* the data, in the zigzag, mask 0 ((x + y) even: inverted) */
	int i = 0;
	for (int right = size - 1; right >= 1; right -= 2) {
		if (right == 6) right = 5;
		for (int vert = 0; vert < size; vert++)
			for (int j = 0; j < 2; j++) {
				int x = right - j, up = ((right + 1) & 2) == 0, y = up ? size - 1 - vert : vert;
				if (m[y * QR_MAX_SIZE + x] & 2) continue;
				int dark = 0;
				if (i < raw * 8) dark = (all[i >> 3] >> (7 - (i & 7))) & 1;
				i++;
				if ((x + y) % 2 == 0) dark = !dark;
				qr_set(m, x, y, dark, 0);
			}
	}
	return size;
}

#endif
