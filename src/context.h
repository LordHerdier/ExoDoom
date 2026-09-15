#ifndef CONTEXT_H
#define CONTEXT_H

#include <stddef.h>
#include <stdint.h>

#include "page_alloc.h"   /* page_owner_t */
#include "vmm.h"          /* VMM_MAX_ADDRESS_SPACES */

/*
 * context.c — the process/context table (SCRUM-107).
 *
 * src/vmm.c already tracks which PML4 a page_owner_t runs on (the
 * "per-context address-space registry", VMM_MAX_ADDRESS_SPACES entries);
 * vmm.h's own comment on that table says SCRUM-147 (this ticket's epic) is
 * expected to grow it "or replace the lookup with a field on its context
 * struct" once there is more than one entry worth tracking more than a page
 * dir for. This is that struct: one entry per live LibOS, holding not just
 * the page dir (still vmm's registry underneath -- see context_pml4()) but
 * also a saved-register area and a scheduling state, the two pieces
 * SCRUM-108 (context switch: save/restore register state + CR3 swap) needs
 * a home for and doesn't have one today.
 *
 * SCRUM-107's acceptance was narrower than a real switch: the kernel can
 * track 2+ LibOS contexts' id/page-dir/registers/state at once, proven by
 * tests/kernel/test_context_k.c. context_regs_t was a fixed save area
 * nothing wrote to.
 *
 * SCRUM-108 (this ticket) adds the switch itself: context_prime() seeds a
 * never-run context's regs with an initial iretq frame, and
 * context_switch_request() + src/context_switch.s do the save-outgoing /
 * swap-CR3 / restore-incoming work, spliced into src/syscall_entry.s's tail.
 * Deliberately NOT done here: the full `swapgs` + per-CPU
 * (IA32_KERNEL_GS_BASE) rework docs/syscall_spec.md §3.4 and
 * src/libos_launch.h's libos_enter() comment both describe. This ticket's
 * switch instead stages outgoing/incoming state through the same single
 * globals (syscall_entry.s's saved_user_rsp, read via live RCX/R11/RBX/RBP/
 * R12-R15 immediately after `call exo_syscall_dispatch` returns) that were
 * already there -- correct because IA32_FMASK clears IF for the whole
 * syscall/switch window and this kernel targets exactly one CPU, so no
 * second entry can interleave. The full swapgs rework is deferred to
 * SCRUM-176, needed once SCRUM-127 (preemptive, IRQ-driven switching) wants
 * to switch context from inside an interrupt handler with IF set. Nothing
 * here is wired into kernel_main's normal boot tail either, for the same
 * reason libos_launch.h gives for SCRUM-47/-49/-50 landing unwired: there
 * is still only one real LibOS launched on a normal boot.
 */

typedef enum {
    CONTEXT_STATE_UNUSED = 0,  /* table slot is free */
    CONTEXT_STATE_READY,       /* created, not currently running */
    CONTEXT_STATE_RUNNING,     /* currently the active context */
    CONTEXT_STATE_BLOCKED,     /* created, waiting on something */
} context_state_t;

/*
 * Saved integer register state for one context. Mirrors exactly what
 * src/libos_enter.s pushes/needs to resume a launched context: the six
 * callee-saved GPRs it pushes onto the kernel stack before iretq, plus the
 * three an iretq/syscall frame itself carries (RSP, RIP, RFLAGS).
 * context_prime() fills this in with a synthesized "first launch" frame;
 * src/context_switch.s fills it in for real on the way out of a context
 * (from live RCX/R11/saved_user_rsp/RBX/RBP/R12-R15, all still valid
 * immediately after exo_syscall_dispatch() returns -- see context.h's top
 * comment) and restores it on the way back in via iretq.
 *
 * Deliberately NOT the full 14-register set src/syscall_entry.s preserves
 * for an ordinary (non-switching) syscall: rdi/rsi/rdx/r10/r8/r9 are SysV
 * caller-saved at the C call site (exo_yield()) that triggers a switch, so
 * a context resumed after one needs only the callee-saved set plus
 * rsp/rip/rflags to satisfy that call's own ABI contract. rax is the one
 * caller-saved register that DOES need to survive the round trip: it is
 * exo_yield()'s own return value (docs/syscall_spec.md's "RAX=return"
 * convention), so whatever a resumed context finds in RAX after
 * context_switch_tail's iretq becomes the yield call's apparent result. A
 * primed (context_prime(), never-run) context relies on context_create()'s
 * memset leaving this 0, matching exo_yield()'s "returns 0 when
 * rescheduled" contract for a context's very first resume.
 */
