/*
 * fpconv.c — IEEE-754 binary64 <-> decimal, integer arithmetic only
 * (SCRUM-65).  See src/fpconv.h for why this exists and what it promises.
 *
 * ── The representation the formatter works in ──────────────────────────
 *
 * A finite binary64 is  (-1)^sign * mantissa * 2^exp2  with a 53-bit
 * mantissa.  Splitting that into an integer part and a fraction is the whole
 * job, and the fraction is held as a Q64 fixed-point value -- a uint64_t
 * where 2^64 means 1.0.  That choice is what makes digit extraction exact:
 * multiplying a Q64 fraction by 10 in 128 bits gives the next decimal digit
 * in the high half and the remaining fraction in the low half, with no
 * rounding anywhere in the loop.  128-bit multiply is a single mulq on
 * x86_64 and needs no SSE, so it is available in every build this file
 * appears in.
 */

#include "fpconv.h"

#include <stddef.h>
#include <stdint.h>

#define F64_SIGN_MASK  0x8000000000000000ULL
#define F64_EXP_MASK   0x7FF0000000000000ULL
#define F64_FRAC_MASK  0x000FFFFFFFFFFFFFULL
#define F64_FRAC_BITS  52
#define F64_EXP_BIAS   1023

/* The largest precision worth honouring.  Past this the digits are noise --
 * binary64 carries about 17 significant decimal digits total -- and the
 * bound also keeps the output length something a caller can reason about. */
#define FPCONV_MAX_PRECISION 30

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} sink_t;

/* Append one character, dropping it if the buffer is full.  Truncation is
 * silent by design: every caller here is producing a diagnostic, and a
 * half-written number is more useful than a fault. */
static void put(sink_t *s, char c)
{
    if (s->len + 1 < s->cap) {
        s->buf[s->len++] = c;
    }
}

static void put_str(sink_t *s, const char *str)
{
    while (*str != '\0') {
        put(s, *str++);
    }
}

/* Write `v` in decimal.  Used for the integer part, which is already known to
 * fit in 64 bits by the time this runs. */
static void put_u64(sink_t *s, uint64_t v)
{
    char   tmp[20];
    size_t n = 0;

    if (v == 0) {
        put(s, '0');
        return;
    }

    while (v != 0) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }

    while (n > 0) {
        put(s, tmp[--n]);
    }
}

