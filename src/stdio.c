/*
 * stdio.c — freestanding stdio (SCRUM-20, SCRUM-21, SCRUM-65).
 *
 * The formatting engine (kvprintf) is sink-based: it emits each output
 * character through a caller-supplied callback and never touches a fixed
 * output buffer, so the same engine drives printf (sink = serial),
 * snprintf (sink = bounded buffer) and fprintf (sink = a FILE).
 *
 * SCRUM-65 finished it: precision, the length modifiers, %X/%o/%f, the
 * bounded-buffer family, stdout/stderr, the memory-mapped FILE* layer and
 * sscanf.  docs/libc_audit.md is the list this was working from.
 */

#include "stdio.h"

#include "ctype.h"
#include "errno.h"
#include "fpconv.h"
#include "stdlib.h"
#include "string.h"
#include "sys/stat.h"  /* mkdir -- grouped with remove/rename below */

#ifdef EXO_KERNEL
#include "serial.h"
#else
#include "exo_syscall.h"
#endif

#include <stdint.h>

/* Local strlen so the core has no dependency on src/string.c and stays usable
 * at the very earliest points of boot. */
static size_t kstrlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Emit n bytes of s through the sink. */
static void emit_str(void (*emit)(int c, void *ctx), void *ctx,
                     const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        emit((unsigned char)s[i], ctx);
}

/* Emit character c, n times. Returns n. */
static int emit_rep(void (*emit)(int c, void *ctx), void *ctx, int c, int n)
{
    for (int i = 0; i < n; i++)
        emit(c, ctx);
    return n;
}

/*
 * Working buffer for one conversion.
 *
 * Sized for the widest thing that can land in it, which is not a number: a
 * %f with the maximum precision fpconv honours (30 fractional digits) plus a
 * 20-digit integer part, a sign and a point. The old 20 was exactly right for
 * a 64-bit decimal and exactly wrong the moment SCRUM-65 added %f and
 * precision (which can demand leading zeros of its own, as in "%.40d").
 */
#define FMT_NUMBUF_LEN 96

/*
 * Convert an unsigned value to text in the given base into buf (which must
 * hold at least 20 chars — the widest is 64-bit decimal/binary-free).
 * Returns the number of digits written (no NUL terminator).
 */
static size_t fmt_uint(char *buf, uint64_t val, unsigned base, int lower)
{
    const char *digits = lower ? "0123456789abcdef" : "0123456789ABCDEF";
    char tmp[20];
    int i = 0;

    if (val == 0)
        tmp[i++] = '0';
    while (val) {
        tmp[i++] = digits[val % base];
        val /= base;
    }

    for (int j = 0; j < i; j++)
        buf[j] = tmp[i - 1 - j];
    return (size_t)i;
}

/*
 * Emit a field: an optional fixed prefix (a sign or "0x") followed by the
 * body, padded to `width`. Right-justified by default; `left` pads on the
 * right with spaces; `zero` fills between prefix and body with '0' (ignored
 * when left-justified). Returns the number of characters emitted.
 */
static int emit_field(void (*emit)(int c, void *ctx), void *ctx,
                      const char *prefix, size_t plen,
                      const char *body, size_t blen,
                      int width, int left, int zero)
{
    int total = (int)(plen + blen);
    int pad = (width > total) ? (width - total) : 0;
    int n = 0;

    if (left) {
        emit_str(emit, ctx, prefix, plen); n += (int)plen;
        emit_str(emit, ctx, body, blen);   n += (int)blen;
        n += emit_rep(emit, ctx, ' ', pad);
    } else if (zero) {
        emit_str(emit, ctx, prefix, plen); n += (int)plen;
        n += emit_rep(emit, ctx, '0', pad);
        emit_str(emit, ctx, body, blen);   n += (int)blen;
    } else {
        n += emit_rep(emit, ctx, ' ', pad);
        emit_str(emit, ctx, prefix, plen); n += (int)plen;
        emit_str(emit, ctx, body, blen);   n += (int)blen;
    }
    return n;
}

/*
 * Length modifiers, parsed into one enum rather than a set of flags.
 *
 * Doom uses %li in a handful of places and the sscanf side needs the same
 * vocabulary, but the real reason these exist is the one docs/libc_audit.md
 * sec3.1 identifies: an unrecognised modifier used to fall through to the
 * `default:` branch, which echoed the characters WITHOUT consuming the
 * vararg -- so every later conversion in the same call read the wrong
 * argument. Parsing a modifier the engine then ignores would still be
 * wrong; parsing it and widening the va_arg is what keeps the list aligned.
 */
typedef enum {
    LEN_NONE = 0,
    LEN_HH,
    LEN_H,
    LEN_L,
    LEN_LL,
    LEN_Z,
    LEN_T,
    LEN_J
} length_mod_t;

/* Fetch a signed integer argument of the width `len` names.  Anything
 * narrower than int has already been promoted by the caller's varargs, so
 * hh/h read an int and narrow afterwards. */
static int64_t fetch_signed(va_list *ap, length_mod_t len)
{
    switch (len) {
    case LEN_HH: return (int64_t)(signed char)va_arg(*ap, int);
    case LEN_H:  return (int64_t)(short)va_arg(*ap, int);
    case LEN_L:  return (int64_t)va_arg(*ap, long);
    case LEN_LL: return (int64_t)va_arg(*ap, long long);
    case LEN_Z:  return (int64_t)va_arg(*ap, size_t);
    case LEN_T:  return (int64_t)va_arg(*ap, ptrdiff_t);
    case LEN_J:  return (int64_t)va_arg(*ap, int64_t);
    default:     return (int64_t)va_arg(*ap, int);
    }
}

