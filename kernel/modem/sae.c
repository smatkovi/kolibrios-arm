/* KolibriOS for ARM: SAE for group 19 (sae.h), after hostapd 2.11's
 * src/common/sae.c and dragonfly.c (Copyright (c) 2012-2019 Jouni Malinen
 * and The Linux Foundation; BSD licence: see the README of hostapd). The
 * steps keep hostapd's order and its constant-time selections where the
 * password is involved; the numbers are ecc.c's. */
#include "sae.h"
#include "crypto.h"

#define K_MIN_ITER 40                    /* dragonfly_min_pwe_loop_iter for ECC groups */

/* --- small helpers ----------------------------------------------------------- */

/* x = mask ? b : a, bytewise without a branch (const_time_select_bin) */
static void csel(u8 *x, const u8 *a, const u8 *b, u32 n, u8 mask)
{
	for (u32 i = 0; i < n; i++) x[i] = (u8)((a[i] & ~mask) | (b[i] & mask));
}

/* big-endian a < b */
static bool lt(const u8 *a, const u8 *b, u32 n)
{
	for (u32 i = 0; i < n; i++)
		if (a[i] != b[i]) return a[i] < b[i];
	return false;
}

/* MAX(a, b) || MIN(a, b) (sae_pwd_seed_key, sae_max_min_addr) */
static void max_min(u8 o[12], const u8 *a, const u8 *b)
{
	bool a_hi = lt(b, a, 6);
	mcopy(o, a_hi ? a : b, 6);
	mcopy(o + 6, a_hi ? b : a, 6);
}

/* a random number below m (crypto_bignum_rand: rejection) */
static void rand_below(struct sae *s, u8 out[32], const u8 m[32])
{
	do s->random(out, 32);
	while (!lt(out, m, 32));
}

/* p - 1 and the like: big-endian constants derived once */
static u8 p_minus_1[32];

static void consts(void)
{
	static bool done;
	if (done) return;
	mcopy(p_minus_1, p256_p, 32);
	p_minus_1[31] -= 1;                    /* p ends with 0xff */
	done = true;
}

/* --- hunting and pecking --------------------------------------------------------- */

/* dragonfly_is_quadratic_residue_blind: 1 if val (Montgomery form) is a
 * residue, the test on val * r^2 * (qr or qnr by r's parity) */
static int qr_blind(struct sae *s, const fe *qr, const fe *qnr, const fe *val)
{
	u8 rb[32];
	rand_below(s, rb, p_minus_1);          /* [0, p - 2], then + 1: [1, p - 1] */
	fe r, rm, num, t;
	fe_from_bytes(&r, rb);
	/* r + 1 (r < p - 1: no carry past p) */
	u64 c = 1;
	for (int i = 0; i < 4; i++) {
		unsigned __int128 x = (unsigned __int128)r.v[i] + c;
		r.v[i] = (u64)x;
		c = (u64)(x >> 64);
	}
	fp_enc(&rm, &r);
	fp_mul(&num, val, &rm);
	fp_mul(&num, &num, &rm);
	u8 odd = (u8)(r.v[0] & 1), mask = (u8)(0 - odd);
	u8 a[32], b[32], q[32];
	fe_to_bytes(a, qnr);
	fe_to_bytes(b, qr);
	csel(q, a, b, 32, mask);               /* odd: qr, even: qnr */
	fe_from_bytes(&t, q);
	fp_mul(&num, &num, &t);
	int check = odd ? 1 : -1;
	int res = fp_legendre(&num);
	crypto_wipe(rb, sizeof(rb));
	crypto_wipe(&r, sizeof(r));
	crypto_wipe(&rm, sizeof(rm));
	return res == check;
}

