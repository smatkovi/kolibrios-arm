/* KolibriOS for ARM: P-256 (ecc.h). Montgomery multiplication (CIOS,
 * 64-bit limbs, the products in 128 bits), Jacobian point formulas for a
 * = -3 (doubling "dbl-2001-b", addition "add-2007-bl" of the Explicit-
 * Formulas Database, the special cases handled), a Montgomery ladder for
 * the scalar multiplication. The curve: FIPS 186-4 D.1.2.3. */
#include "ecc.h"

typedef unsigned __int128 u128;

const u8 p256_p[32] = {
	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
const u8 p256_n[32] = {
	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
};
const u8 p256_b[32] = {
	0x5a, 0xc6, 0x35, 0xd8, 0xaa, 0x3a, 0x93, 0xe7, 0xb3, 0xeb, 0xbd, 0x55, 0x76, 0x98, 0x86, 0xbc,
	0x65, 0x1d, 0x06, 0xb0, 0xcc, 0x53, 0xb0, 0xf6, 0x3b, 0xce, 0x3c, 0x3e, 0x27, 0xd2, 0x60, 0x4b,
};
const u8 p256_gx[32] = {
	0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
	0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
};
const u8 p256_gy[32] = {
	0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16,
	0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce, 0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5,
};

/* --- plain 256-bit numbers ------------------------------------------------- */

void fe_from_bytes(fe *a, const u8 b[32])
{
	for (int i = 0; i < 4; i++) {
		u64 w = 0;
		for (int j = 0; j < 8; j++) w = w << 8 | b[8 * (3 - i) + j];
		a->v[i] = w;
	}
}

void fe_to_bytes(u8 b[32], const fe *a)
{
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 8; j++) b[8 * (3 - i) + j] = (u8)(a->v[i] >> (56 - 8 * j));
}

int fe_cmp(const fe *a, const fe *b)
{
	for (int i = 3; i >= 0; i--) {
		if (a->v[i] > b->v[i]) return 1;
		if (a->v[i] < b->v[i]) return -1;
	}
	return 0;
}

bool fe_is_zero(const fe *a)
{
	return !(a->v[0] | a->v[1] | a->v[2] | a->v[3]);
}

/* r = a + b, the carry out */
static u64 add4(fe *r, const fe *a, const fe *b)
{
	u128 c = 0;
	for (int i = 0; i < 4; i++) {
		c += (u128)a->v[i] + b->v[i];
		r->v[i] = (u64)c;
		c >>= 64;
	}
	return (u64)c;
}

/* r = a - b, the borrow out */
static u64 sub4(fe *r, const fe *a, const fe *b)
{
	u64 br = 0;
	for (int i = 0; i < 4; i++) {
		u128 d = (u128)a->v[i] - b->v[i] - br;
		r->v[i] = (u64)d;
		br = (u64)(d >> 64) & 1;
	}
	return br;
}

/* r = cond ? b : a, without a branch */
static void sel(fe *r, const fe *a, const fe *b, u64 cond)
{
	u64 m = 0 - (cond & 1);
	for (int i = 0; i < 4; i++) r->v[i] = (a->v[i] & ~m) | (b->v[i] & m);
}

/* --- Montgomery arithmetic for a modulus m > 2^255 --------------------------- */

struct mod {
	fe m, r2, one;                      /* the modulus, R^2 mod m, R mod m */
	u64 minv;                           /* -m^-1 mod 2^64 */
};

static struct mod P, N;
static fe b_m, a_m, three_m;            /* b, a = -3 and 3 in Montgomery form (mod p) */
static fe e_inv_p, e_sqrt, e_legendre;  /* p - 2, (p + 1) / 4, (p - 1) / 2 */
static bool ready;

static void mont_mul(fe *r, const fe *a, const fe *b, const struct mod *md)
{
	u64 t[6] = { 0 };
	for (int i = 0; i < 4; i++) {
		u128 c = 0;
		for (int j = 0; j < 4; j++) {
			c += (u128)a->v[j] * b->v[i] + t[j];
			t[j] = (u64)c;
			c >>= 64;
		}
		c += t[4];
		t[4] = (u64)c;
		t[5] = (u64)(c >> 64);
		u64 q = t[0] * md->minv;
		c = (u128)q * md->m.v[0] + t[0];
		c >>= 64;
		for (int j = 1; j < 4; j++) {
			c += (u128)q * md->m.v[j] + t[j];
			t[j - 1] = (u64)c;
			c >>= 64;
		}
		c += t[4];
		t[3] = (u64)c;
		t[4] = t[5] + (u64)(c >> 64);
	}
	fe x = { { t[0], t[1], t[2], t[3] } }, y;
	u64 br = sub4(&y, &x, &md->m);
	/* x - m when x >= m (the carry word set, or no borrow) */
	sel(r, &x, &y, t[4] | (br ^ 1));
}