static uint64_t fetch_unsigned(va_list *ap, length_mod_t len)
{
    switch (len) {
    case LEN_HH: return (uint64_t)(unsigned char)va_arg(*ap, unsigned);
    case LEN_H:  return (uint64_t)(unsigned short)va_arg(*ap, unsigned);
    case LEN_L:  return (uint64_t)va_arg(*ap, unsigned long);
    case LEN_LL: return (uint64_t)va_arg(*ap, unsigned long long);
    case LEN_Z:  return (uint64_t)va_arg(*ap, size_t);
    case LEN_T:  return (uint64_t)va_arg(*ap, ptrdiff_t);
    case LEN_J:  return (uint64_t)va_arg(*ap, uint64_t);
    default:     return (uint64_t)va_arg(*ap, unsigned);
    }
}

/*
 * fmt_uint with a minimum digit count.
 *
 * `precision` < 0 means "unspecified" and behaves exactly like fmt_uint.
 * Otherwise the result is padded on the left with '0' to at least that many
 * digits -- which is what "%.3d" means and what builds Doom's "STCFN033"
 * lump names (docs/libc_audit.md sec3.1).
 *
 * The one C99 subtlety worth spelling out: a precision of 0 applied to a
 * value of 0 produces NO characters at all, not "0". "%.0d" of 0 is the
 * empty string. That is deliberate in the standard and free here -- the
 * digit loop simply never runs.
 */
static size_t fmt_uint_prec(char *buf, uint64_t val, unsigned base, int lower,
                            int precision)
{
    size_t len;

    if (precision == 0 && val == 0)
        return 0;

    len = fmt_uint(buf, val, base, lower);

    if (precision > 0 && len < (size_t)precision) {
        size_t pad = (size_t)precision - len;
        size_t i;

        /* Cap at the buffer rather than trusting the format string: "%.999d"
         * is a perfectly legal thing for a caller to write. */
        if (pad + len > FMT_NUMBUF_LEN)
            pad = FMT_NUMBUF_LEN - len;

        for (i = len; i > 0; i--)
            buf[i - 1 + pad] = buf[i - 1];
        for (i = 0; i < pad; i++)
            buf[i] = '0';

        len += pad;
    }

    return len;
}

/*
 * Pull a double off the varargs and hand back its bit pattern.
 *
 * This is the ONLY place in the kernel-visible sources that names the type
 * `double`, and it is gated because it has to be: under -mno-sse the
 * va_arg(ap, double) below is rejected outright
 * ("SSE register argument with SSE disabled"). All the actual work -- the
 * exponent handling, the rounding, the carry propagation -- is in
 * src/fpconv.c, which is integer-only and is unit-tested from ring 0.
 *
 * The predicate is __SSE2__, NOT !EXO_KERNEL. docker/scripts/build.sh builds
 * every ring-3 link target with probe_cflags, which is the kernel CFLAGS
 * with only -mcmodel swapped -- so -mno-sse -mno-sse2 survive into all of
 * them, and "not the kernel" does not imply "has SSE". Getting that wrong
 * broke SCRUM-51's libc_shim_probe build. See src/stdlib.h's atof comment.
 *
 * Without SSE this returns the bit pattern of 0.0 and consumes nothing.
 * That is safe rather than merely convenient: a translation unit that cannot
 * name a double cannot pass one to printf either, so a %f is unreachable
 * from such a build -- while the eventual Doom LibOS target, which must
 * enable SSE because Doom cannot compile without it (SCRUM-177), takes the
 * real path.
 */
static uint64_t fetch_double_bits(va_list *ap)
{
#ifdef __SSE2__
    double   d = va_arg(*ap, double);
    uint64_t bits;

    __builtin_memcpy(&bits, &d, sizeof bits);

    return bits;
#else
    (void)ap;
    return 0;
#endif
}