int sae_pwe_hnp(struct sae *s, const u8 *pw, u32 pw_len)
{
	consts();
	if (pw_len > 128) return -1;
	/* a random residue and non-residue for the blinding
	 * (dragonfly_get_random_qr_qnr) */
	fe qr, qnr;
	bool have_qr = false, have_qnr = false;
	for (int i = 0; i < 1000 && !(have_qr && have_qnr); i++) {
		u8 b[32];
		fe t, tm;
		rand_below(s, b, p256_p);
		fe_from_bytes(&t, b);
		fp_enc(&tm, &t);
		int l = fp_legendre(&tm);
		if (l == 1 && !have_qr) qr = tm, have_qr = true;
		else if (l == -1 && !have_qnr) qnr = tm, have_qnr = true;
	}
	if (!have_qr || !have_qnr) return -1;
	u8 key[12], stub[128], tmp_pw[128];
	max_min(key, s->own, s->peer);
	s->random(stub, pw_len);
	u8 x[32], found = 0, seed_odd = 0;
	mzero(x, sizeof(x));
	for (u32 counter = 1; counter <= K_MIN_ITER || !found; counter++) {
		if (counter > 200) break;
		/* pwd-seed = H(MAX || MIN, base || counter); after the find a
		 * stub password, for the same work */
		csel(tmp_pw, pw, stub, pw_len, found);
		u8 ctr = (u8)counter, seed[SHA256_LEN], val[32];
		const u8 *v[2] = { tmp_pw, &ctr };
		u32 l[2] = { pw_len, 1 };
		hmac_sha256_vec(key, 12, 2, v, l, seed);
		/* pwd-value = KDF-256(pwd-seed, "SAE Hunting and Pecking", p) */
		sha256_kdf_bits(seed, SHA256_LEN, "SAE Hunting and Pecking", p256_p, 32, val, 256);
		u8 in_range = (u8)(0 - (u8)lt(val, p256_p, 32));
		fe xc, xm, y2;
		fe_from_bytes(&xc, val);
		fp_enc(&xm, &xc);
		fp_y2(&y2, &xm);
		u8 res = (u8)(qr_blind(s, &qr, &qnr, &y2) ? 0xff : 0) & in_range;
		/* the first find kept */
		u8 take = res & (u8)~found;
		csel(x, x, val, 32, take);
		seed_odd = (u8)((seed_odd & ~take) | ((seed[SHA256_LEN - 1] & 1) & take));
		found |= res;
		crypto_wipe(seed, sizeof(seed));
		crypto_wipe(val, sizeof(val));
	}
	crypto_wipe(tmp_pw, sizeof(tmp_pw));
	crypto_wipe(stub, sizeof(stub));
	if (!found) return -1;
	/* y = sqrt(x^3 + ax + b); the one whose parity is the seed's */
	fe xp, xm, y2, y, yp, ny;
	fe_from_bytes(&xp, x);
	fp_enc(&xm, &xp);
	fp_y2(&y2, &xm);
	fp_sqrt(&y, &y2);
	fp_dec(&yp, &y);
	u8 yb[32], nyb[32], ys[32];
	fe_to_bytes(yb, &yp);
	fe pf;
	fe_from_bytes(&pf, p256_p);
	fe zero = { { 0 } };
	fp_sub(&ny, &zero, &y);                /* p - y, Montgomery */
	fp_dec(&ny, &ny);
	fe_to_bytes(nyb, &ny);
	u8 eq = (u8)(0 - (u8)((yb[31] & 1) == seed_odd));
	csel(ys, nyb, yb, 32, eq);
	int r = ecp_from_xy(&s->pwe, x, ys);
	crypto_wipe(x, sizeof(x));
	return r;
}

/* --- hash-to-element ------------------------------------------------------------- */

/* sswu (RFC 9380's simplified SWU as IEEE 802.11-2020 12.4.4.2.3 has it,
 * z = -10 for group 19); u a plain number < p */
