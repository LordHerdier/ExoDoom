#include "syscall.h"
#include "context.h"
#include "exo_syscall.h"
#include "msr.h"
#include "vmm.h"

#ifndef TESTING
/* SCRUM-181: fb_compositor_service() is called from exo_syscall_dispatch()
 * below. Guarded like pit.c's old direct call used to be -- the
 * framebuffer, fb_binding and fb_shadow are never initialized under a
 * TESTING build (src/kernel.c exits before that). */
#include "fb_compositor.h"
#endif

/*
 * syscall.c — MSR setup and syscall dispatch (SCRUM-32).
 *
 * The asm entry stub in syscall_entry.s does the mechanical work — stack
 * swap, register preservation, argument marshalling.  Everything policy-ish
 * lives here.
 */

/* ── Segment selectors ────────────────────────────────────────────────────
 *
 * These must match the GDT laid out in src/boot.s, and the layout is not
 * free: `syscall` loads CS from STAR[47:32] and SS from STAR[47:32] + 8,
 * while `sysretq` loads CS from STAR[63:48] + 16 and SS from STAR[63:48] + 8.
 * Both halves therefore name the *base* of a descriptor pair, not the
 * selector that ends up loaded.
 *
 * The RPL bits in the sysret half are conventional rather than load-bearing
 * — sysret forces CPL 3 either way — but they are set so a reader comparing
 * this against the selectors observed in a debugger sees the same values.
 */
#define SEL_KERNEL_BASE  0x08u          /* -> CS 0x08, SS 0x10 on syscall  */
#define SEL_USER_BASE    (0x18u | 3u)   /* -> SS 0x20, CS 0x28 on sysret   */

/*
 * RFLAGS bits cleared on syscall entry, so the kernel never inherits a
 * hostile flags word from the caller:
 *
 *   IF — run the dispatcher with interrupts off.  The entry stub keeps the
 *        outgoing user RSP in a single global, so it is not reentrant; this
 *        is what makes that safe.
 *   DF — string instructions must count upward, per the SysV ABI, and the
 *        compiler assumes it without checking.
 *   TF — a LibOS setting the trap flag must not single-step kernel code.
 *   AC — alignment checking off, so a misaligned kernel access cannot be
 *        weaponised into a fault by the caller.
 *   NT — a stale nested-task flag corrupts IRET.
 */
#define SYSCALL_FMASK  (0x200u  /* IF */ | 0x400u /* DF */ | 0x100u /* TF */ \
                        | 0x40000u /* AC */ | 0x4000u /* NT */)

/* Handler table.  NULL means "number is in range but nothing implements it
 * yet", which the dispatcher reports identically to an out-of-range number:
 * -EXO_ENOSYS.  All 21 entries start NULL — SCRUM-32 delivers the road, not
 * the traffic. */
static exo_handler_t handlers[EXO_SYS_COUNT];

page_owner_t syscall_current_context(void)
{
    /* context_current() (src/context.h) defaults to PAGE_OWNER_LIBOS until
     * the first SCRUM-108 context_switch_request() ever succeeds, so this
     * matches v1's single-LibOS behavior exactly until something actually
     * switches. */
    return context_current();
}

int exo_user_range_mapped(uint64_t base, uint64_t len, int writable)
{
    /* Mirrors exo_range_in_user_window's len==0 rule: an empty range can't
     * touch memory that isn't there. */
    if (len == 0)
        return 1;

    /* Deliberately CR3 itself, not vmm_address_space_for(syscall_current_
     * context()): `syscall` never switches CR3 (only the caller's own
     * exo_page_map/-unmap calls change what it points at), so whatever is
     * loaded there right now is exactly the tree a write through `base`
     * would really be checked against -- the thing this function exists to
     * answer. The registry lookup is a different question ("who owns the
     * address space bound to this context id"), which several ring-3 test
     * harnesses (tests/kernel/libos_test_common.h) deliberately answer
     * differently from "what's actually in CR3" by launching under a
     * scratch owner id that is never bound to PAGE_OWNER_LIBOS -- reading
     * CR3 directly gives the right answer for both v1's single real LibOS
     * and every such test harness, with no dependency on that registry
     * being kept in sync with reality. */
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *root = (uint64_t *)(uintptr_t)(cr3 & ~0xFFFULL);

    /* The caller already ran exo_range_in_user_window(), so [base, base+len)
     * neither wraps nor leaves the LibOS window -- this only has to walk it
     * one page at a time and confirm every page is really there. */
    uint64_t start = base & ~(uint64_t)(VMM_PAGE_SIZE - 1);
    uint64_t end   = base + len;
    uint64_t need  = VMM_USER | (writable ? VMM_WRITE : 0);

    for (uint64_t page = start; page < end; page += VMM_PAGE_SIZE) {
        uint64_t flags;
        if (vmm_translate_in(root, page, NULL, &flags) != VMM_OK)
            return 0;

        if ((flags & need) != need)
            return 0;
    }

    return 1;
}

void syscall_init(void)
{
    /* Read-modify-write: boot.s already set LME here, and clobbering it
     * would drop the CPU out of long mode on the next mode-affecting
     * event. */
    wrmsr(MSR_IA32_EFER, rdmsr(MSR_IA32_EFER) | EFER_SCE);

    wrmsr(MSR_IA32_STAR, ((uint64_t)SEL_USER_BASE << 48) |
                         ((uint64_t)SEL_KERNEL_BASE << 32));

    wrmsr(MSR_IA32_LSTAR, (uint64_t)(uintptr_t)&syscall_entry);

    wrmsr(MSR_IA32_FMASK, SYSCALL_FMASK);
}

void exo_syscall_register(uint64_t num, exo_handler_t fn)
{
    if (num >= EXO_SYS_COUNT)
        return;

    handlers[num] = fn;
}

exo_handler_t exo_syscall_handler(uint64_t num)
{
    if (num >= EXO_SYS_COUNT)
        return 0;

    return handlers[num];
}

int64_t exo_syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5,
                             uint64_t a6)
{
#ifndef TESTING
    /* SCRUM-181: every LibOS syscall re-enters here, and every LibOS today
     * polls exo_kbd_poll()/exo_get_ticks() every loop iteration (docs/
     * syscall_spec.md §3.5), so this is a frequent, already-there hook to
     * service a deferred compositor tick from -- unconditionally, ahead of
     * the range check below, so it runs regardless of which syscall number
     * got us here, mirroring irq0_handler()'s old unconditional call. */
    fb_compositor_service();
#endif

    /* The number arrives as a full 64-bit RAX straight from ring 3, so the
     * range check is the only thing standing between a hostile LibOS and an
     * arbitrary read off the end of the table.  Unsigned compare — a
     * "negative" number is a huge unsigned one and fails the same way. */
    if (num >= EXO_SYS_COUNT)
        return -EXO_ENOSYS;

    exo_handler_t fn = handlers[num];

    if (!fn)
        return -EXO_ENOSYS;

    return fn(a1, a2, a3, a4, a5, a6);
}