size_t exo_fmt_f64(char *buf, size_t cap, uint64_t bits, unsigned precision)
{
    sink_t   s = { buf, cap, 0 };
    int      negative;
    uint64_t exp_field;
    uint64_t frac_field;
    uint64_t mantissa;
    int      exp2;
    uint64_t whole;
    uint64_t frac_q64; /* fraction, 2^64 == 1.0 */
    unsigned i;

    if (cap == 0) {
        return 0;
    }

    if (precision > FPCONV_MAX_PRECISION) {
        precision = FPCONV_MAX_PRECISION;
    }

    negative   = (bits & F64_SIGN_MASK) != 0;
    exp_field  = (bits & F64_EXP_MASK) >> F64_FRAC_BITS;
    frac_field = bits & F64_FRAC_MASK;

    /* Infinity and NaN share the all-ones exponent; the significand tells
     * them apart.  C99 spells these "inf"/"nan" for %f. */
    if (exp_field == 0x7FFu) {
        if (frac_field != 0) {
            put_str(&s, "nan");
        } else {
            put_str(&s, negative ? "-inf" : "inf");
        }
        s.buf[s.len] = '\0';
        return s.len;
    }

    if (exp_field == 0) {
        /*
         * Subnormal (or zero): no implicit leading 1, and the exponent is
         * 1 - bias rather than 0 - bias.  Every subnormal is smaller than
         * 2^-1022, so at any precision this code will honour it formats as
         * zero -- but going through the same path rather than special-casing
         * it keeps the sign right, so -0.0 prints as "-0.000000" the way a
         * hosted printf does.
         */
        mantissa = frac_field;
        exp2     = 1 - F64_EXP_BIAS - F64_FRAC_BITS;
    } else {
        mantissa = frac_field | (1ULL << F64_FRAC_BITS);
        exp2     = (int)exp_field - F64_EXP_BIAS - F64_FRAC_BITS;
    }

    /*
     * Split mantissa * 2^exp2 into `whole` and a Q64 fraction.
     *
     * The three cases are the three places the shift can land relative to the
     * 64-bit word, and each has an overflow or underflow edge that a single
     * expression would get wrong:
     */
    if (exp2 >= 0) {
        /* No fraction at all.  The guard is the documented "<huge>" limit:
         * mantissa is up to 2^53, so anything shifted past 2^64 has an
         * integer part 64 bits cannot hold. */
        if (exp2 >= 64 || mantissa > (UINT64_MAX >> exp2)) {
            put_str(&s, negative ? "-<huge>" : "<huge>");
            s.buf[s.len] = '\0';
            return s.len;
        }
        whole    = mantissa << exp2;
        frac_q64 = 0;
    } else if (-exp2 < 64) {
        int shift = -exp2;

        whole = mantissa >> shift;
        /* The remainder is strictly below 2^shift, so promoting it to Q64 is
         * a left shift by exactly (64 - shift) and loses nothing. */
        frac_q64 = (mantissa & ((1ULL << shift) - 1u)) << (64 - shift);
    } else {
        /*
         * The value is below 2^-11 or so; there is no integer part, and the
         * mantissa has to be shifted DOWN into Q64 rather than up.  For a
         * deeply subnormal input the shift exceeds 64 and the Q64 fraction is
         * genuinely zero -- writing that out explicitly avoids the undefined
         * behaviour of a >= 64 bit shift, which on x86 would otherwise
         * silently use (shift % 64) and produce a large wrong fraction.
         */
        int shift = -exp2 - 64;

        whole    = 0;
        frac_q64 = (shift < 64) ? (mantissa >> shift) : 0;
    }

    /*
     * Generate the fractional digits into a local buffer first, then round.
     *
     * Rounding cannot be done as the digits are emitted, because a carry out
     * of the last one propagates all the way back through them and into the
     * integer part: 9.9999 at precision 3 has to become "10.000", and a
     * round-as-you-go loop would already have printed the 9s.
     *
     * The rule is round-half-to-even at the final digit, which is what a
     * hosted printf does under the default FE_TONEAREST -- so "%.0f" of 0.5
     * is "0" and of 1.5 is "2".  Half-away-from-zero would have been simpler
     * (add half an ulp up front and let it carry), and it is what an earlier
     * version of this function did; it was changed because it disagreed with
     * glibc on every exact tie, which makes the reference test in
     * tests/kernel/test_fpconv_k.c far less useful as a check.
     */
    {
        char     digits[FPCONV_MAX_PRECISION];
        int      round_up;
        unsigned last_parity;

        for (i = 0; i < precision; i++) {
            /* Q64 * 10: the high half of the 128-bit product is the next
             * digit, the low half is the fraction that remains.  Exact --
             * no rounding anywhere in this loop. */
            unsigned __int128 scaled = (unsigned __int128)frac_q64 * 10u;

            digits[i] = (char)('0' + (unsigned)(scaled >> 64));
            frac_q64  = (uint64_t)scaled;
        }

        /* What is left over decides the rounding.  Exactly 0.5 is the tie. */
        last_parity = (precision > 0)
                          ? (unsigned)(digits[precision - 1] - '0') & 1u
                          : (unsigned)(whole & 1u);

        if (frac_q64 > (1ULL << 63)) {
            round_up = 1;
        } else if (frac_q64 < (1ULL << 63)) {
            round_up = 0;
        } else {
            round_up = (last_parity != 0); /* ties to even */
        }

        if (round_up) {
            int d = (int)precision - 1;

            while (d >= 0) {
                if (digits[d] != '9') {
                    digits[d]++;
                    break;
                }
                digits[d] = '0';
                d--;
            }

            if (d < 0) {
                whole++; /* the carry reached the integer part */
            }
        }

        if (negative) {
            put(&s, '-');
        }

        put_u64(&s, whole);

        if (precision > 0) {
            put(&s, '.');
            for (i = 0; i < precision; i++) {
                put(&s, digits[i]);
            }
        }
    }

    s.buf[s.len] = '\0';
    return s.len;
}

/*
 * Exact powers of ten.
 *
 * 10^22 is the largest power of ten that binary64 represents exactly (past
 * it the value needs more than 53 significant bits), which is why
 * exo_parse_f64 is exact only within a decimal exponent of +/-22: inside that
 * window the scaling is one multiply or one divide by an exact value, so it
 * rounds once, and a single correctly-rounded operation on an exact operand
 * is the correctly-rounded result.
 */