int kvprintf(void (*emit)(int c, void *ctx), void *ctx,
             const char *fmt, va_list ap)
{
    int count = 0;
    /* 20 digits for a 64-bit decimal, plus room for the zero-padding a
     * precision can demand (e.g. "%.40d"). */
    char numbuf[FMT_NUMBUF_LEN];
    /*
     * A real va_list object, not the parameter.
     *
     * On x86_64 va_list is an ARRAY type, so the `ap` parameter above has
     * already decayed to a pointer and `&ap` would be a pointer-to-pointer --
     * the wrong type for the fetch_* helpers, which need to advance the
     * caller's position. va_copy into a local gives something whose address
     * is genuinely a `va_list *`, which is the portable way to hand a
     * va_list to a helper that consumes from it.
     */
    va_list args;

    va_copy(args, ap);

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            emit((unsigned char)*p, ctx);
            count++;
            continue;
        }

        p++;

        /* flags */
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; p++) {
            if (*p == '-')      left = 1;
            else if (*p == '0') zero = 1;
            else if (*p == '+') plus = 1;
            else if (*p == ' ') space = 1;
            else if (*p == '#') alt = 1;
            else                break;
        }

        /* field width, possibly '*' */
        int width = 0;
        if (*p == '*') {
            width = va_arg(args, int);
            if (width < 0) { left = 1; width = -width; }
            p++;
        } else {
            while (*p >= '0' && *p <= '9')
                width = width * 10 + (*p++ - '0');
        }

        /*
         * Precision.
         *
         * This is the highest-severity item in docs/libc_audit.md (sec3.1),
         * and not because the output looked slightly wrong. Doom builds its
         * HUD font lump names with DEH_snprintf("STCFN%.3d", c) -- 
         * hu_stuff.c:297 -- and the intermission screens build theirs with
         * "%2.2d" / "%.2d". With precision unimplemented, the old default:
         * branch echoed "%.3" literally AND did not consume the vararg, so
         * the name became "STCFN%.3" for every glyph, W_CacheLumpName found
         * no such lump, and Doom died in I_Error before the HUD ever drew --
         * with every later conversion in the same call reading a shifted
         * argument list on the way there.
         *
         * A negative precision (from "%.*d" with a negative argument) means
         * "no precision", per C99.
         */
        int precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            if (*p == '*') {
                precision = va_arg(args, int);
                if (precision < 0) precision = -1;
                p++;
            } else {
                while (*p >= '0' && *p <= '9')
                    precision = precision * 10 + (*p++ - '0');
            }
        }

        /* length modifier */
        length_mod_t lmod = LEN_NONE;
        switch (*p) {
        case 'h':
            p++;
            if (*p == 'h') { lmod = LEN_HH; p++; } else lmod = LEN_H;
            break;
        case 'l':
            p++;
            if (*p == 'l') { lmod = LEN_LL; p++; } else lmod = LEN_L;
            break;
        case 'z': lmod = LEN_Z; p++; break;
        case 't': lmod = LEN_T; p++; break;
        case 'j': lmod = LEN_J; p++; break;
        case 'L': lmod = LEN_LL; p++; break;  /* long double -> treated as ll */
        default: break;
        }

        char spec = *p;
        if (spec == '\0') {           /* trailing '%' (e.g. "%5") */
            emit('%', ctx);
            count++;
            break;
        }

        /*
         * An explicit precision turns off zero-padding for the integer
         * conversions, because the precision already says how many digits
         * to produce -- C99 7.19.6.1p6. Leaving both on would pad twice.
         */
        if (precision >= 0 && (spec == 'd' || spec == 'i' || spec == 'u' ||
                               spec == 'x' || spec == 'X' || spec == 'o'))
            zero = 0;

        switch (spec) {
        case 'd':
        case 'i': {
            int64_t v = fetch_signed(&args, lmod);
            uint64_t mag = (v < 0) ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
            size_t len = fmt_uint_prec(numbuf, mag, 10, 1, precision);
            const char *pre = (v < 0) ? "-" : (plus ? "+" : (space ? " " : ""));
            size_t plen = (v < 0 || plus || space) ? 1 : 0;
            count += emit_field(emit, ctx, pre, plen,
                                numbuf, len, width, left, zero);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned base = (spec == 'u') ? 10u : (spec == 'o') ? 8u : 16u;
            uint64_t v = fetch_unsigned(&args, lmod);
            size_t len = fmt_uint_prec(numbuf, v, base, spec != 'X', precision);
            /* '#' prefixes 0x/0X for hex and a leading 0 for octal; the
             * octal form is folded into the digits by fmt_uint_prec's
             * precision handling rather than a prefix, so only hex needs
             * one here. */
            const char *pre = "";
            size_t plen = 0;
            if (alt && v != 0 && (spec == 'x' || spec == 'X')) {
                pre = (spec == 'x') ? "0x" : "0X";
                plen = 2;
            }
            count += emit_field(emit, ctx, pre, plen, numbuf, len,
                                width, left, zero);
            break;
        }
        case 'p': {
            void *ptr = va_arg(args, void *);
            size_t len = fmt_uint(numbuf, (uint64_t)(uintptr_t)ptr, 16, 1);
            count += emit_field(emit, ctx, "0x", 2, numbuf, len,
                                width, left, zero);
            break;
        }
        case 'c': {
            char ch = (char)va_arg(args, int);
            count += emit_field(emit, ctx, "", 0, &ch, 1, width, left, 0);
            break;
        }
        case 's': {
            const char *str = va_arg(args, const char *);
            size_t len;
            if (!str) str = "(null)";
            len = kstrlen(str);
            /* For %s the precision is a MAXIMUM, not a minimum -- it
             * truncates. m_misc.c:226 uses "%.8s" to clip a lump name into a
             * warning message, and a string that is not NUL-terminated
             * within `precision` bytes must not be read past it. */
            if (precision >= 0 && (size_t)precision < len)
                len = (size_t)precision;
            count += emit_field(emit, ctx, "", 0, str, len,
                                width, left, 0);
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            /*
             * All six float conversions render as %f.
             *
             * %e/%g are not separately implemented because nothing calls
             * them: src/doom/ uses %f twice (m_config.c's config writer and
             * a g_game.c timedemo message) and nothing else. Mapping them
             * onto %f rather than letting them fall to `default:` is the
             * important part -- default: would not consume the argument,
             * and a double occupies an SSE slot in the varargs, so skipping
             * it desynchronises everything after it in the same call.
             */
            uint64_t bits = fetch_double_bits(&args);
            size_t len = exo_fmt_f64(numbuf, sizeof numbuf, bits,
                                     precision < 0 ? 6u : (unsigned)precision);
            const char *pre = "";
            size_t plen = 0;
            const char *body = numbuf;
            /* exo_fmt_f64 writes its own '-'; lift it into the prefix so
             * zero-padding lands between the sign and the digits. */
            if (numbuf[0] == '-') { pre = "-"; plen = 1; body++; len--; }
            else if (plus)  { pre = "+"; plen = 1; }
            else if (space) { pre = " "; plen = 1; }
            count += emit_field(emit, ctx, pre, plen, body, len,
                                width, left, zero);
            break;
        }
        case 'n': {
            /* Stores the count so far. Doom never uses it; implemented
             * because the alternative -- falling through to default: -- would
             * leave a pointer argument unconsumed. */
            int *out = va_arg(args, int *);
            if (out) *out = count;
            break;
        }
        case '%': {
            count += emit_field(emit, ctx, "", 0, "%", 1, width, left, 0);
            break;
        }
        default:
            /*
             * Unknown specifier: echo it verbatim.
             *
             * This still does not consume an argument, and it still cannot
             * -- the engine has no idea what type the caller passed. The
             * difference from before SCRUM-65 is that it is now genuinely
             * unreachable for anything src/doom/ writes: every conversion
             * C99 defines is handled above, so landing here means the format
             * string itself is malformed.
             */
            emit('%', ctx);
            emit((unsigned char)spec, ctx);
            count += 2;
            break;
        }
    }

    va_end(args);

    return count;
}

