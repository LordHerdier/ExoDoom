#ifndef LIBOS_LAUNCH_H
#define LIBOS_LAUNCH_H

#include <stddef.h>
#include <stdint.h>

#include "page_alloc.h"   /* page_owner_t */
#include "exo_syscall.h"  /* EXO_USER_VA_BASE */

/*
 * libos_launch — the real ring-3 launch mechanism (SCRUM-47).
 *
 * tests/kernel/ring3_probe.s's ring3_run() proved the `syscall`/`iretq` round
 * trip and register preservation, but it cheats twice in ways only a test
 * build can afford: it runs its probe out of ordinary kernel .text and a
 * kernel .bss stack, reachable from CPL 3 only because TESTING builds mark
 * the *entire* identity map user-accessible (RING3_PROBE=1 in boot.s), and it
 * never switches CR3 -- the probe runs on the kernel's own address space the
 * whole time. Neither holds on a normal build, where kernel pages are
 * supervisor-only and every LibOS needs its own address space (SCRUM-48) so
 * two of them cannot unmap each other's pages.
 *
 * A real launch therefore needs code and a stack that live inside the LibOS
 * mapping window ([EXO_USER_VA_BASE, EXO_USER_VA_END), src/exo_syscall.h) of
 * a *fresh* address space, mapped VMM_USER, backed by pages the launched
 * context actually owns -- libos_build_image() is that setup. libos_enter()
 * (src/libos_enter.s) is the mechanical half: switch CR3, then iretq to CPL 3
 * at the mapped entry point. It does not return in the normal sense -- see
 * its own comment -- so getting back out (this ticket's tests do, production
 * code need not) is libos_return(), registered as a syscall handler the same
 * way tests/kernel/ring3_probe.s's ring3_escape is.
 *
 * What this ticket does NOT do: load a real ELF (SCRUM-49) or define a
 * libos_main() calling convention (SCRUM-50). libos_build_image() takes a
 * flat, position-independent code blob because that is all that exists
 * today; SCRUM-49 supplies a loaded binary's bytes as that same blob later.
 * It also does not wire this into kernel_main's normal boot tail -- that
 * tail is a live interactive demo (timer + keyboard loop), not a
 * placeholder, and there is nothing real yet for it to jump to. This lands
 * as a proven primitive + its own test suite, the same way the syscall entry
 * path (SCRUM-32) and the TSS (SCRUM-46) did.
 *
 * SCRUM-49 extends the above with a real code *and* data placement: a LibOS
 * is no longer assumed to fit in one PIC page with no writable state. Code
 * and data now live at their own defined, fixed virtual addresses inside the
 * LibOS window, sized independently and each spanning up to
 * LIBOS_LAUNCH_MAX_{CODE,DATA}_PAGES -- still a fixed cap, not a general
 * loader, because nothing above this layer parses a binary format yet
 * (SCRUM-50 is the entry framework that will want one). The data region
 * covers `.data` (copied) immediately followed by `.bss` (zeroed) in one
 * contiguous mapped range, since both are just "writable state at a known
 * address" as far as this loader cares -- splitting them further has no
 * customer until something needs different permissions for the two.
 */

/* Fixed layout inside the LibOS window: code, then data+bss, then the stack,
 * each given enough headroom (LIBOS_LAUNCH_MAX_*_PAGES) that one section's
 * worst case cannot overlap the next section's base. Everything here stays
 * well below EXO_USER_VA_BASE + 0x20000, which tests/kernel/libos_launch_probe.s
 * relies on being unmapped (its deliberate page-fault target) -- move the
 * stack without checking that file's comment first. */
#define LIBOS_LAUNCH_MAX_CODE_PAGES 4
#define LIBOS_LAUNCH_MAX_DATA_PAGES 4

#define LIBOS_LAUNCH_CODE_VADDR  (EXO_USER_VA_BASE + 0x1000ULL)
#define LIBOS_LAUNCH_DATA_VADDR  (LIBOS_LAUNCH_CODE_VADDR + \
                                  LIBOS_LAUNCH_MAX_CODE_PAGES * 0x1000ULL)
#define LIBOS_LAUNCH_STACK_VADDR (LIBOS_LAUNCH_DATA_VADDR + \
                                  LIBOS_LAUNCH_MAX_DATA_PAGES * 0x1000ULL)

typedef struct {
    uint64_t pml4_phys;        /* the new address space's PML4 (for teardown) */
    uint64_t entry_vaddr;      /* LIBOS_LAUNCH_CODE_VADDR, for libos_enter()  */
    uint64_t stack_top_vaddr;  /* top of the mapped stack page, ditto         */
    uint64_t code_paddrs[LIBOS_LAUNCH_MAX_CODE_PAGES]; /* backing pages, in order */
    uint32_t code_pages;       /* how many of the above are actually mapped   */
    uint64_t data_paddrs[LIBOS_LAUNCH_MAX_DATA_PAGES]; /* ditto, for data+bss  */
    uint32_t data_pages;
    uint64_t stack_paddr;      /* backing page for the stack                  */
} libos_image_t;