static const uint64_t pow10_u64[20] = {
    1ULL,
    10ULL,
    100ULL,
    1000ULL,
    10000ULL,
    100000ULL,
    1000000ULL,
    10000000ULL,
    100000000ULL,
    1000000000ULL,
    10000000000ULL,
    100000000000ULL,
    1000000000000ULL,
    10000000000000ULL,
    100000000000000ULL,
    1000000000000000ULL,
    10000000000000000ULL,
    100000000000000000ULL,
    1000000000000000000ULL,
    10000000000000000000ULL
};

/*
 * Build a binary64 bit pattern from a non-negative integer significand and a
 * binary exponent, rounding to nearest-even.
 *
 * `value` is the significand as an integer and `exp2` the power of two it is
 * scaled by, i.e. the result is value * 2^exp2.
 */
static uint64_t pack_f64(int negative, uint64_t value, int exp2)
{
    uint64_t sign = negative ? F64_SIGN_MASK : 0;
    int      shift;
    int      msb;
    int      biased;

    if (value == 0) {
        return sign; /* +/- 0.0 */
    }

    /* Normalise so the significand occupies bits [52:0] with the implicit
     * leading 1 at bit 52. */
    msb = 63;
    while ((value & (1ULL << msb)) == 0) {
        msb--;
    }

    shift = msb - F64_FRAC_BITS;

    if (shift > 0) {
        /* Losing bits: round to nearest, ties to even, which is what the
         * hardware would do and what keeps a parse of "0.5" exact. */
        uint64_t dropped_mask = (1ULL << shift) - 1u;
        uint64_t dropped      = value & dropped_mask;
        uint64_t half         = 1ULL << (shift - 1);

        value >>= shift;
        exp2 += shift;

        if (dropped > half || (dropped == half && (value & 1u) != 0)) {
            value++;
            if (value == (1ULL << (F64_FRAC_BITS + 1))) {
                /* The round carried into a new binade. */
                value >>= 1;
                exp2++;
            }
        }
    } else if (shift < 0) {
        value <<= -shift;
        exp2 += shift;
    }

    biased = exp2 + F64_EXP_BIAS + F64_FRAC_BITS;

    if (biased >= 0x7FF) {
        return sign | F64_EXP_MASK; /* overflow to infinity */
    }
    if (biased <= 0) {
        /* Underflow.  Subnormals are not reconstructed: the values this
         * parser exists for are config-file scalars, and returning a signed
         * zero is both correct to within the smallest normal and honest
         * about not having gone further. */
        return sign;
    }

    return sign | ((uint64_t)biased << F64_FRAC_BITS) | (value & F64_FRAC_MASK);
}

/* Scale `bits` by 10^n (n > 0) or 10^-n, one exact power at a time.  Each
 * step multiplies or divides an already-rounded binary64 by an exactly
 * representable power of ten, so a single step is correctly rounded and a
 * chain of them is within 1 ULP -- the bound src/fpconv.h documents. */
