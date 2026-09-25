#include "syscall_stat.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "page_alloc.h"
#include "context.h"

#include <stddef.h>
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
        ps_out[i].page_count = page_count_owned(infos[i].id);
    }

    return (int64_t)count;
}

void syscall_stat_init(void)
{
    exo_syscall_register(EXO_SYS_MEMSTAT, sys_memstat);
    exo_syscall_register(EXO_SYS_PSLIST,  sys_pslist);
}
