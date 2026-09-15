#ifndef CONTEXT_H
#define CONTEXT_H

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
 * What this ticket does NOT do: an actual context switch. context_regs_t is
 * a fixed save area, not wired to anything yet -- libos_enter.s and
 * syscall_entry.s still each keep their own single global saved-RSP slot,
 * which both that file's and docs/syscall_spec.md's STAR/LSTAR/FMASK
 * section already flag as the thing that has to become a per-CPU
 * (`swapgs`-based) slot once two contexts are ever live *concurrently*
 * through the syscall/launch path -- that rework, and actually performing a
 * switch, is SCRUM-108's job. This ticket's acceptance is narrower: the
 * kernel can track 2+ LibOS contexts' id/page-dir/registers/state at once,
 * proven by tests/kernel/test_context_k.c. Nothing here is wired into
 * kernel_main's normal boot tail either, for the same reason libos_launch.h
 * gives for SCRUM-47/-49/-50 landing unwired: there is still only one real
 * LibOS to run on a normal boot.
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
 * three an iretq/syscall frame itself carries (RSP, RIP, RFLAGS). A future
 * SCRUM-108 switch fills this in on the way out of a context and restores
 * it on the way back in -- nothing here does either yet.
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
} context_regs_t;

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

#define CONTEXT_OK      0
#define CONTEXT_ENOMEM  (-1)
#define CONTEXT_ENOENT  (-2)
#define CONTEXT_EINVAL  (-3)

#endif