static uint64_t scale_pow10(uint64_t bits, int decexp)
{
    /* Implemented against the integer significand rather than by
     * multiplying doubles, since this file cannot use `double` at all
     * (see src/fpconv.h).  Unpack, scale in 128 bits, repack. */
    while (decexp > 0) {
        int      step = decexp > 19 ? 19 : decexp;
        uint64_t sign = bits & F64_SIGN_MASK;
        uint64_t expf = (bits & F64_EXP_MASK) >> F64_FRAC_BITS;
        uint64_t man;
        int      e2;
        unsigned __int128 prod;

        if (expf == 0 || expf == 0x7FFu) {
            return bits; /* zero, subnormal, inf or nan: nothing to scale */
        }

        man  = (bits & F64_FRAC_MASK) | (1ULL << F64_FRAC_BITS);
        e2   = (int)expf - F64_EXP_BIAS - F64_FRAC_BITS;
        prod = (unsigned __int128)man * pow10_u64[step];

        /* Bring the product back under 64 bits before repacking; every bit
         * shifted out here is accounted for by pack_f64's rounding. */
        while (prod > (unsigned __int128)UINT64_MAX) {
            prod >>= 1;
            e2++;
        }

        bits = pack_f64(sign != 0, (uint64_t)prod, e2);
        decexp -= step;
    }

    while (decexp < 0) {
        uint64_t sign = bits & F64_SIGN_MASK;
        uint64_t expf = (bits & F64_EXP_MASK) >> F64_FRAC_BITS;
        uint64_t man;
        int      e2;
        int      step;
        int      i;

        if (expf == 0 || expf == 0x7FFu) {
            return bits;
        }

        man  = (bits & F64_FRAC_MASK) | (1ULL << F64_FRAC_BITS);
        e2   = (int)expf - F64_EXP_BIAS - F64_FRAC_BITS;
        step = decexp < -19 ? 19 : -decexp;

        /*
         * Divide by ten `step` times, in 64-bit arithmetic.
         *
         * The obvious form -- shift the mantissa up into a 128-bit value and
         * divide once by 10^step -- is what this used to do, and it is why
         * this loop exists instead: a 128/64 division compiles to a call to
         * libgcc's __udivti3, and the ring-3 link targets
         * (build_ring3_link_target in docker/scripts/build.sh) link with
         * `x86_64-elf-ld` and no -lgcc at all.  The kernel would have linked
         * it fine; a LibOS would have failed with an undefined reference to a
         * symbol nobody wrote, which is a confusing way to discover a libgcc
         * dependency.
         *
         * Renormalising to bit 63 before each divide is what keeps the
         * precision: the quotient then always carries ~60 significant bits,
         * so each step truncates below 2^-59 relative.  Even 300 steps (the
         * "1e-300" case) accumulate under 2 ULP of the final double, which is
         * the bound src/fpconv.h documents and measures.
         */
        for (i = 0; i < step; i++) {
            while ((man >> 63) == 0) {
                man <<= 1;
                e2--;
            }
            man /= 10u;
        }

        bits = pack_f64(sign != 0, man, e2);
        decexp += step;
    }

    return bits;
}

uint64_t exo_parse_f64(const char *s, const char **endp)
{
    const char *p = s;
    int         negative = 0;
    int         any_digits = 0;
    uint64_t    significand = 0;
    int         decexp = 0;
    uint64_t    bits;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
           *p == '\v' || *p == '\f') {
        p++;
    }

    if (*p == '+' || *p == '-') {
        negative = (*p == '-');
        p++;
    }

    /*
     * Integer digits.  Once the significand is full, further digits are
     * counted into the decimal exponent rather than accumulated -- dropping
     * them outright would turn "12345678901234567890" into a number a
     * thousand times too small, where scaling keeps the magnitude right and
     * only loses digits past binary64's 17-digit resolution anyway.
     */
    while (*p >= '0' && *p <= '9') {
        any_digits = 1;
        if (significand <= (UINT64_MAX - 9u) / 10u) {
            significand = significand * 10u + (uint64_t)(*p - '0');
        } else {
            decexp++;
        }
        p++;
    }

    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            any_digits = 1;
            if (significand <= (UINT64_MAX - 9u) / 10u) {
                significand = significand * 10u + (uint64_t)(*p - '0');
                decexp--;
            }
            /* Past the significand's capacity a fraction digit changes
             * nothing, so it is consumed and discarded without touching
             * decexp -- unlike an integer digit, which moves the point. */
            p++;
        }
    }

    if (!any_digits) {
        /* No conversion performed.  atof is specified to return 0 here, and
         * *endp must be the original pointer -- including the whitespace and
         * sign this function already walked past, which is why `s` is used
         * rather than `p`. */
        if (endp != NULL) {
            *endp = s;
        }
        return 0;
    }

    if (*p == 'e' || *p == 'E') {
        const char *exp_start = p;
        int         exp_neg   = 0;
        int         exp_val   = 0;
        int         exp_digits = 0;

        p++;
        if (*p == '+' || *p == '-') {
            exp_neg = (*p == '-');
            p++;
        }

        while (*p >= '0' && *p <= '9') {
            exp_digits = 1;
            /* Clamped: an exponent past a few hundred already saturates to
             * infinity or zero, and letting it overflow int would wrap the
             * sign and give the opposite answer. */
            if (exp_val < 100000) {
                exp_val = exp_val * 10 + (*p - '0');
            }
            p++;
        }

        if (exp_digits) {
            decexp += exp_neg ? -exp_val : exp_val;
        } else {
            /* "1e" with no digits: the 'e' is not part of the number. */
            p = exp_start;
        }
    }

    bits = pack_f64(negative, significand, 0);
    bits = scale_pow10(bits, decexp);

    if (endp != NULL) {
        *endp = p;
    }

    return bits;
}