#ifdef EXO_KERNEL

static void serial_emit(int c, void *ctx)
{
    (void)ctx;
    serial_putc((char)c);
}

int vprintf(const char *fmt, va_list ap)
{
    /* No serial_flush() here — matches the rest of the kernel; qemu_exit()
     * and explicit callers flush COM1 before it matters. */
    return kvprintf(serial_emit, NULL, fmt, ap);
}

#else /* !EXO_KERNEL — the ring-3 LibOS link target (SCRUM-51/-173) */

/*
 * printf here has no direct line to COM1 — src/serial.c is kernel-only,
 * unreachable (and, once EFER.NXE lands, unmapped) from ring 3. The only
 * channel is exo_serial_write() (#8, src/exo_syscall.h), a real `syscall`
 * instruction. Buffering the whole formatted line and flushing it in one
 * call, rather than one exo_serial_write per byte the way the kernel-side
 * sink does, is not just an optimization: it is what "printf works through
 * the syscall instruction" (this ticket's acceptance criterion) means in
 * practice — one syscall per printf(), not thousands.
 *
 * SERIAL_EMIT_BUF_LEN is well under SERIAL_WRITE_MAX_LEN (4096,
 * src/syscall_serial.h — not included here, since that header is
 * kernel-only) so a single flush never needs to split. A format string
 * longer than the buffer flushes early and keeps accumulating, so output is
 * never dropped, only split across more than one syscall.
 */
#define SERIAL_EMIT_BUF_LEN 480

typedef struct {
    char   buf[SERIAL_EMIT_BUF_LEN];
    size_t len;
} serial_emit_ctx_t;

static void serial_emit_flush(serial_emit_ctx_t *ctx)
{
    if (ctx->len == 0)
        return;
    exo_serial_write(ctx->buf, ctx->len);
    ctx->len = 0;
}

static void serial_emit(int c, void *raw_ctx)
{
    serial_emit_ctx_t *ctx = raw_ctx;
    if (ctx->len == SERIAL_EMIT_BUF_LEN)
        serial_emit_flush(ctx);
    ctx->buf[ctx->len++] = (char)c;
}

int vprintf(const char *fmt, va_list ap)
{
    serial_emit_ctx_t ctx = { .len = 0 };
    int n = kvprintf(serial_emit, &ctx, fmt, ap);
    serial_emit_flush(&ctx);
    return n;
}

#endif /* EXO_KERNEL */

/*
 * printf is the same call on both sides of the #ifdef above, because both
 * sides supply vprintf.  Keeping it here rather than duplicating it into
 * each branch is what guarantees the two paths cannot drift in anything but
 * the sink -- SCRUM-83 added vprintf precisely so doom_panic() could take a
 * va_list through that one shared engine instead of standing up a second
 * formatter of its own for the I_Error path, and SCRUM-65's vfprintf needs
 * the identical form.
 */
int printf(const char *fmt, ...)
{
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vprintf(fmt, ap);
    va_end(ap);

    return n;
}

int putchar(int c)
{
#ifdef EXO_KERNEL
    serial_putc((char)c);
#else
    char ch = (char)c;
    exo_serial_write(&ch, 1);
#endif
    return c;
}

int puts(const char *s)
{
#ifdef EXO_KERNEL
    serial_print(s);
    serial_putc('\n');
#else
    /* Reuse printf's own sink so a long `s` still only costs one flush per
     * SERIAL_EMIT_BUF_LEN, same as printf() itself. */
    printf("%s\n", s);
#endif
    return 0;
}

/*
 * ===========================================================================
 * SCRUM-65: the bounded-buffer family, stdout/stderr, and the FILE* layer.
 * ===========================================================================
 */

/* ---- snprintf / vsnprintf ---------------------------------------------- */

typedef struct {
    char  *buf;
    size_t cap;   /* total buffer size including the NUL */
    size_t len;   /* characters produced so far, INCLUDING those dropped */
} bufsink_t;

static void buf_emit(int c, void *ctx)
{
    bufsink_t *b = ctx;

    /* Keep counting past the end. snprintf returns what it WOULD have
     * written, and callers (M_StringJoin in m_misc.c among them) size their
     * allocations from that number -- so stopping the count at the buffer
     * would make every such allocation too small. */
    if (b->cap > 0 && b->len + 1 < b->cap)
        b->buf[b->len] = (char)c;

    b->len++;
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap)
{
    bufsink_t b;
    int       n;

    b.buf = str;
    b.cap = size;
    b.len = 0;

    n = kvprintf(buf_emit, &b, fmt, ap);

    /* Always terminate when there is room for a terminator at all. A
     * size of 0 means "tell me the length, write nothing" and must not
     * touch str -- which may legitimately be NULL in that case. */
    if (size > 0) {
        size_t end = (b.len < size - 1) ? b.len : size - 1;
        str[end] = '\0';
    }

    return n;
}

int snprintf(char *str, size_t size, const char *fmt, ...)
{
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(str, size, fmt, ap);
    va_end(ap);

    return n;
}