static void mod_add(fe *r, const fe *a, const fe *b, const struct mod *md)
{
	fe s, d;
	u64 c = add4(&s, a, b);
	u64 br = sub4(&d, &s, &md->m);
	sel(r, &s, &d, c | (br ^ 1));
}

static void mod_sub(fe *r, const fe *a, const fe *b, const struct mod *md)
{
	fe d, s;
	u64 br = sub4(&d, a, b);
	add4(&s, &d, &md->m);
	sel(r, &d, &s, br);
}

static void mod_init(struct mod *md, const u8 m[32])
{
	fe_from_bytes(&md->m, m);
	/* -m^-1 mod 2^64 by Newton's iteration */
	u64 inv = 1;
	for (int i = 0; i < 7; i++) inv *= 2 - md->m.v[0] * inv;
	md->minv = 0 - inv;
	/* R mod m = 2^256 - m (m > 2^255), then 256 doublings: R^2 mod m */
	fe zero = { { 0 } };
	sub4(&md->one, &zero, &md->m);
	fe r = md->one;
	for (int i = 0; i < 256; i++) mod_add(&r, &r, &r, md);
	md->r2 = r;
}

static void shr1(fe *r, const fe *a)
{
	for (int i = 0; i < 4; i++) r->v[i] = a->v[i] >> 1 | (i < 3 ? a->v[i + 1] << 63 : 0);
}

static void init(void)
{
	if (ready) return;
	mod_init(&P, p256_p);
	mod_init(&N, p256_n);
	fe t, one = { { 1 } }, two = { { 2 } };
	fe_from_bytes(&t, p256_b);
	mont_mul(&b_m, &t, &P.r2, &P);
	fe three = { { 3 } };
	mont_mul(&three_m, &three, &P.r2, &P);
	mod_sub(&a_m, &(fe){ { 0 } }, &three_m, &P);
	sub4(&e_inv_p, &P.m, &two);
	add4(&t, &P.m, &one);                   /* p + 1 < 2^256 */
	shr1(&t, &t);
	shr1(&e_sqrt, &t);
	sub4(&t, &P.m, &one);
	shr1(&e_legendre, &t);
	ready = true;
}

/* --- the field -------------------------------------------------------------- */

void fp_enc(fe *r, const fe *a) { init(); mont_mul(r, a, &P.r2, &P); }
void fp_dec(fe *r, const fe *a) { init(); mont_mul(r, a, &(fe){ { 1 } }, &P); }
void fp_add(fe *r, const fe *a, const fe *b) { init(); mod_add(r, a, b, &P); }
void fp_sub(fe *r, const fe *a, const fe *b) { init(); mod_sub(r, a, b, &P); }
void fp_mul(fe *r, const fe *a, const fe *b) { init(); mont_mul(r, a, b, &P); }
void fp_sqr(fe *r, const fe *a) { init(); mont_mul(r, a, a, &P); }

void fp_const(fe *r, u32 small)
{
	fe t = { { small } };
	fp_enc(r, &t);
}

void fp_pow(fe *r, const fe *a, const fe *e)
{
	init();
	fe x = P.one, b = *a;
	/* square and multiply from the top bit, always multiplying (the
	 * result kept or not by the bit) */
	for (int i = 255; i >= 0; i--) {
		fe t;
		mont_mul(&x, &x, &x, &P);
		mont_mul(&t, &x, &b, &P);
		sel(&x, &x, &t, e->v[i / 64] >> (i % 64));
	}
	*r = x;
}

void fp_inv(fe *r, const fe *a) { init(); fp_pow(r, a, &e_inv_p); }
void fp_sqrt(fe *r, const fe *a) { init(); fp_pow(r, a, &e_sqrt); }

int fp_legendre(const fe *a)
{
	init();
	fe t, m1;
	fp_pow(&t, a, &e_legendre);
	if (fe_is_zero(&t)) return 0;
	if (!fe_cmp(&t, &P.one)) return 1;
	mod_sub(&m1, &(fe){ { 0 } }, &P.one, &P);
	return !fe_cmp(&t, &m1) ? -1 : -2;
}

