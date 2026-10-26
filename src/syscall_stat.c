#include "syscall_stat.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "page_alloc.h"
#include "context.h"
#include "fb_binding.h"
#include "pcm_mixer.h"
#include "disk_binding.h"
#include "syscall_sound.h"

#include <stddef.h>
#include <string.h>
#include <stdint.h>

/*
 * syscall_stat.c — exo_memstat (#22) / exo_pslist (#23), SCRUM-113.
 *
 * Neither handler is ownership-checked the way src/syscall_mem.c's are: both
 * are deliberately unscoped introspection (see exo_syscall.h's own comment
 * on exo_memstat/exo_pslist) rather than a resource grant, so there is
 * nothing here to enforce beyond "is the output pointer usable" — the same
 * exo_range_in_user_window() + exo_user_range_mapped() pair every other
 * output-pointer handler in this codebase runs (src/syscall_fb.c's
 * sys_fb_acquire).
 */

/* #22 — page-usage snapshot of the whole PMM.
 *   0             *out filled
 *   -EXO_EFAULT   [out, out + sizeof(exo_memstat_t)) is not entirely inside
 *                 the LibOS window and mapped writable */
static int64_t sys_memstat(uint64_t out, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!exo_range_in_user_window(out, sizeof(exo_memstat_t)) ||
        !exo_user_range_mapped(out, sizeof(exo_memstat_t), 1))
        return -EXO_EFAULT;

    exo_memstat_t *stat_out = (exo_memstat_t *)(uintptr_t)out;

    page_alloc_totals(&stat_out->total_pages, &stat_out->free_pages,
                      &stat_out->kernel_pages, &stat_out->libos_pages);
    stat_out->region_count = page_alloc_region_count();
    stat_out->reserved = 0;

    return 0;
}

/* #38 — free-resource snapshot for a launcher's admission check (SCRUM-232).
 *   0             *out filled
 *   -EXO_EFAULT   [out, out + sizeof(exo_resstat_t)) is not entirely inside
 *                 the LibOS window and mapped writable
 * Reports only; whether a program fits is the launcher's decision. */
static int64_t sys_resstat(uint64_t out, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!exo_range_in_user_window(out, sizeof(exo_resstat_t)) ||
        !exo_user_range_mapped(out, sizeof(exo_resstat_t), 1))
        return -EXO_EFAULT;

    exo_resstat_t *r = (exo_resstat_t *)(uintptr_t)out;

    uint32_t total, free_pages, kernel, libos;
    page_alloc_totals(&total, &free_pages, &kernel, &libos);
    uint32_t live = context_count();
    uint32_t active = pcm_mixer_active_voices();

    r->free_pages = free_pages;
    r->free_contexts = live < CONTEXT_MAX ? CONTEXT_MAX - live : 0;
    r->sound_voices_free = active < PCM_MIXER_VOICES ? PCM_MIXER_VOICES - active : 0;
    r->fb_held = fb_binding_owner() != PAGE_OWNER_FREE;
    for (int i = 0; i < 4; i++)
        r->reserved[i] = 0;

    return 0;
}

/* #39 — what `ctx` was granted (SCRUM-235).
 *   0             *out filled
 *   -EXO_ENOENT   no live context with that id
 *   -EXO_EFAULT   [out, out + sizeof(exo_grant_t)) is not entirely inside the
 *                 LibOS window and mapped writable
 * Every field is read from the table that already enforces it; nothing is
 * recorded for this call. Pointer first, then the id, like the other
 * introspection handlers: a bad pointer is the same error for any id. */
static int64_t sys_grantstat(uint64_t ctx, uint64_t out, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!exo_range_in_user_window(out, sizeof(exo_grant_t)) ||
        !exo_user_range_mapped(out, sizeof(exo_grant_t), 1))
        return -EXO_EFAULT;

    if (ctx == 0 || ctx > PAGE_OWNER_ID_MASK ||
        context_lookup((page_owner_t)ctx) == NULL)
        return -EXO_ENOENT;

    page_owner_t id = (page_owner_t)ctx;
    exo_grant_t *g = (exo_grant_t *)(uintptr_t)out;

    /* A context can be assigned a private volume and the program volume
     * (SCRUM-267), and each is read out under its own name. */
    uint32_t base = 0, count = 0;
    if (disk_binding_assignment_role(id, 0, &base, &count) != DISK_BIND_OK) {
        base = 0;
        count = 0;
    }
    uint32_t pbase = 0, pcount = 0;
    if (disk_binding_assignment_role(id, 1, &pbase, &pcount) != DISK_BIND_OK) {
        pbase = 0;
        pcount = 0;
    }

    g->ctx_id       = id;
    g->pages        = page_count_owned(id);
    g->fb_owner     = fb_binding_owner();
    g->disk_base    = base;
    g->disk_sectors = count;
    g->sound_voices = syscall_sound_voices_of(id);
    g->sound_stream = syscall_sound_stream_holder() == id;
    g->prog_base    = pbase;
    g->prog_sectors = pcount;
    g->reserved     = 0;

    return 0;
}

/* #23 — list every live LibOS context into `out`, an array of at least
 * `max` exo_ps_info_t entries.
 *   >= 0          number of entries written (0..max)
 *   -EXO_EFAULT   [out, out + max * sizeof(exo_ps_info_t)) is not entirely
 *                 inside the LibOS window and mapped writable
 *   -EXO_EINVAL   max > EXO_PSLIST_MAX
 *
 * max is checked against EXO_PSLIST_MAX before anything else so the
 * on-stack context_info_t buffer below is always large enough — the same
 * "reject a bad argument before touching state" order sys_page_map uses. */
static int64_t sys_pslist(uint64_t out, uint64_t max, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (max > EXO_PSLIST_MAX)
        return -EXO_EINVAL;

    uint64_t len = max * (uint64_t)sizeof(exo_ps_info_t);
    if (!exo_range_in_user_window(out, len) ||
        !exo_user_range_mapped(out, len, 1))
        return -EXO_EFAULT;

    context_info_t infos[EXO_PSLIST_MAX];
    uint32_t count = context_list(infos, (uint32_t)max);

    exo_ps_info_t *ps_out = (exo_ps_info_t *)(uintptr_t)out;
    for (uint32_t i = 0; i < count; i++) {
        ps_out[i].id = infos[i].id;
        ps_out[i].state = (uint8_t)infos[i].state;
        ps_out[i].reserved = 0;
        memcpy(ps_out[i].name, infos[i].name, CONTEXT_NAME_MAX);
        ps_out[i].page_count = page_count_owned(infos[i].id);
    }

    return (int64_t)count;
}

void syscall_stat_init(void)
{
    exo_syscall_register(EXO_SYS_MEMSTAT, sys_memstat);
    exo_syscall_register(EXO_SYS_PSLIST,  sys_pslist);
    exo_syscall_register(EXO_SYS_RESSTAT, sys_resstat);
    exo_syscall_register(EXO_SYS_GRANTSTAT, sys_grantstat);
}