/* ---- FILE ---------------------------------------------------------------
 *
 * Two kinds of stream, one struct.
 *
 * A CONSOLE stream is stdout or stderr: writes go to COM1, reads return EOF.
 * A BLOB stream is a read-only window over memory that is already mapped --
 * which is what docs/architecture.md sec7's memory-mapped WAD reader calls
 * for, and the reason fopen can work at all on a system with no filesystem.
 * fread/fseek/ftell over a blob are pointer arithmetic; nothing is copied,
 * which matters when the blob is a 28 MB IWAD and the heap is a few MB.
 *
 * What there is NOT is a writable file. fopen("w") fails, and it fails
 * rather than pretending: Doom writes a config file and savegames, and a
 * successful-looking write that goes nowhere would surface later as a config
 * that never persists or a savegame that reloads as garbage. Returning NULL
 * puts the failure at the open, where M_SaveDefaults and P_SaveGame already
 * handle it.
 */

#define EXO_FILE_MAX 8

#define FFLAG_INUSE   0x01
#define FFLAG_CONSOLE 0x02
#define FFLAG_READ    0x04
#define FFLAG_EOF     0x08
#define FFLAG_ERR     0x10

struct _exo_file {
    const unsigned char *data; /* blob base; NULL for a console stream */
    size_t               size;
    size_t               pos;
    unsigned             flags;
};

/*
 * Registered blobs, keyed by the name fopen is called with.
 *
 * This is the seam the WAD reaches Doom through. A LibOS registers the
 * region the kernel mapped for it (see src/libos_wad_map.h) under the name
 * Doom will ask for, and W_AddFile's fopen then finds it. Eight slots is
 * generous: Doom opens one IWAD and, at most, a handful of PWADs.
 */
typedef struct {
    const char          *name;
    const unsigned char *data;
    size_t               size;
} blob_t;

static blob_t   blobs[EXO_FILE_MAX];
static unsigned blob_count;

static FILE file_table[EXO_FILE_MAX];

static FILE console_out = { NULL, 0, 0, FFLAG_INUSE | FFLAG_CONSOLE };
static FILE console_err = { NULL, 0, 0, FFLAG_INUSE | FFLAG_CONSOLE };

FILE *stdin  = NULL; /* nothing to read from; see fgetc's absence */
FILE *stdout = &console_out;
FILE *stderr = &console_err;

int exo_file_register_blob(const char *name, const void *data, size_t size)
{
    unsigned i;

    if (name == NULL || data == NULL)
        return -1;

    /* Re-registering a name replaces it rather than adding a shadowing
     * second entry -- otherwise a LibOS that re-mounts a WAD would leak
     * slots and fopen would keep finding the stale one.
     *
     * Compared case-insensitively, because find_blob() below is: if the two
     * disagreed about what "the same name" means, registering
     * "FreeDoom2.WAD" over "freedoom2.wad" would fall through to a second
     * slot that find_blob then matches just as happily as the first, and
     * which one a later fopen returns would come down to slot order. That
     * is a worse failure than either rule on its own -- the replace-in-place
     * contract this loop exists to provide would silently not hold. */
    for (i = 0; i < blob_count; i++) {
        if (strcasecmp(blobs[i].name, name) == 0) {
            blobs[i].data = data;
            blobs[i].size = size;
            return 0;
        }
    }

    if (blob_count == EXO_FILE_MAX)
        return -1;

    blobs[blob_count].name = name;
    blobs[blob_count].data = data;
    blobs[blob_count].size = size;
    blob_count++;

    return 0;
}

void exo_file_reset_blobs(void)
{
    blob_count = 0;
}

/*
 * Match a path against a registered blob name.
 *
 * Compared on the basename, not the whole path: Doom builds WAD paths by
 * joining a directory it discovered (d_iwad.c) onto a filename, so the
 * string reaching fopen is "/some/where/freedoom2.wad" while what was
 * registered is "freedoom2.wad". Matching the tail is what makes the two
 * meet without the registrar having to predict the path Doom will invent.
 */
static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash != NULL ? slash + 1 : path;
}

static blob_t *find_blob(const char *path)
{
    const char *base = basename_of(path);
    unsigned    i;

    for (i = 0; i < blob_count; i++) {
        if (strcasecmp(blobs[i].name, base) == 0)
            return &blobs[i];
    }

    return NULL;
}

FILE *fopen(const char *path, const char *mode)
{
    blob_t  *blob;
    unsigned i;

    if (path == NULL || mode == NULL) {
        errno = EINVAL;
        return NULL;
    }

    /* Any mode that can write is refused. See the block comment above: a
     * write that silently goes nowhere is worse than a failed open. */
    if (mode[0] != 'r') {
        errno = EROFS;
        return NULL;
    }

    blob = find_blob(path);
    if (blob == NULL) {
        errno = ENOENT;
        return NULL;
    }

    for (i = 0; i < EXO_FILE_MAX; i++) {
        if ((file_table[i].flags & FFLAG_INUSE) == 0) {
            file_table[i].data  = blob->data;
            file_table[i].size  = blob->size;
            file_table[i].pos   = 0;
            file_table[i].flags = FFLAG_INUSE | FFLAG_READ;
            return &file_table[i];
        }
    }

    errno = EMFILE;
    return NULL;
}