static void sswu(struct ecp *out, const fe *u_plain)
{
	fe u, u2, t1, t2, z, t, a, b, x1a, x1b, x1, gx1, x2, gx2, v, x, y, one, zero = { { 0 } }, ten;
	fp_enc(&u, u_plain);
	fp_const(&ten, 10);
	fp_sub(&z, &zero, &ten);               /* z = -10 */
	fp_const(&one, 1);
	fp_const(&t, 3);
	fp_sub(&a, &zero, &t);                 /* a = -3 */
	fe bp;
	fe_from_bytes(&bp, p256_b);
	fp_enc(&b, &bp);
	/* m = z^2 u^4 + z u^2: t1 = z u^2, m = t1^2 + t1 */
	fp_sqr(&u2, &u);
	fp_mul(&t1, &z, &u2);
	fp_sqr(&t2, &t1);
	fp_add(&t1, &t1, &t2);
	u64 m_zero = fe_is_zero(&t1);
	/* t = m^(p-2) (0 for 0) */
	fp_inv(&t, &t1);
	/* x1a = b / (z a); x1b = (-b / a)(1 + t) */
	fp_mul(&t1, &z, &a);
	fp_inv(&t1, &t1);
	fp_mul(&x1a, &b, &t1);
	fp_sub(&t1, &zero, &b);
	fp_inv(&t2, &a);
	fp_mul(&t1, &t1, &t2);
	fp_add(&t2, &one, &t);
	fp_mul(&x1b, &t1, &t2);
	u8 ba[32], bb[32], bx[32];
	fe_to_bytes(ba, &x1a);
	fe_to_bytes(bb, &x1b);
	csel(bx, bb, ba, 32, (u8)(0 - (u8)m_zero));
	fe_from_bytes(&x1, bx);
	fp_y2(&gx1, &x1);
	/* x2 = z u^2 x1, gx2 = x2^3 + a x2 + b */
	fp_mul(&t1, &z, &u2);
	fp_mul(&x2, &t1, &x1);
	fp_y2(&gx2, &x2);
	/* gx1 a residue (or 0)? */
	int l = fp_legendre(&gx1);
	u8 is_qr = (u8)(0 - (u8)(l >= 0));
	fe_to_bytes(ba, &gx1);
	fe_to_bytes(bb, &gx2);
	csel(bx, bb, ba, 32, is_qr);
	fe_from_bytes(&v, bx);
	fe_to_bytes(ba, &x1);
	fe_to_bytes(bb, &x2);
	csel(bx, bb, ba, 32, is_qr);
	fe_from_bytes(&x, bx);
	fp_sqrt(&y, &v);
	/* the y with u's parity (both as plain numbers) */
	fe yp, ny, nyp;
	fp_dec(&yp, &y);
	fp_sub(&ny, &zero, &y);
	fp_dec(&nyp, &ny);
	u8 ub[32], yb[32], nyb[32], ys[32];
	fe_to_bytes(ub, u_plain);
	fe_to_bytes(yb, &yp);
	fe_to_bytes(nyb, &nyp);
	csel(ys, nyb, yb, 32, (u8)(0 - (u8)((ub[31] & 1) == (yb[31] & 1))));
	fe yf, ym;
	fe_from_bytes(&yf, ys);
	fp_enc(&ym, &yf);
	ecp_set_affine(out, &x, &ym);
}

int sae_pt(u8 pt[64], const u8 *ssid, u32 ssid_len, const u8 *pw, u32 pw_len, const char *id)
{
	/* pwd-seed = HKDF-Extract(ssid, password [|| identifier]) */
	u8 seed[SHA256_LEN], val[48];
	const u8 *v[2] = { pw, (const u8 *)id };
	u32 l[2] = { pw_len, id ? mstrlen(id) : 0 };
	hmac_sha256_vec(ssid, ssid_len, id ? 2 : 1, v, l, seed);
	struct ecp p1, p2, r;
	static const char *const info[2] = { "SAE Hash to Element u1 P1", "SAE Hash to Element u2 P2" };
	for (int i = 0; i < 2; i++) {
		/* pwd-value = HKDF-Expand(pwd-seed, info, 48); u = pwd-value mod p */
		hkdf_expand_sha256(seed, SHA256_LEN, (const u8 *)info[i], mstrlen(info[i]), val, 48);
		fe u;
		fp_reduce_bytes(&u, val, 48);
		sswu(i ? &p2 : &p1, &u);
	}
	ecp_add(&r, &p1, &p2);
	crypto_wipe(seed, sizeof(seed));
	crypto_wipe(val, sizeof(val));
	return ecp_to_xy(&r, pt, pt + 32);
}

