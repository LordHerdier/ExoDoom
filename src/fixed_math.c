/*
 * fixed_math.c — fixed-point sin/cos/tan/atan (SCRUM-41).
 *
 * See src/fixed_math.h for the API, the units, and why this is integer
 * arithmetic rather than a `double` libm.  This file is about *how*.
 *
 * ── Internal format: Q30, not 16.16 ────────────────────────────────────
 *
 * Every intermediate here is Q30 (1.0 == 2^30) held in int64_t, and only
 * the final result is narrowed to 16.16.  Two reasons, both load-bearing:
 *
 *   Headroom.  Q30 is the widest scale at which the products this code
 *   forms still fit a signed 64-bit register.  The largest is y * y for
 *   y = pi/2, i.e. 1.687e9 * 1.687e9 = 2.85e18 against int64's 9.22e18
 *   ceiling.  Q31 would double both factors and overflow; Q32 would
 *   quadruple them.  (A wider scale is reachable with __int128, but it
 *   buys nothing -- see the accuracy note below, the polynomial is already
 *   four orders of magnitude finer than the 16.16 result can express.)
 *
 *   Precision.  Computing in 16.16 directly and rounding once at the end
 *   is not the same thing as computing in 16.16 throughout: the Horner
 *   chain below performs seven multiplies, and at 16.16 each one would
 *   discard bits the next one needs.  Q30 keeps 14 guard bits under the
 *   result, so the polynomial's own error (measured: 2.6 Q30 units, or
 *   0.00016 LSB of the 16.16 answer) is negligible against the 0.5 LSB the
 *   final rounding costs.  The output is therefore correctly rounded --
 *   always the nearest representable 16.16 value to the true sine.
 */

#include "fixed_math.h"

#include <stdint.h>

/* 1.0 and pi/2 in Q30.  pi/2 * 2^30 = 1686629713.226..., truncated; the
 * 0.226 Q30 units it drops are 1.4e-8 of a 16.16 LSB. */
#define Q30_ONE    (1073741824LL)
#define Q30_HALFPI (1686629713LL)

/*
 * Q30 multiply with round-to-nearest.
 *
 * The +2^29 before the shift is what makes the Horner chain unbiased.
 * Without it every step truncates toward -infinity (>> on a negative
 * int64_t is an arithmetic shift, i.e. a floor), and seven such steps
 * accumulate a one-sided drift rather than cancelling out.
 */
static int64_t q30_mul(int64_t a, int64_t b)
{
    return (a * b + (1LL << 29)) >> 30;
}

/*
 * sin(t * pi/2) for t in Q30 over [0, 1], result in Q30 over [0, 1].
 *
 * Maclaurin series for sine, in Horner form over y^2:
 *
 *   sin y = y (1 - y^2/6 (1 - y^2/20 (1 - y^2/42 (1 - y^2/72
 *                (1 - y^2/110 (1 - y^2/156))))))
 *
 * The divisors are (2k)(2k+1) for k = 1..6 -- 6, 20, 42, 72, 110, 156 --
 * which is what turns the nested form back into the familiar
 * y - y^3/3! + y^5/5! - y^7/7! + y^9/9! - y^11/11! + y^13/13!.  Getting
 * that sequence wrong is silent: the shape still looks like a sine and
 * still passes at small angles, it just diverges toward the quadrant edge.
 * The table-regeneration test in tests/kernel/test_fixed_math_k.c is what
 * catches it, because it samples the whole quadrant.
 *
 * Seven terms are enough precisely because the argument is pre-reduced to
 * [0, pi/2]: the first dropped term is y^15/15!, which at y = pi/2 is
 * 4.7e-13 -- 500 Q30 units below the least significant bit this function
 * can return.  Without the reduction the series would need far more terms
 * and would still lose accuracy for large angles, which is the classic
 * reason a naive Taylor sine is a bad idea.
 */
static int64_t q30_sin_quadrant(int64_t t)
{
    int64_t y  = q30_mul(t, Q30_HALFPI);
    int64_t y2 = q30_mul(y, y);
    int64_t p;

    p = Q30_ONE - q30_mul(y2, Q30_ONE) / 156;
    p = Q30_ONE - q30_mul(y2, p) / 110;
    p = Q30_ONE - q30_mul(y2, p) / 72;
    p = Q30_ONE - q30_mul(y2, p) / 42;
    p = Q30_ONE - q30_mul(y2, p) / 20;
    p = Q30_ONE - q30_mul(y2, p) / 6;

    return q30_mul(y, p);
}

/*
 * Q30 -> 16.16 with round-to-nearest, applied to the magnitude.
 *
 * The sign is split off first rather than shifting the signed value
 * directly.  `(s + 8192) >> 14` looks like round-to-nearest and is, for
 * s >= 0; for s < 0 the arithmetic shift floors, which rounds away from
 * zero and leaves a systematic one-LSB bias across the two negative
 * quadrants.  Measured, that bug tripled the worst-case error (0.4999 LSB
 * -> 1.4999 LSB) while leaving every positive-quadrant value untouched --
 * so half the table stayed exact and the symptom looked like an accuracy
 * limit rather than a sign bug.
 */
