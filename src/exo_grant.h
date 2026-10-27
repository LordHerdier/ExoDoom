#ifndef EXO_GRANT_H
#define EXO_GRANT_H

/*
 * exo_grant -- the launch-time resource read-out (SCRUM-235).
 *
 * A package (src/exo_pkg.h) says what it asked for; exo_grantstat (#39,
 * src/exo_syscall.h) says what its context actually holds. This puts the two
 * side by side as text, so the moment a program starts is also the moment its
 * resources are named, owned and handed over in plain view.
 *
 * Header-only and pure for the reason src/exo_admit.h is: no syscalls, no
 * globals, <stdint.h>/<stddef.h> only, so the ring-3 shell and the KUnit
 * suite use the same code and every line can be checked against hand-built
 * structs. The formatter writes into a caller's buffer and never past it.
 *
 * Every function is always_inline, and that is load-bearing: the ring-3 shell
 * is linked with -fno-toplevel-reorder and libos_build_image() takes byte 0 of
 * its code as the entry point (docker/scripts/build.sh), so a helper from this
 * header that GCC emitted out of line would land ahead of shell_main() and be
 * run as the program's entry. Inlined, nothing is emitted here at all.
 *
 * Nothing here decides anything. Whether the program was admitted is
 * exo_admit()'s job and has already happened.
 */

#include <stddef.h>
#include <stdint.h>

#include "exo_pkg.h"
#include "exo_syscall.h"

/* Worst case is about 330 bytes (every number at its maximum, both a private
 * and a program volume, SCRUM-267); callers size their buffer with this. */
#define EXO_GRANT_TEXT_MAX 384

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;   /* bytes written, excluding the NUL */
} exo_grant_out_t;

#define EXO_GRANT_INLINE static inline __attribute__((always_inline))

EXO_GRANT_INLINE void exo_grant_puts(exo_grant_out_t *o, const char *s)
{
    while (*s != '\0' && o->len + 1 < o->cap) {
        o->buf[o->len++] = *s++;
    }
    o->buf[o->len] = '\0';
}

EXO_GRANT_INLINE void exo_grant_putu(exo_grant_out_t *o, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    char s[11];
    int k = 0;
    while (n > 0) {
        s[k++] = tmp[--n];
    }
    s[k] = '\0';
    exo_grant_puts(o, s);
}

/*
 * Format the read-out for one launch into buf (NUL-terminated, at most `cap`
 * bytes including the NUL); returns the length written.
 *
 *   hdr      the package's manifest: what it asked for
 *   g        exo_grantstat() for the new context: what it holds
 *   res      exo_resstat() taken after the context was built: what is left,
 *            which is how many sound voices are still free to be started
 *   fb_prev  the context that held the framebuffer binding before this
 *            launch, 0 if none -- the binding moves when the program is
 *            entered, so `g` still shows the old holder
 *
 * The framebuffer line says what WILL happen, because the binding moves only
 * once the program is entered (src/syscall_launch.c, #40); a hold that is
 * never started displaces nobody.
 */
EXO_GRANT_INLINE size_t exo_grant_format(char *buf, size_t cap,
                                         const exo_pkg_header_t *hdr,
                                         const exo_grant_t *g,
                                         const exo_resstat_t *res,
                                         uint32_t fb_prev)
{
    exo_grant_out_t o = { buf, cap, 0 };
    if (cap == 0) {
        return 0;
    }
    buf[0] = '\0';

    exo_grant_puts(&o, "granted: context ");
    exo_grant_putu(&o, g->ctx_id);
    exo_grant_puts(&o, "\n  pages  ");
    exo_grant_putu(&o, g->pages);
    exo_grant_puts(&o, " (asked min ");
    exo_grant_putu(&o, hdr->min_pages);
    exo_grant_puts(&o, ", rec ");
    exo_grant_putu(&o, hdr->recommended_pages);
    exo_grant_puts(&o, ")\n  fb     ");
    if ((hdr->want_flags & EXO_PKG_WANT_FRAMEBUFFER) == 0) {
        exo_grant_puts(&o, "not asked for");
    } else if (fb_prev == 0) {
        exo_grant_puts(&o, "binding free, program may take it");
    } else {
        exo_grant_puts(&o, "takes the binding from context ");
        exo_grant_putu(&o, fb_prev);
    }
    exo_grant_puts(&o, "\n  disk   ");
    if (g->disk_sectors == 0 && g->prog_sectors == 0) {
        exo_grant_puts(&o, (hdr->want_flags & EXO_PKG_WANT_DISK) ? "asked for, none assigned"
                                                                 : "none");
    }
    /* A context can hold a private volume and the read-only program volume at
     * once (SCRUM-267, Doom), so each gets its own line and neither hides the
     * other. */
    if (g->disk_sectors != 0) {
        exo_grant_puts(&o, "volume slot ");
        exo_grant_putu(&o, hdr->volume_slot);
        exo_grant_puts(&o, ", ");
        exo_grant_putu(&o, g->disk_sectors);
        exo_grant_puts(&o, " sectors from ");
        exo_grant_putu(&o, g->disk_base);
    }
    if (g->prog_sectors != 0) {
        if (g->disk_sectors != 0) {
            exo_grant_puts(&o, "\n         ");
        }
        exo_grant_puts(&o, "program volume (read-only), ");
        exo_grant_putu(&o, g->prog_sectors);
        exo_grant_puts(&o, " sectors from ");
        exo_grant_putu(&o, g->prog_base);
    }
    exo_grant_puts(&o, "\n  sound  ");
    if (hdr->sound_voices == 0) {
        exo_grant_puts(&o, "none asked for");
    } else {
        uint32_t got = res->sound_voices_free < hdr->sound_voices
                           ? res->sound_voices_free : hdr->sound_voices;
        exo_grant_putu(&o, got);
        exo_grant_puts(&o, " of ");
        exo_grant_putu(&o, hdr->sound_voices);
        exo_grant_puts(&o, " voices available");
    }
    exo_grant_puts(&o, "\n");
    return o.len;
}

#endif /* EXO_GRANT_H */
