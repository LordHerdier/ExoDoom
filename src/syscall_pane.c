#include "syscall_pane.h"
#include "syscall.h"
#include "syscall_launch.h"
#include "exo_syscall.h"
#include "fb_layout.h"

#include <stdint.h>

/* #42 -- set or clear the pane of the context the caller holds (SCRUM-296).
 * Checked in the order the answers are useful in: a flag this kernel does not
 * know first (the caller expects behavior it will not get), then whether the
 * caller may touch `ctx` at all, then the rect, which fb_layout_set() judges
 * against the screen as it is now. */
static int64_t sys_pane_set(uint64_t ctx, uint64_t x, uint64_t y, uint64_t w,
                            uint64_t h, uint64_t flags)
{
    if ((flags & ~(uint64_t)EXO_PANE_CLEAR) != 0) {
        return -EXO_EINVAL;
    }
    if (ctx > (page_owner_t)~0u) {
        return -EXO_ENOENT;
    }
    int64_t rc = launch_hold_check((page_owner_t)ctx,
                                   syscall_current_context());
    if (rc != 0) {
        return rc;
    }

    if (flags & EXO_PANE_CLEAR) {
        fb_layout_clear((page_owner_t)ctx);
        return 0;
    }

    /* Whole 64-bit registers, so a raw caller's high bits cannot turn an
     * off-screen rect into an on-screen one by truncation. */
    if (x > UINT32_MAX || y > UINT32_MAX || w > UINT32_MAX || h > UINT32_MAX) {
        return -EXO_EINVAL;
    }
    fb_rect_t r = { (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h };
    switch (fb_layout_set((page_owner_t)ctx, &r)) {
    case FB_LAYOUT_OK:     return 0;
    case FB_LAYOUT_ENOSPC: return -EXO_ENOSPC;
    case FB_LAYOUT_EBUSY:  return -EXO_EBUSY;
    case FB_LAYOUT_EINVAL:
    default:               return -EXO_EINVAL;
    }
}

void syscall_pane_init(void)
{
    exo_syscall_register(EXO_SYS_PANE_SET, sys_pane_set);
}
