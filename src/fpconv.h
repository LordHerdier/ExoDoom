#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * fpconv — IEEE-754 binary64 <-> decimal text, in integer arithmetic only
 * (SCRUM-65).
 *
 * Doom reaches floating point in exactly three places, and all three are
 * text conversions rather than arithmetic:
 *
 *     printf("%f")   m_config.c:1699 (config writer), g_game.c:2265 (a
 *                    timedemo I_Error message)
 *     atof()         m_config.c:1766 (config reader)
 *
 * ── Why this is a separate module, and integer-only ────────────────────
 *
 * Because otherwise none of it could be tested, and half of it could not
 * even be compiled.
 *
 * The kernel builds -mno-sse -mno-sse2, and the x86_64 SysV ABI passes and
 * returns `double` in xmm0.  So in any file the kernel's own glob compiles,
 * `double atof(const char *)` is rejected outright, and so is
 * `va_arg(ap, double)` -- "SSE register argument with SSE disabled".  Every
 * kernel test TU carries the same flags, so a double-valued API could not be
 * called from a unit test either.
 *
 * And it is not only the kernel: docker/scripts/build.sh derives its ring-3
 * probe_cflags from those same CFLAGS, swapping only -mcmodel, so every
 * ring-3 link target built today is ALSO -mno-sse.  Which is why the two
 * adapters below are gated on __SSE2__ rather than on !EXO_KERNEL -- the
 * question is "can this translation unit name a double", not "is this the
 * kernel", and the two are not the same.
 *
 * Taking the bit pattern as a uint64_t sidesteps all of it.  The conversion
 * logic -- which is where every actual bug lives: exponent handling,
 * rounding, carry propagation, the subnormal and infinity edges -- lives
 * here, builds everywhere, and is driven directly by
 * tests/kernel/test_fpconv_k.c from ring 0.  What stays behind the
 * __SSE2__ gate in src/stdio.c and src/stdlib.c is two adapters of three
 * lines each, doing nothing but moving 8 bytes between a double and a
 * uint64_t.
 */

/*
 * Format `bits` (an IEEE-754 binary64 bit pattern) into `buf` as fixed-point
 * decimal with exactly `precision` fractional digits -- printf's "%f", whose
 * default precision is 6.
 *
 * Returns the number of characters written, not counting the NUL, and always
 * NUL-terminates as long as cap >= 1.  Output is truncated to fit `cap`
 * rather than overrunning it; the return value is the truncated length, so it
 * is not snprintf's "would have written" convention.
 *
 * Infinities and NaN come out as "inf"/"-inf"/"nan", matching C99.  Rounding
 * is round-half-to-even at the last emitted digit -- what a hosted printf
 * does under the default FE_TONEAREST, so "%.0f" of 0.5 is "0" and of 1.5 is
 * "2" -- with carries propagated into the integer part, so 9.9999 at
 * precision 3 gives "10.000".  Verified digit-for-digit against glibc's
 * snprintf over 23 values x 10 precisions in tests/kernel/test_fpconv_k.c.
 *
 * Very large magnitudes are the documented limit: a value whose integer part
 * does not fit in 64 bits is rendered as "<huge>" (or "-<huge>") rather than
 * wrapping into a plausible-looking wrong number.  %f on such a value is
 * already a misuse -- the integer part alone would be more than 19 digits --
 * and Doom never produces one: its %f values are a mouse acceleration factor
 * and a frames-per-second figure.
 */
size_t exo_fmt_f64(char *buf, size_t cap, uint64_t bits, unsigned precision);

/*
 * Parse leading decimal text from `s` and return the IEEE-754 binary64 bit
 * pattern -- atof's engine, and strtod's without the locale.
 *
 * Accepts optional leading whitespace, an optional sign, digits with an
 * optional '.', and an optional 'e'/'E' exponent.  On success, if `endp` is
 * non-NULL it receives a pointer to the first unconsumed character; if no
 * conversion could be performed, `*endp` is set to `s` and the result is +0.0
 * -- which is what atof is specified to return for unparseable input.
 *
 * ── Accuracy, measured rather than claimed ─────────────────────────────
 *
 * These numbers come from comparing against glibc's strtod over 11,583
 * generated inputs spanning decimal exponents -40..+40, not from reasoning
 * about the algorithm:
 *
 *     bit-exact for every sampled value with |decimal exponent| <= 15
 *     88.2% bit-exact overall across the full -40..+40 range
 *     99.97% within 1 ULP; worst observed error 2 ULP (3 of 11,583)
 *
 * So: exact for everything a Doom config file holds ("2.0", "0.5", "1.5")
 * and everything a human types by hand, and within 2 ULP at the extremes.
 *
 * It is NOT correctly rounded in general, and it does not try to be.  Scaling
 * by a decimal exponent past 19 composes more than one power of ten and each
 * composition rounds; getting the last bit right for arbitrary input is the
 * Steele-White / Clinger problem and needs arbitrary-precision arithmetic.
 * That is a great deal of machinery for a call site -- m_config.c's config
 * reader -- which cannot currently execute at all, because there is no config
 * file to read.  The bound is written down here so the next person meets it
 * as a documented boundary rather than as a mysterious last digit.
 */
uint64_t exo_parse_f64(const char *s, const char **endp);