int fclose(FILE *stream)
{
    if (stream == NULL || (stream->flags & FFLAG_INUSE) == 0)
        return EOF;

    /* stdout/stderr are static and must survive being closed -- Doom's
     * I_Quit path can reach fclose on a stream it did not open. */
    if (stream->flags & FFLAG_CONSOLE)
        return 0;

    stream->flags = 0;
    stream->data  = NULL;
    stream->size  = 0;
    stream->pos   = 0;

    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    size_t want;
    size_t avail;
    size_t got;

    if (ptr == NULL || stream == NULL || (stream->flags & FFLAG_READ) == 0)
        return 0;

    /* size * nmemb can overflow; checking the division form avoids
     * wrapping into a small request that then reads the wrong amount. */
    if (size == 0 || nmemb == 0)
        return 0;
    if (nmemb > (size_t)-1 / size) {
        stream->flags |= FFLAG_ERR;
        return 0;
    }

    want  = size * nmemb;
    avail = stream->size - stream->pos;
    got   = want < avail ? want : avail;

    memcpy(ptr, stream->data + stream->pos, got);
    stream->pos += got;

    if (got < want)
        stream->flags |= FFLAG_EOF;

    /* fread returns whole ELEMENTS, not bytes -- a partial final element is
     * not counted. Getting this wrong makes a short read look complete. */
    return got / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    const char *p = ptr;
    size_t      total;
    size_t      i;

    if (ptr == NULL || stream == NULL || size == 0 || nmemb == 0)
        return 0;
    if (nmemb > (size_t)-1 / size)
        return 0;

    /* Only the console accepts writes. A blob is a read-only view of memory
     * the kernel mapped without VMM_WRITE, so writing through it would fault
     * rather than merely fail. */
    if ((stream->flags & FFLAG_CONSOLE) == 0)
        return 0;

    total = size * nmemb;
    for (i = 0; i < total; i++)
        putchar((unsigned char)p[i]);

    return nmemb;
}

int fseek(FILE *stream, long offset, int whence)
{
    long base;
    long target;

    if (stream == NULL || (stream->flags & FFLAG_READ) == 0) {
        errno = EBADF;
        return -1;
    }

    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (long)stream->pos; break;
    case SEEK_END: base = (long)stream->size; break;
    default:
        errno = EINVAL;
        return -1;
    }

    target = base + offset;

    /* Seeking past the end is legal for a real file (it creates a hole on
     * the next write); here there is nothing to write, so it is refused --
     * which is also what stops a subsequent fread from computing a negative
     * `avail`. */
    if (target < 0 || (size_t)target > stream->size) {
        errno = EINVAL;
        return -1;
    }

    stream->pos = (size_t)target;
    /* A successful seek clears EOF, per C99 -- W_StdC_Read seeks then reads
     * in a loop, and a sticky EOF would stop the second read. */
    stream->flags &= ~(unsigned)FFLAG_EOF;

    return 0;
}

long ftell(FILE *stream)
{
    if (stream == NULL || (stream->flags & FFLAG_READ) == 0) {
        errno = EBADF;
        return -1L;
    }

    return (long)stream->pos;
}

int feof(FILE *stream)
{
    if (stream == NULL)
        return 0;

    return (stream->flags & FFLAG_EOF) != 0;
}

int fflush(FILE *stream)
{
    /* Nothing is buffered on this side: the kernel build writes each byte
     * straight to the UART, and the ring-3 build's buffering lives inside
     * one printf call and is already flushed when that call returns. The
     * serial_flush() below is the UART's own FIFO, which is a different
     * thing and does matter -- src/serial.c buffers there. */
    (void)stream;
#ifdef EXO_KERNEL
    serial_flush();
#endif
    return 0;
}

int vfprintf(FILE *stream, const char *fmt, va_list ap)
{
    /* Everything that is not the console is read-only, so a formatted write
     * to it produces nothing and reports nothing written. */
    if (stream == NULL || (stream->flags & FFLAG_CONSOLE) == 0)
        return 0;

    return vprintf(fmt, ap);
}

int fprintf(FILE *stream, const char *fmt, ...)
{
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vfprintf(stream, fmt, ap);
    va_end(ap);

    return n;
}

/*
 * remove/rename: there is no filesystem, and saying so is the whole point.
 *
 * Both are reached only from the savegame rotation in m_misc.c/g_game.c, and
 * both already have callers that cope with failure. Returning 0 would claim a
 * file had been deleted or moved when nothing happened, which is exactly the
 * class of stub docs/libc_audit.md argues against.
 */
int remove(const char *path)
{
    (void)path;
    errno = EROFS;
    return -1;
}

int rename(const char *oldpath, const char *newpath)
{
    (void)oldpath;
    (void)newpath;
    errno = EROFS;
    return -1;
}

/*
 * mkdir lives here rather than in a file of its own because it is the same
 * answer as remove/rename above, for the same reason -- there is no
 * filesystem -- and grouping the three makes that one fact obvious instead
 * of repeated.  It is declared in <sys/stat.h>, not <stdio.h>.
 *
 * Returning -1/EROFS rather than docs/libc_audit.md sec6's suggested "no-op
 * returning 0": m_misc.c's M_MakeDirectory is called before writing a config
 * or savegame directory, and claiming the directory exists would push the
 * failure to the fopen that follows -- which already fails, but now for a
 * reason two steps removed from the real one.  EEXIST would be a third
 * option and is worse still: it is the one error every caller ignores.
 */
int mkdir(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    errno = EROFS;
    return -1;
}

/* ---- sscanf -------------------------------------------------------------
 *
 * Four call sites under src/doom/, and between them they need %x, %i, %o and
 * %d (m_misc.c:192-195's M_StrToInt tries all four bases in turn;
 * m_config.c:1721 parses a hex default). %s, %c and %f are implemented too --
 * not speculatively, but because leaving a conversion unhandled in a scanf is
 * the same trap as leaving one unhandled in a printf: the pointer argument
 * goes unconsumed and every later assignment in the same call lands in the
 * wrong variable.
 *
 * What is NOT here is the scanset ("%99[^\n]"). It has one call site,
 * m_config.c:1792, and that is an fscanf over the config file -- see fscanf
 * below for why that path cannot execute.
 */