/*
 * Build a fresh address space for `owner` and place three things in it, each
 * at its own fixed address in the LibOS window:
 *
 *   - `code_len` bytes of `code`, at LIBOS_LAUNCH_CODE_VADDR, across as many
 *     pages as needed (present, user, NOT writable -- it is instructions,
 *     copied once here and never written again).
 *   - `data_len` bytes of `data` followed by `bss_len` zeroed bytes, at
 *     LIBOS_LAUNCH_DATA_VADDR, across as many pages as `data_len + bss_len`
 *     needs (present, user, writable). `data` may be NULL / `data_len` and
 *     `bss_len` may both be 0 for a LibOS with no writable state to load.
 *   - one stack page at LIBOS_LAUNCH_STACK_VADDR (present, user, writable),
 *     zeroed, unconditionally.
 *
 * Binds the address space to `owner` in the vmm registry as a side effect,
 * so a caller only has to hang onto `owner` to tear it down later
 * (vmm_destroy_address_space).
 *
 * `code` and `data` must be position-independent with respect to each other
 * and to wherever the compiler happened to place them in the kernel image:
 * both are copied verbatim to their fixed destinations, so no internal
 * jump/call and no RIP-relative reference is allowed in `code` that assumes
 * it and `data` keep their relative offset from the kernel image -- a
 * reference to `data` must instead use the fixed immediate
 * LIBOS_LAUNCH_DATA_VADDR, the same way tests/kernel/libos_launch_probe.s
 * already does for other fixed addresses.
 *
 * `owner` must not already have a bound address space (this always creates a
 * new one) and must not be PAGE_OWNER_FREE/PAGE_OWNER_KERNEL, same
 * restriction vmm_bind_address_space enforces.
 *
 * Returns VMM_OK, or a VMM_E* status (see vmm.h) from whichever step failed.
 * On any failure after the address space was created and bound, every page
 * allocated so far and the address space itself are freed before returning,
 * so a failed call leaves nothing behind for the caller to clean up.
 * Rejected as VMM_EINVAL before anything is allocated: `code_len` of 0 (there
 * is nothing for `entry_vaddr` to point at); `code_len` above
 * `LIBOS_LAUNCH_MAX_CODE_PAGES * VMM_PAGE_SIZE`; `data_len` or `bss_len`
 * individually, or `data_len + bss_len`, above
 * `LIBOS_LAUNCH_MAX_DATA_PAGES * VMM_PAGE_SIZE`; or `data` NULL while
 * `data_len` is nonzero.
 *
 * On success, every backing page is still owned by `owner` and mapped into
 * the new address space -- pass `out` to libos_destroy_image() (with the
 * same `owner`) to tear the whole thing down, rather than calling
 * vmm_destroy_address_space() directly, which frees page tables only and
 * leaks these.
 */
int libos_build_image(page_owner_t owner,
                      const void *code, size_t code_len,
                      const void *data, size_t data_len, size_t bss_len,
                      libos_image_t *out);

/*
 * Undo a successful libos_build_image(): frees every code, data and the
 * stack page back to the PMM (free_page_owned(), since alloc_page_owned()
 * tagged them `owner`) and then vmm_destroy_address_space()s the PML4 they
 * were mapped into. `owner`/`img` must be the same pair libos_build_image()
 * returned VMM_OK for -- this is the only correct way to tear down a built
 * image.
 */
void libos_destroy_image(page_owner_t owner, const libos_image_t *img);

/*
 * `iretq` to CPL 3 at `entry_vaddr` with RSP = `stack_top_vaddr`. RFLAGS is
 * 0x002 (IF clear) -- a hardware interrupt arriving while CPL-3 code runs is
 * mechanistically expected to switch to TSS.RSP0 the same way the SCRUM-46
 * page-fault test proves an exception does, but that is unverified
 * (SCRUM-170) and deliberately not this ticket's risk to take on.
 *
 * libos_return() unwinds with IF still clear regardless of what it was in
 * the caller -- fine today because every caller runs before kernel_main's
 * `sti`, but a caller running after it must re-enable interrupts itself on
 * the way back; this function does not save/restore RFLAGS across the trip.
 *
 * `libos_saved_rsp` (src/libos_enter.s) is a single global, so only one
 * libos_enter()/libos_return() round trip can be in flight at a time -- the
 * same non-reentrancy syscall_entry.s's saved-user-RSP slot calls out, and
 * the same fix applies: `swapgs` plus a per-CPU block once SCRUM-107 needs
 * more than one context live at once.
 *
 * Does NOT switch CR3 -- `entry_vaddr`/`stack_top_vaddr` only resolve inside
 * the address space libos_build_image() mapped them into, so the caller
 * must vmm_switch_address_space(img->pml4_phys) first (and switch back to
 * vmm_kernel_pml4() once this returns). Kept as the caller's job rather than
 * folded in here because it is already a separate, already-tested primitive
 * (src/vmm.c) with its own failure mode (VMM_EINVAL) that a void-returning
 * step in the middle of this function could not report.
 *
 * Does not return in the normal sense: there is no `ret` on the other side
 * of an `iretq` to CPL 3. What it returns is whatever the launched code
 * eventually passes to libos_return() -- the same saved-RSP unwind
 * tests/kernel/ring3_probe.s's ring3_run/ring3_escape pair uses, generalized
 * out of the test tree because a scheduler yielding away from a context
 * (SCRUM-107) will want the identical mechanism. Nothing calls
 * libos_return() in production; it only does anything once a caller
 * registers it as a syscall handler, which is this ticket's tests' job.
 */
uint64_t libos_enter(uint64_t entry_vaddr, uint64_t stack_top_vaddr);

/* The far side of libos_enter() -- see above. Signature matches
 * exo_handler_t (src/syscall.h) so it can be registered directly:
 * exo_syscall_register(SOME_NUM, libos_return). */
int64_t libos_return(uint64_t result, uint64_t a2, uint64_t a3, uint64_t a4,
                     uint64_t a5, uint64_t a6);

#endif