typedef struct {
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rax;
} context_regs_t;

/* src/context_switch.s hardcodes these field offsets (no C compiler
 * available to it to compute them) -- these guards make a layout change to
 * context_regs_t a compile error there instead of a silent register
 * corruption at runtime. Keep in sync with CTX_REGS_OFF_* in
 * src/context_switch.s if this struct ever changes. */
_Static_assert(offsetof(context_regs_t, rsp) == 0,
                "context_switch.s hardcodes context_regs_t.rsp's offset");
_Static_assert(offsetof(context_regs_t, rip) == 8,
                "context_switch.s hardcodes context_regs_t.rip's offset");
_Static_assert(offsetof(context_regs_t, rflags) == 16,
                "context_switch.s hardcodes context_regs_t.rflags's offset");
_Static_assert(offsetof(context_regs_t, rbx) == 24,
                "context_switch.s hardcodes context_regs_t.rbx's offset");
_Static_assert(offsetof(context_regs_t, rbp) == 32,
                "context_switch.s hardcodes context_regs_t.rbp's offset");
_Static_assert(offsetof(context_regs_t, r12) == 40,
                "context_switch.s hardcodes context_regs_t.r12's offset");
_Static_assert(offsetof(context_regs_t, r13) == 48,
                "context_switch.s hardcodes context_regs_t.r13's offset");
_Static_assert(offsetof(context_regs_t, r14) == 56,
                "context_switch.s hardcodes context_regs_t.r14's offset");
_Static_assert(offsetof(context_regs_t, r15) == 64,
                "context_switch.s hardcodes context_regs_t.r15's offset");
_Static_assert(offsetof(context_regs_t, rax) == 72,
                "context_switch.s hardcodes context_regs_t.rax's offset");
_Static_assert(sizeof(context_regs_t) == 80,
                "context_switch.s hardcodes sizeof(context_regs_t)");

typedef struct {
    page_owner_t     id;
    context_regs_t   regs;
    context_state_t  state;
} context_t;

/* Table capacity is VMM_MAX_ADDRESS_SPACES minus one: every context here is
 * required to have a bound address space (see context_create()), so there
 * is never a reason to track more contexts than vmm can back with a PML4
 * binding -- but src/kernel.c permanently occupies one of vmm's
 * VMM_MAX_ADDRESS_SPACES slots at boot (PAGE_OWNER_LIBOS -> vmm_kernel_pml4(),
 * ahead of the TESTING branch, never unbound in v1), entirely outside this
 * table. Sizing CONTEXT_MAX to the full VMM_MAX_ADDRESS_SPACES would claim
 * capacity this table can never actually reach: context_create()'s id
 * allocator correctly refuses to reuse an id already bound in vmm's
 * registry (see its own comment), so the (VMM_MAX_ADDRESS_SPACES)th
 * concurrent context_create() would always hit vmm's real ENOMEM one slot
 * earlier than this table's own capacity would suggest. */
#define CONTEXT_MAX (VMM_MAX_ADDRESS_SPACES - 1)

/*
 * Create a new context bound to `pml4_phys` (typically the output of
 * vmm_create_address_space()). Allocates a fresh page_owner_t id -- starting
 * at PAGE_OWNER_LIBOS and incrementing, skipping any id already live in
 * this table *or* already bound in vmm.c's own address-space registry
 * (src/kernel.c binds PAGE_OWNER_LIBOS there directly on every boot, ahead
 * of the TESTING branch, entirely outside this table -- an id search that
 * only checked this table would silently hand that id back out and
 * corrupt it) -- binds it to `pml4_phys` via vmm_bind_address_space() (the
 * same call site vmm.h's own comment names as this ticket's job), and sets
 * state to CONTEXT_STATE_READY with a zeroed register area.
 *
 * Returns CONTEXT_OK and writes the new id to `id_out`, or:
 *   CONTEXT_EINVAL  if `pml4_phys` is 0 (vmm_address_space_for()'s and
 *                   context_pml4()'s "no binding" sentinel -- a context
 *                   bound to it would be indistinguishable from an unknown
 *                   id), or the id space is exhausted past
 *                   PAGE_OWNER_ID_MASK
 *   CONTEXT_ENOMEM  if this table is full (CONTEXT_MAX live contexts
 *                   already), or vmm_bind_address_space() itself reports
 *                   VMM_ENOMEM (its own address_spaces[] registry is full --
 *                   a transient exhaustion, not a bad argument, kept
 *                   distinct from CONTEXT_EINVAL so a caller knows retrying
 *                   later could succeed)
 */