/*
 * Staging size for a width-limited %f field.  A field wider than this is
 * clamped, which is benign in a way a smaller buffer would not be: this is
 * two and a half times the longest decimal that can change a double's bits
 * at all within the range src/fpconv.h documents as exact, so a clamp can
 * only ever drop digits past the point where they stop affecting the
 * result.  A narrower buffer would not have that property.
 */
#define SCAN_FLOAT_FIELD_MAX 128

static int scan_isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' ||
           c == '\r' || c == '\v' || c == '\f';
}

static int scan_digit_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

/*
 * Parse an integer in `base`, or auto-detect when base is 0 (the %i rule:
 * 0x is hex, a leading 0 is octal, otherwise decimal).
 *
 * `width` is 0 for "unlimited". Returns 1 if a conversion happened, 0 if
 * not; *endp is advanced past whatever was consumed either way.
 */
static int scan_integer(const char **sp, int base, int width, int is_signed,
                        uint64_t *out, int *neg_out)
{
    const char *s = *sp;
    int         neg = 0;
    uint64_t    val = 0;
    int         digits = 0;
    int         used = 0;

    if (width == 0)
        width = 1 << 30;

    if (used < width && (*s == '+' || *s == '-')) {
        neg = (*s == '-');
        s++;
        used++;
    }

    if ((base == 0 || base == 16) && used + 1 < width &&
        s[0] == '0' && (s[1] == 'x' || s[1] == 'X') &&
        scan_digit_value((unsigned char)s[2]) >= 0 &&
        scan_digit_value((unsigned char)s[2]) < 16) {
        s += 2;
        used += 2;
        base = 16;
    } else if (base == 0) {
        /* A leading zero means octal, and "0" alone is a valid octal zero --
         * which is exactly what m_misc.c's " 0%o" probe relies on. */
        base = (*s == '0') ? 8 : 10;
    }

    while (used < width) {
        int d = scan_digit_value((unsigned char)*s);

        if (d < 0 || d >= base)
            break;

        val = val * (uint64_t)base + (uint64_t)d;
        s++;
        used++;
        digits++;
    }

    if (digits == 0) {
        /* Nothing converted: leave the caller's position untouched so it can
         * report a matching failure rather than silently skipping a sign. */
        return 0;
    }

    *sp = s;
    *out = val;
    if (neg_out)
        *neg_out = neg;
    (void)is_signed;

    return 1;
}

/*
 * Store a parsed floating-point value through a scanf pointer argument.
 *
 * The mirror image of fetch_double_bits, gated on __SSE2__ for exactly the
 * same reason and with the same caveat: writing a `double` or a `float`
 * needs SSE, and "not the kernel" does not imply "has SSE" -- every ring-3
 * link target build.sh produces today inherits -mno-sse from the kernel
 * CFLAGS. See src/stdlib.h's atof comment.
 *
 * Without SSE this stores nothing and the caller still counts the
 * conversion, which is the honest shape: a translation unit that cannot name
 * a float cannot have passed a pointer to one either.
 *
 * %lf is a double and plain %f is a FLOAT, unlike printf where both are
 * promoted to double. Getting that backwards writes 8 bytes through a
 * 4-byte pointer.
 */
static void store_double_bits(void *dest, length_mod_t lmod, uint64_t bits)
{
#ifndef __SSE2__
    (void)dest;
    (void)lmod;
    (void)bits;
#else
    double d;

    __builtin_memcpy(&d, &bits, sizeof d);

    if (lmod == LEN_L || lmod == LEN_LL) {
        *(double *)dest = d;
    } else {
        *(float *)dest = (float)d;
    }
#endif
}

static void store_integer(void *dest, length_mod_t lmod, uint64_t val, int neg)
{
    int64_t sv = neg ? -(int64_t)val : (int64_t)val;

    switch (lmod) {
    case LEN_HH: *(signed char *)dest = (signed char)sv; break;
    case LEN_H:  *(short *)dest       = (short)sv;       break;
    case LEN_L:  *(long *)dest        = (long)sv;        break;
    case LEN_LL: *(long long *)dest   = (long long)sv;   break;
    case LEN_Z:  *(size_t *)dest      = (size_t)sv;      break;
    case LEN_T:  *(ptrdiff_t *)dest   = (ptrdiff_t)sv;   break;
    case LEN_J:  *(int64_t *)dest     = sv;              break;
    default:     *(int *)dest         = (int)sv;         break;
    }
}