int sae_pwe_from_pt(struct sae *s, const u8 pt[64])
{
	/* val = H(0^32, MAX || MIN) mod (n - 1) + 1; PWE = val * PT */
	u8 addrs[12], zero[32], h[SHA256_LEN];
	max_min(addrs, s->own, s->peer);
	mzero(zero, sizeof(zero));
	hmac_sha256(zero, 32, addrs, 12, h);
	fe v, nm1, n;
	fe_from_bytes(&v, h);
	fe_from_bytes(&n, p256_n);
	nm1 = n;
	nm1.v[0] -= 1;                          /* n is odd */
	if (fe_cmp(&v, &nm1) >= 0) {
		/* one subtraction: v < 2^256 < 2 (n - 1) */
		u64 br = 0;
		for (int i = 0; i < 4; i++) {
			unsigned __int128 d = (unsigned __int128)v.v[i] - nm1.v[i] - br;
			v.v[i] = (u64)d;
			br = (u64)(d >> 64) & 1;
		}
	}
	fe one = { { 1 } };
	unsigned __int128 c = (unsigned __int128)v.v[0] + one.v[0];
	v.v[0] = (u64)c;
	for (int i = 1; i < 4 && (c >> 64); i++) {
		c = (unsigned __int128)v.v[i] + 1;
		v.v[i] = (u64)c;
	}
	u8 vb[32];
	fe_to_bytes(vb, &v);
	struct ecp p;
	if (ecp_from_xy(&p, pt, pt + 32)) return -1;
	ecp_mul(&s->pwe, &p, vb);
	s->h2e = true;
	return ecp_is_inf(&s->pwe) ? -1 : 0;
}

/* --- commit and confirm ---------------------------------------------------------- */

int sae_commit(struct sae *s, const u8 *rand, const u8 *mask)
{
	u8 m[32];
	fe r, mk, sc;
	for (int tries = 0; tries < 100; tries++) {
		/* 1 < rand, mask < r; rand + mask mod r > 1 (dragonfly_generate_scalar) */
		if (rand && mask) {
			mcopy(s->rand, rand, 32);
			mcopy(m, mask, 32);
		} else {
			rand_below(s, s->rand, p256_n);
			rand_below(s, m, p256_n);
		}
		fe_from_bytes(&r, s->rand);
		fe_from_bytes(&mk, m);
		fe one = { { 1 } };
		if ((fe_cmp(&r, &one) <= 0 || fe_cmp(&mk, &one) <= 0) && !(rand && mask)) continue;
		sn_add(&sc, &r, &mk);
		if (!sn_valid(&sc) && !(rand && mask)) continue;
		fe_to_bytes(s->scalar, &sc);
		/* COMMIT-ELEMENT = inverse(scalar-op(mask, PWE)) */
		struct ecp e;
		ecp_mul(&e, &s->pwe, m);
		ecp_neg(&e, &e);
		crypto_wipe(m, sizeof(m));
		return ecp_to_xy(&e, s->elem, s->elem + 32);
	}
	return -1;
}

u32 sae_write_commit(const struct sae *s, u8 *o)
{
	u32 n = 0;
	put16(o, SAE_GROUP);
	n = 2;
	if (!s->h2e && s->token_len) {
		mcopy(o + n, s->token, s->token_len);
		n += s->token_len;
	}
	mcopy(o + n, s->scalar, 32);
	mcopy(o + n + 32, s->elem, 64);
	n += 96;
	if (s->h2e && s->token_len) {
		/* the Anti-Clogging Token Container (an extension element, 93) */
		o[n++] = 255;
		o[n++] = (u8)(1 + s->token_len);
		o[n++] = 93;
		mcopy(o + n, s->token, s->token_len);
		n += s->token_len;
	}
	return n;
}

int sae_take_token(struct sae *s, const u8 *b, u32 len)
{
	/* the group, then the token (hash-to-element: in its container) */
	if (len < 2 || get16(b) != SAE_GROUP) return -1;
	const u8 *t = b + 2;
	u32 tl = len - 2;
	if (s->h2e) {
		if (tl < 3 || t[0] != 255 || !t[1] || t[1] > tl - 2 || t[2] != 93) return -1;
		tl = t[1] - 1u;
		t += 3;
	}
	if (!tl || tl > sizeof(s->token)) return -1;
	mcopy(s->token, t, tl);
	s->token_len = tl;
	return 0;
}