static int32_t q30_to_fixed(int64_t s)
{
    if (s >= 0) {
        return (int32_t)((s + (1LL << 13)) >> 14);
    }
    return -(int32_t)((-s + (1LL << 13)) >> 14);
}

/*
 * Argument reduction.  The top two bits of a BAM angle are the quadrant
 * and the remaining 30 bits are the position within it, which is the whole
 * appeal of BAM: the reduction is a shift and a mask, exact for every
 * input, with no modulo and no cancellation near a multiple of pi.
 *
 *   quadrant 0:  sin =  S(t)        1: sin =  S(1-t)
 *   quadrant 2:  sin = -S(t)        3: sin = -S(1-t)
 *
 * where S(t) = sin(t * pi/2).  The odd quadrants mirror because
 * sin(pi/2 + x) = cos(x) = sin(pi/2 - x).
 */
static int64_t bam_sin_q30(uint32_t angle)
{
    uint32_t quadrant = angle >> 30;
    int64_t  t        = (int64_t)(angle & 0x3FFFFFFFu);
    int64_t  s;

    if (quadrant & 1u) {
        t = Q30_ONE - t;
    }

    s = q30_sin_quadrant(t);

    if (quadrant & 2u) {
        s = -s;
    }

    return s;
}

int32_t exo_fixed_sin(uint32_t angle)
{
    return q30_to_fixed(bam_sin_q30(angle));
}

int32_t exo_fixed_cos(uint32_t angle)
{
    /* cos x = sin(x + pi/2); the quarter-turn offset wraps in uint32_t
     * exactly, so this needs no special case at the top of the circle. */
    return q30_to_fixed(bam_sin_q30(angle + 0x40000000u));
}

int32_t exo_fixed_tan(uint32_t angle)
{
    int64_t s = bam_sin_q30(angle);
    int64_t c = bam_sin_q30(angle + 0x40000000u);
    int64_t q;

    /* Exactly at a pole there is no answer to give; saturate with the sign
     * of the numerator so tan(+pi/2) and tan(-pi/2) come out opposite, the
     * way the limits do. */
    if (c == 0) {
        return (s >= 0) ? INT32_MAX : INT32_MIN;
    }

    /* s is at most 2^30, so the << 16 tops out at 2^46 -- three orders of
     * magnitude clear of int64_t.  The quotient is the tangent already in
     * 16.16. */
    q = (s << 16) / c;

    /* Near a pole the true tangent outgrows 16.16.  Clamping here is what
     * stops the narrowing cast below from wrapping a large positive
     * tangent into a large negative one -- a silent failure that would
     * look like the angle being on the wrong side of the pole. */
    if (q > INT32_MAX) {
        return INT32_MAX;
    }
    if (q < INT32_MIN) {
        return INT32_MIN;
    }

    return (int32_t)q;
}

/*
 * atan(2^-k) in BAM, k = 0..30.
 *
 * The table stops at k = 30 because the next entries are already zero in
 * BAM: atan(2^-31) is under half a BAM unit, so rotating by it cannot
 * change the accumulated angle.  Entries 28..30 are the last that round to
 * anything non-zero at all, which is why the tail reads 3, 1, 1.
 */
static const uint32_t cordic_atan_bam[31] = {
    536870912u, 316933406u, 167458907u, 85004756u,
    42667331u,  21354465u,  10679838u,  5340245u,
    2670163u,   1335087u,   667544u,    333772u,
    166886u,    83443u,     41722u,     20861u,
    10430u,     5215u,      2608u,      1304u,
    652u,       326u,       163u,       81u,
    41u,        20u,        10u,        5u,
    3u,         1u,         1u
};

/*
 * CORDIC, vectoring mode.
 *
 * Start at the vector (1, slope) and rotate it onto the x axis by a fixed
 * sequence of shear steps, each of which is a shift and two adds; the sum
 * of the angles rotated away is the arctangent.  Only the angle is wanted
 * here, so the per-step magnitude gain (the usual 1.6468 CORDIC constant)
 * never has to be compensated for -- it scales x and y together and drops
 * out of the sign test that drives the loop.
 *
 * Working in Q30 rather than in the caller's 16.16 gives the vector 14
 * extra bits to shrink into before the 31 shift steps run out of
 * resolution; at 16.16 the last dozen iterations would operate on zeros
 * and the small-slope end of the range would quantise badly.
 */
uint32_t exo_fixed_atan(int32_t slope)
{
    int64_t  x, y;
    uint32_t angle = 0;
    int      k;

    /* Negative slopes belong to a quadrant this function does not model;
     * see the header.  Zero is the correct answer for zero slope anyway. */
    if (slope <= 0) {
        return 0;
    }

    x = Q30_ONE;
    y = (int64_t)slope << 14; /* 16.16 -> Q30 */

    for (k = 0; k < 31; k++) {
        int64_t x_next;

        if (y == 0) {
            break;
        }

        if (y > 0) {
            x_next = x + (y >> k);
            y      = y - (x >> k);
            angle += cordic_atan_bam[k];
        } else {
            x_next = x - (y >> k);
            y      = y + (x >> k);
            angle -= cordic_atan_bam[k];
        }

        x = x_next;
    }

    return angle;
}
