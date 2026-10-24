#ifndef EXO_ADMIT_H
#define EXO_ADMIT_H

/*
 * exo_admit -- the launch admission check (SCRUM-232).
 *
 * A package (src/exo_pkg.h) declares what it needs; exo_resstat (#38,
 * src/exo_syscall.h) reports what is free. This compares the two. The kernel
 * supplies the numbers and nothing else: whether a program fits, runs
 * degraded or is turned away is the LAUNCHER's decision, which is why this is
 * a header the ring-3 shell includes rather than a kernel function (the same
 * reason src/exo_pkg.h is header-only). The kernel keeps only a backstop in
 * exo_launch_pkg (#36) for a launcher that skips the check.
 *
 * Pure: no syscalls, no globals, <stdint.h> only, so the KUnit suite can
 * drive every boundary with hand-built structs.
 *
 * What is and is not a refusal:
 *   - pages below min_pages           -> REFUSED (EXO_ADMIT_PAGES)
 *   - no free context slot            -> REFUSED (EXO_ADMIT_CONTEXTS)
 *   - fewer free sound voices than
 *     wanted                          -> DEGRADED (EXO_ADMIT_SOUND): the
 *                                        program runs, quieter
 *   - pages in [min, recommended)     -> DEGRADED (EXO_ADMIT_PAGES)
 *   - framebuffer already held        -> still GRANTED. Displacing the holder
 *                                        is the designed launch behaviour
 *                                        (SCRUM-196); `fb_displaces` reports
 *                                        it so the launcher can say so.
 *   - disk / keyboard                 -> informational, never a refusal.
 * Refusal wins over degraded; when both pages and contexts fail, contexts is
 * reported (a free slot cannot be bought with pages, pages sometimes can).
 */

#include <stdint.h>

#include "exo_pkg.h"
#include "exo_syscall.h"

typedef enum {
    EXO_ADMIT_GRANT    = 0,
    EXO_ADMIT_DEGRADED = 1,
    EXO_ADMIT_REFUSED  = 2,
} exo_admit_verdict_t;

typedef enum {
    EXO_ADMIT_NONE     = 0,
    EXO_ADMIT_PAGES    = 1,
    EXO_ADMIT_CONTEXTS = 2,
    EXO_ADMIT_SOUND    = 3,
} exo_admit_reason_t;

typedef struct {
    exo_admit_verdict_t verdict;
    exo_admit_reason_t  reason;  /* what decided a non-GRANT verdict */
    uint32_t needed;             /* the figure that was short ... */
    uint32_t have;               /* ... and what is free. needed/have are
                                  * min_pages vs free_pages for a refusal,
                                  * recommended_pages vs free_pages for a
                                  * pages-degraded grant. */
    uint32_t fb_displaces;       /* 1 if the program wants the framebuffer
                                  * and someone else holds it */
} exo_admit_t;

static inline void exo_admit(const exo_pkg_header_t *hdr,
                             const exo_resstat_t *res, exo_admit_t *out)
{
    out->verdict = EXO_ADMIT_GRANT;
    out->reason = EXO_ADMIT_NONE;
    out->needed = 0;
    out->have = 0;
    out->fb_displaces = (hdr->want_flags & EXO_PKG_WANT_FRAMEBUFFER) &&
                        res->fb_held;

    if (res->free_contexts < 1) {
        out->verdict = EXO_ADMIT_REFUSED;
        out->reason = EXO_ADMIT_CONTEXTS;
        out->needed = 1;
        out->have = res->free_contexts;
        return;
    }
    if (res->free_pages < hdr->min_pages) {
        out->verdict = EXO_ADMIT_REFUSED;
        out->reason = EXO_ADMIT_PAGES;
        out->needed = hdr->min_pages;
        out->have = res->free_pages;
        return;
    }
    if (res->free_pages < hdr->recommended_pages) {
        out->verdict = EXO_ADMIT_DEGRADED;
        out->reason = EXO_ADMIT_PAGES;
        out->needed = hdr->recommended_pages;
        out->have = res->free_pages;
        return;
    }
    if (res->sound_voices_free < hdr->sound_voices) {
        out->verdict = EXO_ADMIT_DEGRADED;
        out->reason = EXO_ADMIT_SOUND;
        out->needed = hdr->sound_voices;
        out->have = res->sound_voices_free;
    }
}

#endif /* EXO_ADMIT_H */