u16 sae_parse_commit(struct sae *s, const u8 *b, u32 len)
{
	if (len < 2) return 1;
	if (get16(b) != SAE_GROUP) return SAE_STATUS_GROUP_UNSUPPORTED;
	if (len < SAE_COMMIT_LEN) return 1;
	/* the scalar: 1 < scalar < r */
	fe sc;
	fe_from_bytes(&sc, b + 2);
	if (!sn_valid(&sc)) return 1;
	/* the element: on the curve (ecp_from_xy: both coordinates < p) */
	struct ecp e;
	if (ecp_from_xy(&e, b + 34, b + 66)) return 1;
	/* (a password identifier, rejected groups, a token container, an
	 * AKM suite selector may follow: not used by a station here) */
	mcopy(s->peer_scalar, b + 2, 32);
	mcopy(s->peer_elem, b + 34, 64);
	/* a reflection: our own scalar and element back */
	if (!crypto_memcmp(s->peer_scalar, s->scalar, 32) && !crypto_memcmp(s->peer_elem, s->elem, 64)) return SAE_SILENT;
	return 0;
}

int sae_process(struct sae *s)
{
	/* K = scalar-op(rand, elem-op(scalar-op(peer-commit-scalar, PWE),
	 * PEER-COMMIT-ELEMENT)); not the identity; k = K's x */
	struct ecp t, pe;
	if (ecp_from_xy(&pe, s->peer_elem, s->peer_elem + 32)) return -1;
	ecp_mul(&t, &s->pwe, s->peer_scalar);
	ecp_add(&t, &t, &pe);
	ecp_mul(&t, &t, s->rand);
	u8 k[32];
	if (ecp_to_xy(&t, k, NULL)) return -1;
	/* keyseed = H(0^32, k); KCK || PMK = KDF-512(keyseed, "SAE KCK and
	 * PMK", (commit-scalar + peer-commit-scalar) mod r); PMKID = its
	 * first 16 octets (hash-to-element without rejected groups: the
	 * same zero salt) */
	u8 zero[32], keyseed[SHA256_LEN], val[32], keys[64];
	mzero(zero, sizeof(zero));
	hmac_sha256(zero, 32, k, 32, keyseed);
	fe a, b2, sum;
	fe_from_bytes(&a, s->scalar);
	fe_from_bytes(&b2, s->peer_scalar);
	sn_add(&sum, &a, &b2);
	fe_to_bytes(val, &sum);
	sha256_kdf_bits(keyseed, SHA256_LEN, "SAE KCK and PMK", val, 32, keys, 512);
	mcopy(s->kck, keys, 32);
	mcopy(s->pmk, keys + 32, 32);
	mcopy(s->pmkid, val, 16);
	crypto_wipe(k, sizeof(k));
	crypto_wipe(keyseed, sizeof(keyseed));
	crypto_wipe(keys, sizeof(keys));
	return 0;
}

/* CN(KCK, send-confirm, scalar1, element1, scalar2, element2) */
static void cn(const struct sae *s, const u8 sc[2], const u8 *s1, const u8 *e1, const u8 *s2, const u8 *e2, u8 out[32])
{
	const u8 *v[5] = { sc, s1, e1, s2, e2 };
	u32 l[5] = { 2, 32, 64, 32, 64 };
	hmac_sha256_vec(s->kck, SAE_KCK_LEN, 5, v, l, out);
}

u32 sae_write_confirm(struct sae *s, u8 *o)
{
	if (s->send_confirm < 0xffff) s->send_confirm++;
	put16(o, s->send_confirm);
	cn(s, o, s->scalar, s->elem, s->peer_scalar, s->peer_elem, o + 2);
	return SAE_CONFIRM_LEN;
}

int sae_check_confirm(const struct sae *s, const u8 *b, u32 len)
{
	if (len < SAE_CONFIRM_LEN) return -1;
	u8 v[32];
	cn(s, b, s->peer_scalar, s->peer_elem, s->scalar, s->elem, v);
	int r = crypto_memcmp(v, b + 2, 32) ? -1 : 0;
	crypto_wipe(v, sizeof(v));
	return r;
}
