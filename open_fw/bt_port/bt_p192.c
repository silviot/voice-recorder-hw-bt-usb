/* NIST P-192 key generation (see bt_p192.h). Field elements are 6 little-endian 32-bit limbs; reduction mod
 * p = 2^192 - 2^64 - 1 uses the NIST fast reduction (FIPS 186-4 D.2.1). Points are Jacobian, a = -3.
 * SPDX-License-Identifier: MIT */
#include "bt_p192.h"

#include <string.h>

typedef uint32_t fe_t[6];
typedef struct {
    fe_t x, y, z;
} jac_t;

static const fe_t P = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
static const fe_t N = { 0xB4D22831u, 0x146BC9B1u, 0x99DEF836u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
static const fe_t B = { 0xC146B9B1u, 0xFEB8DEECu, 0x72243049u, 0x0FA7E9ABu, 0xE59C80E7u, 0x64210519u };
static const fe_t GX = { 0x82FF1012u, 0xF4FF0AFDu, 0x43A18800u, 0x7CBF20EBu, 0xB03090F6u, 0x188DA80Eu };
static const fe_t GY = { 0x1E794811u, 0x73F977A1u, 0x6B24CDD5u, 0x631011EDu, 0xFFC8DA78u, 0x07192B95u };

static void wipe(void *p, size_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    while (n--) {
        *b++ = 0u;
    }
}

static void fe_from_le(fe_t r, const uint8_t *b)
{
    for (int i = 0; i < 6; i++) {
        r[i] = (uint32_t)b[4 * i] | ((uint32_t)b[4 * i + 1] << 8) | ((uint32_t)b[4 * i + 2] << 16) |
               ((uint32_t)b[4 * i + 3] << 24);
    }
}

static void fe_to_le(uint8_t *b, const fe_t a)
{
    for (int i = 0; i < 6; i++) {
        b[4 * i] = (uint8_t)a[i];
        b[4 * i + 1] = (uint8_t)(a[i] >> 8);
        b[4 * i + 2] = (uint8_t)(a[i] >> 16);
        b[4 * i + 3] = (uint8_t)(a[i] >> 24);
    }
}

/* a < m (variable time: only used on public values and on the keygen candidate's range test) */
static int lt(const fe_t a, const fe_t m)
{
    for (int i = 5; i >= 0; i--) {
        if (a[i] != m[i]) {
            return a[i] < m[i];
        }
    }
    return 0;
}

static int is_zero(const fe_t a)
{
    uint32_t acc = 0u;
    for (int i = 0; i < 6; i++) {
        acc |= a[i];
    }
    return acc == 0u;
}

/* r = mask ? a : r, mask all-ones or zero */
static void cmov(uint32_t *r, const uint32_t *a, size_t words, uint32_t mask)
{
    for (size_t i = 0; i < words; i++) {
        r[i] ^= mask & (r[i] ^ a[i]);
    }
}

/* r = a - P if a (with carry-in hi) >= P, constant time. Requires the true value < 2P. */
static void fe_cond_sub_p(fe_t r, const fe_t a, uint32_t hi)
{
    fe_t t;
    uint64_t borrow = 0u;
    for (int i = 0; i < 6; i++) {
        uint64_t d = (uint64_t)a[i] - P[i] - borrow;
        t[i] = (uint32_t)d;
        borrow = (d >> 32) & 1u;
    }
    /* keep t when hi != 0 (value >= 2^192 > P) or no borrow (a >= P) */
    uint32_t use_t = (uint32_t)0u - (uint32_t)((hi != 0u) | (borrow == 0u));
    for (int i = 0; i < 6; i++) {
        r[i] = a[i];
    }
    cmov(r, t, 6u, use_t);
}

static void fe_add(fe_t r, const fe_t a, const fe_t b)
{
    fe_t s;
    uint64_t c = 0u;
    for (int i = 0; i < 6; i++) {
        c += (uint64_t)a[i] + b[i];
        s[i] = (uint32_t)c;
        c >>= 32;
    }
    fe_cond_sub_p(r, s, (uint32_t)c);
}

static void fe_sub(fe_t r, const fe_t a, const fe_t b)
{
    fe_t d, t;
    uint64_t borrow = 0u;
    for (int i = 0; i < 6; i++) {
        uint64_t x = (uint64_t)a[i] - b[i] - borrow;
        d[i] = (uint32_t)x;
        borrow = (x >> 32) & 1u;
    }
    uint64_t c = 0u;
    for (int i = 0; i < 6; i++) {
        c += (uint64_t)d[i] + P[i];
        t[i] = (uint32_t)c;
        c >>= 32;
    }
    uint32_t mask = (uint32_t)0u - (uint32_t)borrow;  /* a < b: add P back */
    for (int i = 0; i < 6; i++) {
        r[i] = d[i];
    }
    cmov(r, t, 6u, mask);
}

/* FIPS 186-4 D.2.1: with 64-bit chunks c0..c5 of the 384-bit product,
 * r = (c2,c1,c0) + (0,c3,c3) + (c4,c4,0) + (c5,c5,c5) mod p. */
static void fe_reduce(fe_t r, const uint32_t t[12])
{
    uint32_t s[6];
    uint64_t acc = 0u;
    acc += (uint64_t)t[0] + t[6] + t[10];
    s[0] = (uint32_t)acc; acc >>= 32;
    acc += (uint64_t)t[1] + t[7] + t[11];
    s[1] = (uint32_t)acc; acc >>= 32;
    acc += (uint64_t)t[2] + t[6] + t[8] + t[10];
    s[2] = (uint32_t)acc; acc >>= 32;
    acc += (uint64_t)t[3] + t[7] + t[9] + t[11];
    s[3] = (uint32_t)acc; acc >>= 32;
    acc += (uint64_t)t[4] + t[8] + t[10];
    s[4] = (uint32_t)acc; acc >>= 32;
    acc += (uint64_t)t[5] + t[9] + t[11];
    s[5] = (uint32_t)acc; acc >>= 32;
    /* fold the carry twice: carry * 2^192 == carry * (2^64 + 1) (mod p). After the second fold the carry is 0. */
    for (int k = 0; k < 2; k++) {
        uint32_t carry = (uint32_t)acc;
        acc = (uint64_t)s[0] + carry;
        s[0] = (uint32_t)acc; acc >>= 32;
        acc += s[1];
        s[1] = (uint32_t)acc; acc >>= 32;
        acc += (uint64_t)s[2] + carry;
        s[2] = (uint32_t)acc; acc >>= 32;
        for (int i = 3; i < 6; i++) {
            acc += s[i];
            s[i] = (uint32_t)acc;
            acc >>= 32;
        }
    }
    fe_cond_sub_p(r, s, (uint32_t)acc);
}

static void fe_mul(fe_t r, const fe_t a, const fe_t b)
{
    uint32_t t[12];
    for (int i = 0; i < 12; i++) {
        t[i] = 0u;
    }
    for (int i = 0; i < 6; i++) {
        uint64_t c = 0u;
        for (int j = 0; j < 6; j++) {
            c += (uint64_t)a[i] * b[j] + t[i + j];
            t[i + j] = (uint32_t)c;
            c >>= 32;
        }
        t[i + 6] = (uint32_t)c;
    }
    fe_reduce(r, t);
}

static void fe_sqr(fe_t r, const fe_t a) { fe_mul(r, a, a); }

/* r = a^(p-2) (Fermat; the exponent is public) */
static void fe_inv(fe_t r, const fe_t a)
{
    static const fe_t E = { 0xFFFFFFFDu, 0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    fe_t x = { 1u, 0u, 0u, 0u, 0u, 0u };
    for (int i = 191; i >= 0; i--) {
        fe_sqr(x, x);
        if ((E[i / 32] >> (i % 32)) & 1u) {
            fe_mul(x, x, a);
        }
    }
    memcpy(r, x, sizeof(fe_t));
}

/* dbl-2001-b (a = -3). Infinity (Z = 0) maps to infinity. */
static void jac_dbl(jac_t *r, const jac_t *p)
{
    fe_t delta, gamma, beta, alpha, t1, t2;
    fe_sqr(delta, p->z);
    fe_sqr(gamma, p->y);
    fe_mul(beta, p->x, gamma);
    fe_sub(t1, p->x, delta);
    fe_add(t2, p->x, delta);
    fe_mul(alpha, t1, t2);
    fe_add(t1, alpha, alpha);
    fe_add(alpha, t1, alpha);              /* alpha = 3 (X - delta)(X + delta) */
    fe_add(t1, p->y, p->z);
    fe_sqr(t1, t1);
    fe_sub(t1, t1, gamma);
    fe_sub(r->z, t1, delta);               /* Z3 = (Y + Z)^2 - gamma - delta */
    fe_add(beta, beta, beta);
    fe_add(beta, beta, beta);              /* 4 beta */
    fe_sqr(t1, alpha);
    fe_add(t2, beta, beta);
    fe_sub(r->x, t1, t2);                  /* X3 = alpha^2 - 8 beta */
    fe_sub(t1, beta, r->x);
    fe_mul(t1, alpha, t1);
    fe_sqr(gamma, gamma);
    fe_add(gamma, gamma, gamma);
    fe_add(gamma, gamma, gamma);
    fe_add(gamma, gamma, gamma);           /* 8 gamma^2 */
    fe_sub(r->y, t1, gamma);
}

/* madd-2007-bl: r = p + (qx, qy) with q affine. Infinity input -> q (constant-time select); p == -q -> infinity
 * (H = 0 gives Z3 = 0 by itself: reached by d = n - 1 in its unused last addition, test vector); p == q -> doubling
 * (branch kept for completeness: the ladder's doubled prefix 2 * (d >> j) equals n + 1 only for d >= n). */
static void jac_madd(jac_t *r, const jac_t *p, const fe_t qx, const fe_t qy)
{
    fe_t z1z1, u2, s2, h, hh, i, j, rr, v, t;
    jac_t out;
    fe_sqr(z1z1, p->z);
    fe_mul(u2, qx, z1z1);
    fe_mul(s2, qy, p->z);
    fe_mul(s2, s2, z1z1);
    fe_sub(h, u2, p->x);
    fe_sub(rr, s2, p->y);
    if (is_zero(h) && is_zero(rr) && !is_zero(p->z)) {
        jac_dbl(r, p);
        return;
    }
    fe_add(rr, rr, rr);
    fe_sqr(hh, h);
    fe_add(i, hh, hh);
    fe_add(i, i, i);
    fe_mul(j, h, i);
    fe_mul(v, p->x, i);
    fe_sqr(t, rr);
    fe_sub(t, t, j);
    fe_sub(t, t, v);
    fe_sub(out.x, t, v);                   /* X3 = r^2 - J - 2V */
    fe_sub(t, v, out.x);
    fe_mul(t, rr, t);
    fe_mul(u2, p->y, j);
    fe_add(u2, u2, u2);
    fe_sub(out.y, t, u2);                  /* Y3 = r (V - X3) - 2 Y1 J */
    fe_add(t, p->z, h);
    fe_sqr(t, t);
    fe_sub(t, t, z1z1);
    fe_sub(out.z, t, hh);                  /* Z3 = (Z1 + H)^2 - Z1Z1 - HH */
    jac_t q;
    memcpy(q.x, qx, sizeof(fe_t));
    memcpy(q.y, qy, sizeof(fe_t));
    fe_t one = { 1u, 0u, 0u, 0u, 0u, 0u };
    memcpy(q.z, one, sizeof(fe_t));
    cmov((uint32_t *)&out, (const uint32_t *)&q, 18u, (uint32_t)0u - (uint32_t)is_zero(p->z));
    *r = out;
    wipe(&out, sizeof(out));
}

static int on_curve(const fe_t x, const fe_t y)
{
    fe_t l, r, t;
    fe_sqr(l, y);
    fe_sqr(r, x);
    fe_mul(r, r, x);                       /* x^3 */
    fe_add(t, x, x);
    fe_add(t, t, x);                       /* 3x */
    fe_sub(r, r, t);
    fe_add(r, r, B);
    return memcmp(l, r, sizeof(fe_t)) == 0;
}

int bt_p192_on_curve(const uint8_t pub_le[48])
{
    fe_t x, y;
    fe_from_le(x, pub_le);
    fe_from_le(y, pub_le + 24);
    return lt(x, P) && lt(y, P) && on_curve(x, y);
}

static int scalar_ok(const fe_t d)
{
    return !is_zero(d) && lt(d, N);
}

int bt_p192_pubkey(const uint8_t d_le[24], uint8_t pub_le[48])
{
    fe_t d, zi, zi2, x, y;
    jac_t r, t;
    int rc = -1;
    fe_from_le(d, d_le);
    if (!scalar_ok(d)) {
        goto out;
    }
    memset(&r, 0, sizeof(r));              /* infinity: Z = 0 (X, Y unused) */
    r.x[0] = 1u;
    r.y[0] = 1u;
    for (int i = 191; i >= 0; i--) {
        jac_dbl(&r, &r);
        jac_madd(&t, &r, GX, GY);
        uint32_t bit = (d[i / 32] >> (i % 32)) & 1u;
        cmov((uint32_t *)&r, (const uint32_t *)&t, 18u, (uint32_t)0u - bit);
    }
    if (is_zero(r.z)) {
        goto out;
    }
    fe_inv(zi, r.z);
    fe_sqr(zi2, zi);
    fe_mul(x, r.x, zi2);
    fe_mul(zi2, zi2, zi);
    fe_mul(y, r.y, zi2);
    if (!on_curve(x, y)) {                 /* fault / miscompile guard: never hand the ROM a bad key */
        goto out;
    }
    fe_to_le(pub_le, x);
    fe_to_le(pub_le + 24, y);
    rc = 0;
out:
    if (rc != 0) {
        wipe(pub_le, 48u);
    }
    wipe(d, sizeof(d));
    wipe(&r, sizeof(r));
    wipe(&t, sizeof(t));
    wipe(zi, sizeof(zi));
    wipe(zi2, sizeof(zi2));
    return rc;
}

int bt_p192_keygen(int (*rng)(void *ctx, void *out, size_t n), void *ctx, uint8_t d_le[24], uint8_t pub_le[48])
{
    if (rng == NULL || d_le == NULL || pub_le == NULL) {
        return -1;
    }
    for (int tries = 0; tries < 8; tries++) {
        if (rng(ctx, d_le, BT_P192_BYTES) != 0) {
            break;                         /* fail closed */
        }
        fe_t c;
        fe_from_le(c, d_le);
        int ok = scalar_ok(c);
        wipe(c, sizeof(c));
        if (ok) {
            if (bt_p192_pubkey(d_le, pub_le) == 0) {
                return 0;
            }
            break;
        }
    }
    wipe(d_le, BT_P192_BYTES);
    wipe(pub_le, 48u);
    return -1;
}
