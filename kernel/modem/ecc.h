/* KolibriOS for ARM: the elliptic curve SAE uses (ecc.c;
 * docs/pro1x-network.md, WPA3): NIST P-256 (FIPS 186-4 D.1.2.3, SEC 2
 * secp256r1), IKE group 19, the group WPA3-Personal requires
 * (wpa_supplicant's default sae_groups 19, 20, 21; 19 mandatory).
 *
 * Field elements and scalars are 256-bit numbers in four 64-bit limbs,
 * least significant first; field arithmetic in Montgomery form (R =
 * 2^256), points in Jacobian coordinates (Z = 0: the point at infinity).
 * The constants are checked against OpenSSL's prime256v1 by tests/wifi. */
#ifndef ECC_H
#define ECC_H
#include "modem.h"

typedef struct { u64 v[4]; } fe;
struct ecp { fe x, y, z; };                 /* Jacobian, Montgomery form */

/* the curve's numbers, big endian */
extern const u8 p256_p[32], p256_n[32], p256_b[32], p256_gx[32], p256_gy[32];

/* 32 bytes big endian <-> a number (not reduced, not in Montgomery form) */
void fe_from_bytes(fe *a, const u8 b[32]);
void fe_to_bytes(u8 b[32], const fe *a);
int fe_cmp(const fe *a, const fe *b);      /* -1, 0, 1 */
bool fe_is_zero(const fe *a);

/* the field (mod p), the values in Montgomery form */
void fp_enc(fe *r, const fe *a);           /* a < p into the form */
void fp_dec(fe *r, const fe *a);           /* back */
void fp_add(fe *r, const fe *a, const fe *b);
void fp_sub(fe *r, const fe *a, const fe *b);
void fp_mul(fe *r, const fe *a, const fe *b);
void fp_sqr(fe *r, const fe *a);
void fp_pow(fe *r, const fe *a, const fe *e);   /* e: a plain number */
void fp_inv(fe *r, const fe *a);           /* a^(p-2): 0 for 0 */
void fp_sqrt(fe *r, const fe *a);          /* a^((p+1)/4): the root when there is one */
int fp_legendre(const fe *a);              /* 1, 0, -1 (Euler's criterion) */
void fp_y2(fe *r, const fe *x);            /* x^3 - 3x + b */
void fp_const(fe *r, u32 small);           /* a small number into the form */
/* a big-endian number of up to 64 bytes modulo p (plain) */
void fp_reduce_bytes(fe *r, const u8 *b, u32 len);

/* scalars (mod n), plain numbers */
void sn_add(fe *r, const fe *a, const fe *b);   /* (a + b) mod n, a and b < n */
bool sn_valid(const fe *a);                /* 1 < a < n */

/* points */
void ecp_inf(struct ecp *r);
bool ecp_is_inf(const struct ecp *a);
/* from affine coordinates (big endian): -1 unless both < p and on the curve */
int ecp_from_xy(struct ecp *r, const u8 x[32], const u8 y[32]);
/* to affine (big endian): -1 for the point at infinity */
int ecp_to_xy(const struct ecp *a, u8 x[32], u8 y[32]);
void ecp_set_affine(struct ecp *r, const fe *x, const fe *y);   /* Montgomery x, y */
void ecp_add(struct ecp *r, const struct ecp *a, const struct ecp *b);
void ecp_dbl(struct ecp *r, const struct ecp *a);
void ecp_neg(struct ecp *r, const struct ecp *a);
/* k * a, k a big-endian 32-byte number: a Montgomery ladder, 256 steps */
void ecp_mul(struct ecp *r, const struct ecp *a, const u8 k[32]);
void ecp_gen(struct ecp *g);               /* the base point */
bool ecp_eq(const struct ecp *a, const struct ecp *b);

#endif