void fp_y2(fe *r, const fe *x)
{
	init();
	fe t, u;
	mont_mul(&t, x, x, &P);
	mont_mul(&t, &t, x, &P);               /* x^3 */
	mont_mul(&u, &a_m, x, &P);             /* -3x */
	mod_add(&t, &t, &u, &P);
	mod_add(r, &t, &b_m, &P);
}

void fp_reduce_bytes(fe *r, const u8 *b, u32 len)
{
	init();
	/* Horner over 8-byte pieces: r = r * 2^64 + piece, mod p, in plain
	 * numbers through the Montgomery form */
	fe acc = { { 0 } }, shift, w;
	fe two64 = { { 0, 1, 0, 0 } };
	fp_enc(&shift, &two64);
	u32 first = len % 8 ? len % 8 : 8;
	for (u32 pos = 0; pos < len;) {
		u32 k = pos ? 8 : first;
		u64 v = 0;
		for (u32 j = 0; j < k; j++) v = v << 8 | b[pos + j];
		pos += k;
		mont_mul(&acc, &acc, &shift, &P);
		fe pv = { { v } };
		fp_enc(&w, &pv);
		mod_add(&acc, &acc, &w, &P);
	}
	fp_dec(r, &acc);
}

/* --- scalars ------------------------------------------------------------------- */

void sn_add(fe *r, const fe *a, const fe *b) { init(); mod_add(r, a, b, &N); }

bool sn_valid(const fe *a)
{
	init();
	fe one = { { 1 } };
	return fe_cmp(a, &one) > 0 && fe_cmp(a, &N.m) < 0;
}

/* --- points ---------------------------------------------------------------------- */

void ecp_inf(struct ecp *r)
{
	init();
	r->x = P.one;
	r->y = P.one;
	mzero(&r->z, sizeof(r->z));
}

bool ecp_is_inf(const struct ecp *a) { return fe_is_zero(&a->z); }

void ecp_set_affine(struct ecp *r, const fe *x, const fe *y)
{
	init();
	r->x = *x;
	r->y = *y;
	r->z = P.one;
}

int ecp_from_xy(struct ecp *r, const u8 x[32], const u8 y[32])
{
	init();
	fe px, py, mx, my, lhs, rhs;
	fe_from_bytes(&px, x);
	fe_from_bytes(&py, y);
	if (fe_cmp(&px, &P.m) >= 0 || fe_cmp(&py, &P.m) >= 0) return -1;
	fp_enc(&mx, &px);
	fp_enc(&my, &py);
	fp_sqr(&lhs, &my);
	fp_y2(&rhs, &mx);
	if (fe_cmp(&lhs, &rhs)) return -1;
	ecp_set_affine(r, &mx, &my);
	return 0;
}

int ecp_to_xy(const struct ecp *a, u8 x[32], u8 y[32])
{
	init();
	if (ecp_is_inf(a)) return -1;
	fe zi, zi2, t;
	fp_inv(&zi, &a->z);
	fp_sqr(&zi2, &zi);
	fp_mul(&t, &a->x, &zi2);
	fp_dec(&t, &t);
	if (x) fe_to_bytes(x, &t);
	fp_mul(&zi2, &zi2, &zi);
	fp_mul(&t, &a->y, &zi2);
	fp_dec(&t, &t);
	if (y) fe_to_bytes(y, &t);
	return 0;
}

void ecp_dbl(struct ecp *r, const struct ecp *a)
{
	init();
	if (ecp_is_inf(a) || fe_is_zero(&a->y)) {
		ecp_inf(r);
		return;
	}
	/* dbl-2001-b: delta = Z1^2, gamma = Y1^2, beta = X1 gamma, alpha =
	 * 3 (X1 - delta)(X1 + delta); X3 = alpha^2 - 8 beta, Z3 = (Y1 + Z1)^2
	 * - gamma - delta, Y3 = alpha (4 beta - X3) - 8 gamma^2 */
	fe delta, gamma, beta, alpha, t, u, x3, y3, z3;
	fp_sqr(&delta, &a->z);
	fp_sqr(&gamma, &a->y);
	fp_mul(&beta, &a->x, &gamma);
	fp_sub(&t, &a->x, &delta);
	fp_add(&u, &a->x, &delta);
	fp_mul(&t, &t, &u);
	fp_mul(&alpha, &t, &three_m);
	fp_sqr(&x3, &alpha);
	fp_add(&t, &beta, &beta);              /* 2 beta */
	fp_add(&t, &t, &t);                     /* 4 beta */
	fp_add(&u, &t, &t);                     /* 8 beta */
	fp_sub(&x3, &x3, &u);
	fp_add(&z3, &a->y, &a->z);
	fp_sqr(&z3, &z3);
	fp_sub(&z3, &z3, &gamma);
	fp_sub(&z3, &z3, &delta);
	fp_sub(&t, &t, &x3);
	fp_mul(&y3, &alpha, &t);
	fp_sqr(&u, &gamma);
	fp_add(&u, &u, &u);
	fp_add(&u, &u, &u);
	fp_add(&u, &u, &u);                     /* 8 gamma^2 */
	fp_sub(&y3, &y3, &u);
	r->x = x3, r->y = y3, r->z = z3;
}

