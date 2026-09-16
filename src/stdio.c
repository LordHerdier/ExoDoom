/*
 * stdio.c — freestanding printf routed to COM1 serial (SCRUM-20).
 *
 * The formatting engine (kvprintf) is sink-based: it emits each output
 * character through a caller-supplied callback and never touches a fixed
 * output buffer, so the same engine drives printf (sink = serial) today and
 * snprintf (sink = bounded buffer) in SCRUM-21.
 */

#include "stdio.h"

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

int kvprintf(void (*emit)(int c, void *ctx), void *ctx,
             const char *fmt, va_list ap)
{
    int count = 0;
    char numbuf[20];

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            emit((unsigned char)*p, ctx);
            count++;
            continue;
        }

        p++;

        /* flags */
        int left = 0, zero = 0;
        for (;; p++) {
            if (*p == '-')      left = 1;
            else if (*p == '0') zero = 1;
            else                break;
        }

        /* field width */
        int width = 0;
        while (*p >= '0' && *p <= '9')
            width = width * 10 + (*p++ - '0');

        char spec = *p;
        if (spec == '\0') {           /* trailing '%' (e.g. "%5") */
            emit('%', ctx);
            count++;
            break;
        }

        switch (spec) {
        case 'd':
        case 'i': {
            int v = va_arg(ap, int);
            uint64_t mag = (v < 0) ? (uint64_t)(-(int64_t)v) : (uint64_t)v;
            size_t len = fmt_uint(numbuf, mag, 10, 1);
            const char *pre = (v < 0) ? "-" : "";
            count += emit_field(emit, ctx, pre, (v < 0) ? 1 : 0,
                                numbuf, len, width, left, zero);
            break;
        }
        case 'u': {
            unsigned v = va_arg(ap, unsigned);
            size_t len = fmt_uint(numbuf, v, 10, 1);
            count += emit_field(emit, ctx, "", 0, numbuf, len,
                                width, left, zero);
            break;
        }
        case 'x': {
            unsigned v = va_arg(ap, unsigned);
            size_t len = fmt_uint(numbuf, v, 16, 1);
            count += emit_field(emit, ctx, "", 0, numbuf, len,
                                width, left, zero);
            break;
        }
        case 'p': {
            void *ptr = va_arg(ap, void *);
            size_t len = fmt_uint(numbuf, (uint64_t)(uintptr_t)ptr, 16, 1);
            count += emit_field(emit, ctx, "0x", 2, numbuf, len,
                                width, left, zero);
            break;
        }
        case 'c': {
            char ch = (char)va_arg(ap, int);
            count += emit_field(emit, ctx, "", 0, &ch, 1, width, left, 0);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            count += emit_field(emit, ctx, "", 0, s, kstrlen(s),
                                width, left, 0);
            break;
        }
        case '%': {
            count += emit_field(emit, ctx, "", 0, "%", 1, width, left, 0);
            break;
        }
        default:
            /* Unknown specifier: emit the '%' and the char verbatim. */
            emit('%', ctx);
            emit((unsigned char)spec, ctx);
            count += 2;
            break;
        }
    }

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
 * formatter of its own for the I_Error path.
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