int context_create(uint64_t pml4_phys, page_owner_t *id_out);

/*
 * Tear down `id`: vmm_destroy_address_space(id) -- which frees the private
 * page-table subtree the bound PML4 built and removes the binding, the same
 * call libos_destroy_image() uses for a full teardown -- then unconditionally
 * clears the slot back to CONTEXT_STATE_UNUSED so its id can be reused by a
 * later context_create() regardless of whether the vmm-side teardown found
 * anything to do. Symmetric with context_create() binding a caller-built
 * address space to a fresh id: since this module is the one that bound it,
 * this module is the one that fully undoes that, rather than leaving the
 * caller to separately free the PML4 pages it can no longer look up once
 * unbound.
 *
 * Returns CONTEXT_OK, or CONTEXT_ENOENT if `id` names no live context in
 * this table, or if it does but vmm_destroy_address_space() found nothing
 * bound to it -- a live context is always supposed to have a vmm-side
 * binding (context_create() guarantees one before marking the slot READY),
 * so this second case means the two tables have desynced and is reported
 * rather than silently treated as a successful teardown.
 */
int context_destroy(page_owner_t id);

/* The live context_t for `id`, or NULL if `id` names no live context.
 * Returned pointer is only valid until the next context_create()/
 * context_destroy() call -- the table has no stable storage guarantee
 * beyond that, the same way vmm's own registry doesn't. */
context_t *context_lookup(page_owner_t id);

/* Number of contexts currently in a non-UNUSED state. */
uint32_t context_count(void);

/* Move `id` to `state`. Returns CONTEXT_OK, or CONTEXT_ENOENT if `id` names
 * no live context. */
int context_set_state(page_owner_t id, context_state_t state);

/* Physical PML4 bound to `id` (a thin wrapper over vmm_address_space_for(),
 * kept here so a caller working purely in context_t terms need not reach
 * into vmm.h), or 0 if `id` names no live context. */
uint64_t context_pml4(page_owner_t id);

/*
 * Seed a never-run context's saved register state with an initial iretq
 * frame -- entry_vaddr/stack_top_vaddr/LIBOS_LAUNCH_RFLAGS, matching
 * exactly what libos_enter() would push for a fresh launch (see
 * src/libos_launch.h). Must be called on a context_create()-fresh
 * (CONTEXT_STATE_READY, zeroed regs) context before it is ever named as a
 * context_switch_request() target -- context_switch_tail
 * (src/context_switch.s) always resumes via this same iretq-frame shape,
 * whether primed here or captured by a previous switch, and cannot tell the
 * two apart (nor does it need to).
 *
 * Returns CONTEXT_OK, or CONTEXT_ENOENT if `id` names no live context.
 */
int context_prime(page_owner_t id, uint64_t entry_vaddr, uint64_t stack_top_vaddr);

/*
 * Same as context_prime(), but seeds RFLAGS.IF set (LIBOS_LAUNCH_RFLAGS_IRQ,
 * src/libos_launch.h) instead -- for a context whose ring-3 code needs
 * exo_get_ticks()/exo_kbd_poll() to see real IRQ-driven state from the
 * moment its very first context_switch_tail resume lands, the same reason
 * libos_enter_irq() exists alongside plain libos_enter() (SCRUM-178: the WAD
 * viewer, launched via context_switch_request() from the shell's `wadview`
 * command rather than a direct libos_enter_irq() call from kernel_main).
 *
 * Returns CONTEXT_OK, or CONTEXT_ENOENT if `id` names no live context.
 */
int context_prime_irq(page_owner_t id, uint64_t entry_vaddr, uint64_t stack_top_vaddr);