void ecp_add(struct ecp *r, const struct ecp *a, const struct ecp *b)
{
	init();
	if (ecp_is_inf(a)) { *r = *b; return; }
	if (ecp_is_inf(b)) { *r = *a; return; }
	/* add-2007-bl */
	fe z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t, x3, y3, z3;
	fp_sqr(&z1z1, &a->z);
	fp_sqr(&z2z2, &b->z);
	fp_mul(&u1, &a->x, &z2z2);
	fp_mul(&u2, &b->x, &z1z1);
	fp_mul(&s1, &a->y, &b->z);
	fp_mul(&s1, &s1, &z2z2);
	fp_mul(&s2, &b->y, &a->z);
	fp_mul(&s2, &s2, &z1z1);
	fp_sub(&h, &u2, &u1);
	fp_sub(&rr, &s2, &s1);
	if (fe_is_zero(&h)) {
		if (fe_is_zero(&rr)) ecp_dbl(r, a);
		else ecp_inf(r);
		return;
	}
	fp_add(&i, &h, &h);
	fp_sqr(&i, &i);
	fp_mul(&j, &h, &i);
	fp_add(&rr, &rr, &rr);
	fp_mul(&v, &u1, &i);
	fp_sqr(&x3, &rr);
	fp_sub(&x3, &x3, &j);
	fp_sub(&x3, &x3, &v);
	fp_sub(&x3, &x3, &v);
	fp_sub(&t, &v, &x3);
	fp_mul(&y3, &rr, &t);
	fp_mul(&t, &s1, &j);
	fp_add(&t, &t, &t);
	fp_sub(&y3, &y3, &t);
	fp_add(&z3, &a->z, &b->z);
	fp_sqr(&z3, &z3);
	fp_sub(&z3, &z3, &z1z1);
	fp_sub(&z3, &z3, &z2z2);
	fp_mul(&z3, &z3, &h);
	r->x = x3, r->y = y3, r->z = z3;
}

void ecp_neg(struct ecp *r, const struct ecp *a)
{
	init();
	*r = *a;
	fp_sub(&r->y, &(fe){ { 0 } }, &a->y);
}

static void cswap(struct ecp *a, struct ecp *b, u64 c)
{
	u64 m = 0 - (c & 1);
	u64 *x = (u64 *)a, *y = (u64 *)b;
	for (u32 i = 0; i < sizeof(*a) / 8; i++) {
		u64 d = (x[i] ^ y[i]) & m;
		x[i] ^= d;
		y[i] ^= d;
	}
}

void ecp_mul(struct ecp *r, const struct ecp *a, const u8 k[32])
{
	init();
	struct ecp r0, r1 = *a;
	ecp_inf(&r0);
	for (int i = 0; i < 256; i++) {
		u64 bit = k[i / 8] >> (7 - i % 8) & 1;
		cswap(&r0, &r1, bit);
		ecp_add(&r1, &r0, &r1);
		ecp_dbl(&r0, &r0);
		cswap(&r0, &r1, bit);
	}
	*r = r0;
	mzero(&r1, sizeof(r1));
}

void ecp_gen(struct ecp *g)
{
	ecp_from_xy(g, p256_gx, p256_gy);
}

bool ecp_eq(const struct ecp *a, const struct ecp *b)
{
	u8 ax[32], ay[32], bx[32], by[32];
	bool ia = ecp_to_xy(a, ax, ay) < 0, ib = ecp_to_xy(b, bx, by) < 0;
	if (ia || ib) return ia && ib;
	for (int i = 0; i < 32; i++)
		if (ax[i] != bx[i] || ay[i] != by[i]) return false;
	return true;
}
