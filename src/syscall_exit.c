#include "syscall_exit.h"

#include <stdint.h>

#include "context.h"
#include "exo_syscall.h"
#include "fb_binding.h"
#include "fb_shadow.h"
#include "page_alloc.h"
#include "syscall.h"
#include "syscall_sound.h"

/*
 * SCRUM-155 — resource reclamation on LibOS exit.
 *
 * The ownership tag is the authority for deciding which resources belong to
 * the terminating context.  Never reclaim PAGE_OWNER_FREE or
 * PAGE_OWNER_KERNEL.
 *
 * Page-map revocation and file-descriptor teardown are added by their owning
 * subsystems once those facilities exist.  Neither subsystem is implemented
 * on the current main branch.
 *
 * SCRUM-178 extends this with the switch-away this ticket's own comment
 * originally called out as missing ("v1 has no scheduler/context
 * destruction path yet, so there is currently nowhere safe for the kernel
 * to transfer control after teardown") -- true when this was written
 * (exo_yield/#19 was not yet bound on main), but SCRUM-107/108's context
 * table and switch primitive were already in the tree, and SCRUM-109 binds
 * exo_yield to them on this branch. Without this, a context that calls
 * exo_exit() keeps running on memory this handler just reclaimed -- fine by
 * pure luck as long as nothing else claims those physical pages before the
 * caller stops touching them, but not something to rely on. Reusing
 * context_next_ready()'s exact round-robin policy rather than inventing a
 * second one: this is the same "hand off to whatever's next" decision
 * sys_yield() (src/syscall_yield.c) already makes, just triggered by exit
 * instead of a voluntary yield. Does not context_destroy() the caller's own
 * row: context_switch_request() below still needs to find it (as the
 * outgoing side) to capture into, so there is no point in this call's own
 * control flow where the row could safely be torn down without racing that
 * requirement -- it is left resourceless instead, for whoever launches the
 * next LibOS into its place to context_destroy() once it is no longer live
 * (see src/syscall_launch.c's WAD viewer launcher for the one caller that
 * does).
 *
 * SCRUM-111 found the actual hazard that leaves this row in: context_
 * switch_request() unconditionally sets the outgoing side to CONTEXT_STATE_
 * READY, which is correct for exo_yield() (the outgoing context really is
 * safe to resume later) but wrong here -- this context has no valid
 * continuation, its pages and framebuffer are gone. Before this ticket
 * nothing was ever positioned to notice: context_next_ready() was only ever
 * driven by something the *outgoing* context itself called (another
 * exo_yield, another exo_exit), and control never returns to a dead
 * context's own code to make that call. SCRUM-111's Ctrl+Tab hotkey drives
 * context_next_ready()/context_switch_request() from the keyboard IRQ
 * instead, entirely independent of which context is actually running --
 * round-robining straight into this READY-but-resourceless row from the
 * shell right after quitting a viewer, executing whatever garbage its freed
 * pages now hold. Downgrading to CONTEXT_STATE_BLOCKED immediately after
 * the handoff closes this for exo_yield()'s own round-robin too, not just
 * Ctrl+Tab's: context_next_ready() already skips BLOCKED rows (see its own
 * comment in context.h), and nothing here ever un-blocks this one -- it
 * stays inert until context_destroy() reaps it, exactly as already
 * documented above.
 */
static int64_t sys_exit(uint64_t code, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)code;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;

    page_owner_t owner = syscall_current_context();

    /*
     * Defense in depth.  A terminating LibOS must never be allowed to trigger
     * reclamation of the FREE sentinel or kernel-owned pages.
     */
    if (owner == PAGE_OWNER_FREE || owner == PAGE_OWNER_KERNEL)
        return -EXO_EPERM;

    /*
     * Release non-page bindings before returning physical pages to the free
     * pool.  Future mapping teardown must also happen before page reclamation
     * so no surviving context retains a mapping to a recycled page.
     *
     * fb_binding_release() is a no-op in v1 (SCRUM-112): nothing acquires
     * that binding from the syscall path anymore, see src/syscall_fb.c.
     * fb_shadow_release() drops this context's virtual-framebuffer directory
     * entry -- the underlying pages are ordinary owner-tagged pages, freed
     * by reclaim_pages_owned() below like any other page this context held.
     */
    fb_binding_release(owner);
    fb_shadow_release(owner);
    /* SCRUM-100: an exiting LibOS's last effect must not keep playing into
     * whoever runs next. Tones are bounded (SOUND_TONE_MAX_MS) regardless,
     * but that bound is seconds, not "now". */
    syscall_sound_release(owner);
    (void)reclaim_pages_owned(owner);

    /* SCRUM-178: hand off to whatever else is ready, exactly like
     * exo_yield() -- if nothing is, there is still nowhere to send control,
     * matching this handler's original v1 behavior for that case. */
    page_owner_t next = context_next_ready(owner);
    if (next != PAGE_OWNER_FREE) {
        if (context_switch_request(next) == CONTEXT_OK) {
            /* SCRUM-111: override the CONTEXT_STATE_READY context_switch_
             * request() just set for `owner` -- see this file's own top
             * comment for why READY is wrong for an exited context and
             * BLOCKED is what keeps it out of every future round-robin. */
            context_set_state(owner, CONTEXT_STATE_BLOCKED);
        }
    }

    return 0;
}

void syscall_exit_init(void)
{
    exo_syscall_register(EXO_SYS_EXIT, sys_exit);
}