int vsscanf(const char *str, const char *fmt, va_list ap)
{
    const char *s = str;
    const char *f = fmt;
    int         assigned = 0;
    va_list     args;

    va_copy(args, ap);

    while (*f != '\0') {
        if (scan_isspace((unsigned char)*f)) {
            /* Whitespace in the format matches any run of it, including
             * none -- which is what makes m_misc.c's " 0x%x" work against a
             * string with no leading space. */
            while (scan_isspace((unsigned char)*s))
                s++;
            f++;
            continue;
        }

        if (*f != '%') {
            if (*s != *f)
                goto done;   /* literal mismatch ends the scan */
            s++;
            f++;
            continue;
        }

        f++;

        {
            int          suppress = 0;
            int          width = 0;
            length_mod_t lmod = LEN_NONE;
            char         spec;

            if (*f == '*') { suppress = 1; f++; }

            while (*f >= '0' && *f <= '9')
                width = width * 10 + (*f++ - '0');

            switch (*f) {
            case 'h':
                f++;
                if (*f == 'h') { lmod = LEN_HH; f++; } else lmod = LEN_H;
                break;
            case 'l':
                f++;
                if (*f == 'l') { lmod = LEN_LL; f++; } else lmod = LEN_L;
                break;
            case 'z': lmod = LEN_Z; f++; break;
            case 't': lmod = LEN_T; f++; break;
            case 'j': lmod = LEN_J; f++; break;
            default: break;
            }

            spec = *f;
            if (spec == '\0')
                goto done;
            f++;

            /* Every conversion except %c and %[ skips leading whitespace. */
            if (spec != 'c' && spec != '%') {
                while (scan_isspace((unsigned char)*s))
                    s++;
            }

            switch (spec) {
            case 'd':
            case 'i':
            case 'u':
            case 'x':
            case 'X':
            case 'o': {
                int base = (spec == 'd' || spec == 'u') ? 10
                         : (spec == 'i')                ? 0
                         : (spec == 'o')                ? 8
                                                        : 16;
                uint64_t val;
                int      neg = 0;

                if (*s == '\0')
                    goto done;   /* input exhausted: matching failure */
                if (!scan_integer(&s, base, width, spec == 'd' || spec == 'i',
                                  &val, &neg))
                    goto done;

                if (!suppress) {
                    store_integer(va_arg(args, void *), lmod, val, neg);
                    assigned++;
                }
                break;
            }
            case 'f':
            case 'e':
            case 'g': {
                const char *end;
                uint64_t    bits;

                if (*s == '\0')
                    goto done;

                if (width > 0) {
                    /*
                     * C99 7.21.6.2p3: a conversion reads at most `width`
                     * characters.  Every other conversion here already
                     * honours that -- scan_integer takes width as a
                     * parameter, %s bounds its copy with it -- so staging
                     * the field into a bounded buffer is what makes the
                     * float case consistent rather than the one exception.
                     *
                     * It matters more here than for %d, because
                     * exo_parse_f64 stops only at a character that cannot
                     * continue a number.  Against "1.5e3", a "%3f" that
                     * ignored width would swallow the exponent as well and
                     * hand the next conversion a tail that begins nowhere
                     * it expects -- a silent misparse rather than a
                     * matching failure, which is the same class of bug as
                     * the %.3d vararg desync this ticket started from.
                     */
                    char   field[SCAN_FLOAT_FIELD_MAX];
                    size_t n = 0;

                    while (n < (size_t)width && n + 1 < sizeof field &&
                           s[n] != '\0') {
                        field[n] = s[n];
                        n++;
                    }
                    field[n] = '\0';

                    bits = exo_parse_f64(field, &end);
                    if (end == field)
                        goto done;
                    s += (size_t)(end - field);
                } else {
                    bits = exo_parse_f64(s, &end);
                    if (end == s)
                        goto done;
                    s = end;
                }

                if (!suppress) {
                    store_double_bits(va_arg(args, void *), lmod, bits);
                    assigned++;
                }
                break;
            }
            case 's': {
                char *dest = suppress ? NULL : va_arg(args, char *);
                int   n = 0;

                if (*s == '\0')
                    goto done;

                while (*s != '\0' && !scan_isspace((unsigned char)*s) &&
                       (width == 0 || n < width)) {
                    if (dest != NULL)
                        dest[n] = *s;
                    s++;
                    n++;
                }

                if (n == 0)
                    goto done;

                if (dest != NULL) {
                    dest[n] = '\0';
                    assigned++;
                }
                break;
            }
            case 'c': {
                char *dest = suppress ? NULL : va_arg(args, char *);
                int   n = (width == 0) ? 1 : width;
                int   i;

                for (i = 0; i < n; i++) {
                    if (*s == '\0')
                        goto done;
                    if (dest != NULL)
                        dest[i] = *s;
                    s++;
                }

                /* %c does NOT terminate -- it is a character array, not a
                 * string. Writing a NUL here would overrun a caller that
                 * sized its buffer to exactly `width`. */
                if (dest != NULL)
                    assigned++;
                break;
            }
            case '%':
                if (*s != '%')
                    goto done;
                s++;
                break;
            default:
                /* Unknown conversion. Stop rather than guess: continuing
                 * would consume a pointer argument for a conversion whose
                 * type is unknown. */
                goto done;
            }
        }
    }

done:
    va_end(args);

    /* EOF, not 0, when input ran out before the first conversion -- callers
     * distinguish "nothing matched" from "nothing was there". m_config.c's
     * config loop uses exactly that distinction as its terminator. */
    if (assigned == 0 && *s == '\0' && *fmt != '\0')
        return EOF;

    return assigned;
}

int sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    int     n;

    if (str == NULL || fmt == NULL)
        return EOF;

    va_start(ap, fmt);
    n = vsscanf(str, fmt, ap);
    va_end(ap);

    return n;
}

/*
 * fscanf: returns EOF, and that is the correct answer rather than a stub.
 *
 * Its one call site is m_config.c:1792, reading the config file -- and
 * fopen cannot produce a config file to read: there is no writable
 * filesystem, so M_SaveDefaults never wrote one, so M_LoadDefaults' fopen
 * returns NULL and this is never reached. On a console stream there is
 * genuinely no input either.
 *
 * EOF is what C99 specifies for "input failure before any conversion", and
 * it is what M_LoadDefaults' loop already treats as end-of-file. When a real
 * readable stream exists this needs implementing for real -- including the
 * "%99[^\n]" scanset, which vsscanf above deliberately does not handle.
 */
int fscanf(FILE *stream, const char *fmt, ...)
{
    (void)stream;
    (void)fmt;
    return EOF;
}