/*
 * The currently RUNNING context's id -- PAGE_OWNER_LIBOS until the first
 * context_switch_request() ever succeeds, matching the single-LibOS default
 * src/syscall.c's syscall_current_context() already returned before this
 * ticket. Not necessarily a live row in this table: the boot-time default
 * LibOS is bound directly in vmm.c's registry (see this header's top
 * comment), not through context_create().
 */
page_owner_t context_current(void);

/*
 * Declare `id` the currently RUNNING context, without touching any
 * context_t's state field or arming a switch. For a caller that is about to
 * libos_enter() `id` directly rather than reach it via
 * context_switch_request() -- the seam a first dispatch (this ticket's own
 * test, and eventually kernel_main's boot tail once SCRUM-147 wires a real
 * scheduler in) uses before the very first switch of a context's life, when
 * there is no "previous" context for context_switch_request() to switch
 * away from. Does not validate `id` against this table: the boot-time
 * default (PAGE_OWNER_LIBOS, never context_create()'d) is a legitimate
 * value too -- see context_current()'s own comment.
 */
void context_set_current(page_owner_t id);

/*
 * Request a switch away from the current context to `to_id`. Validates
 * `to_id` names a live CONTEXT_STATE_READY context and that the current
 * context (context_current()) names a live row in this table -- the
 * boot-time default LibOS does not, since it was never context_create()'d,
 * so switching away from it is refused rather than silently corrupting a
 * table slot that does not exist.
 *
 * On success: flips the current context to CONTEXT_STATE_READY and `to_id`
 * to CONTEXT_STATE_RUNNING, updates context_current(), and arms
 * context_switch_pending so src/syscall_entry.s takes the
 * context_switch_tail exit instead of its normal pop+sysretq epilogue the
 * next time exo_syscall_dispatch() returns to it -- the actual register
 * capture/CR3 swap/restore happens there (src/context_switch.s), not here;
 * this call only decides and records that it will.
 *
 * Returns CONTEXT_OK, or CONTEXT_ENOENT if `to_id` is not a live READY
 * context or the current context has no row in this table.
 */
int context_switch_request(page_owner_t to_id);

/*
 * Round-robin scheduling policy for exo_yield (SCRUM-109, src/syscall_yield.c)
 * -- context_switch_request() above deliberately has no opinion on *which*
 * context to switch to; this is that opinion, kept to the minimum the
 * yield acceptance criterion needs rather than a real scheduler (priority,
 * fairness across BLOCKED contexts, etc. is SCRUM-147's job).
 *
 * Scans this table starting just after `current`'s own slot (found the same
 * way context_switch_request() finds it) and wrapping once around, returning
 * the first CONTEXT_STATE_READY id found. `current`'s own slot is never
 * matched: a context making this call is CONTEXT_STATE_RUNNING, not READY,
 * so no self-exclusion check is needed. Starting just past `current` rather
 * than always at slot 0 is what makes repeated yields cycle through every
 * READY context instead of always landing on the same one.
 *
 * Returns the next READY id, or PAGE_OWNER_FREE (0) if `current` names no
 * row in this table (e.g. the boot-time default LibOS) or no other context
 * is READY -- either way, "nothing to switch to" for the caller.
 */
page_owner_t context_next_ready(page_owner_t current);

/* ── src/context_switch.s / src/syscall_entry.s interface ─────────────────
 *
 * Raw externs, not part of this header's C API -- context_switch_request()
 * is. Nonzero context_switch_pending, set by context_switch_request(), tells
 * syscall_entry.s to jump to context_switch_tail (src/context_switch.s)
 * instead of its normal epilogue; that routine reads
 * context_switch_out_regs/_in_regs/_in_pml4 (also set by
 * context_switch_request()) to do the actual save/CR3-swap/restore. See
 * src/context_switch.s's own comment for the register-capture invariants
 * this depends on. */
extern uint64_t context_switch_pending;
extern context_regs_t *context_switch_out_regs;
extern context_regs_t *context_switch_in_regs;
extern uint64_t context_switch_in_pml4;

#define CONTEXT_OK      0
#define CONTEXT_ENOMEM  (-1)
#define CONTEXT_ENOENT  (-2)
#define CONTEXT_EINVAL  (-3)

#endif
