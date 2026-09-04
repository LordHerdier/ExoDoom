#include "syscall.h"
#include "exo_syscall.h"
#include "msr.h"

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
