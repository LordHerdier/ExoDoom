#ifndef LIBOS_LAUNCH_H
#define LIBOS_LAUNCH_H

#include "exo_syscall.h"  /* EXO_USER_VA_BASE, EXO_SYS_EXIT -- __ASSEMBLER__-safe */

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

#include "page_alloc.h"   /* page_owner_t */
#endif

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
 *
 * SCRUM-50: this header is also included by bare assembly test probes (the
 * .s files under tests/kernel), preprocessed with the C preprocessor before
 * assembly
 * (docker/scripts/build.sh, `-x assembler-with-cpp`, which predefines
 * `__ASSEMBLER__`) so a probe can reference LIBOS_LAUNCH_DATA_VADDR or
 * LIBOS_RETURN_SYSCALL_NUM as the same macro the C side uses instead of a
 * hand-copied literal. Everything an assembler cannot parse -- the typedef,
 * the function prototypes, the layout `_Static_assert` -- is wrapped in
 * `#ifndef __ASSEMBLER__`; keep new C-only content wrapped the same way.
 */

/* Fixed layout inside the LibOS window: code, then data+bss, then the stack,
 * each given enough headroom (LIBOS_LAUNCH_MAX_*_PAGES) that one section's
 * worst case cannot overlap the next section's base.
 *
 * Sizing history, because each step was driven by a real target rather than
 * by round numbers: 4/4 pages originally; 8/16 under SCRUM-51, when the
 * ring-3 libc-shim probe linked in libos_page_alloc.c's slot table
 * (LIBOS_PAGE_ALLOC_MAX_PAGES = 4096 slots) as 48 KiB of static data, more
 * than the whole previous data budget; and 192/192 plus a 16-page stack
 * under SCRUM-66, for Doom -- which needs ~97 pages of code+rodata, ~99 of
 * data+bss, and rather more than one page of stack to recurse through a
 * level's BSP tree. That last step is roughly twice what Doom actually
 * uses, deliberately.
 *
 * The layout no longer has a hand-maintained ceiling. It used to promise it
 * stayed below EXO_USER_VA_BASE + 0x20000 so that the fault probes could
 * hardcode that address as "unmapped"; those probes now derive their target
 * from LIBOS_LAUNCH_UNMAPPED_VADDR (defined below, with the reasoning), so
 * raising a cap moves them with it instead of silently invalidating them.
 *
 * LIBOS_LAUNCH_GUARD_PAGES leaves a deliberately unmapped page between the
 * data+bss region's worst case and the stack: libos_build_image() never maps
 * anything past LIBOS_LAUNCH_DATA_VADDR + (actual data_len+bss_len), which is
 * capped at LIBOS_LAUNCH_MAX_DATA_PAGES*0x1000, so this page is guaranteed
 * never to be mapped by that call no matter how much of the data budget a
 * given LibOS actually uses. Without it, a LibOS that fills its data budget
 * to the max would have its one-page stack sitting immediately below a
 * mapped, writable page -- a stack overflow would then silently corrupt data
 * instead of taking the page fault the guard page exists to produce. */
#define LIBOS_LAUNCH_MAX_CODE_PAGES 192
#define LIBOS_LAUNCH_MAX_DATA_PAGES 192
#define LIBOS_LAUNCH_GUARD_PAGES    1

/* Stack pages, raised from the single page every earlier target ran on
 * (SCRUM-66). 4 KiB was enough for a probe and for the hand-written demo
 * LibOSes and is nowhere near enough for Doom: R_RenderPlayerView descends
 * through R_RenderBSPNode recursively over the level's BSP tree, and the
 * setup path (P_SetupLevel -> P_LoadThings -> P_SpawnMapThing) is deep in
 * its own right. A blown ring-3 stack grows down into
 * LIBOS_LAUNCH_GUARD_PAGES' unmapped page and takes a page fault rather
 * than silently corrupting .bss, so the failure is at least legible -- but
 * it is still a crash, and 64 KiB buys enough depth not to have it.
 *
 * The guard page sits between data+bss and the stack, i.e. below the
 * stack's lowest address, which is the direction a stack grows. */
#define LIBOS_LAUNCH_STACK_PAGES    16

/* Selectors and RFLAGS values shared between src/libos_enter.s and
 * src/context_switch.s (SCRUM-108): both build an iretq frame into the same
 * ring-3 code/data segments, and a context resumed after a switch must use
 * the exact same RFLAGS a fresh libos_enter() launch would have used
 * (context_prime() seeds it) -- one shared definition rather than two
 * private `.set`s that could drift apart. See src/boot.s's GDT for where
 * 0x20/0x28 come from and docs/syscall_spec.md §3.4 for why the layout is
 * fixed.
 *
 * LIBOS_LAUNCH_RFLAGS: IF clear, bit 1 (reserved, must be set) -- every
 * existing fault/launch test depends on this exact value for libos_enter().
 * LIBOS_LAUNCH_RFLAGS_IRQ: same, with IF set, for libos_enter_irq(). */
#define LIBOS_LAUNCH_USER_SS      (0x20 | 3)
#define LIBOS_LAUNCH_USER_CS      (0x28 | 3)
#define LIBOS_LAUNCH_RFLAGS       0x002
#define LIBOS_LAUNCH_RFLAGS_IRQ   0x202

#define LIBOS_LAUNCH_CODE_VADDR  (EXO_USER_VA_BASE + 0x1000ULL)
#define LIBOS_LAUNCH_DATA_VADDR  (LIBOS_LAUNCH_CODE_VADDR + \
                                  LIBOS_LAUNCH_MAX_CODE_PAGES * 0x1000ULL)
#define LIBOS_LAUNCH_STACK_VADDR (LIBOS_LAUNCH_DATA_VADDR + \
                                  (LIBOS_LAUNCH_MAX_DATA_PAGES + \
                                   LIBOS_LAUNCH_GUARD_PAGES) * 0x1000ULL)

/* One past the highest address libos_build_image() can map, and the first
 * page after it, which is therefore guaranteed unmapped.
 *
 * These exist because the layout stopped being small (SCRUM-66). The probes
 * that deliberately fault on an unmapped user address used to hardcode
 * EXO_USER_VA_BASE + 0x20000 and rely on a prose promise in this file that
 * nothing would ever grow past it -- exactly the coupling that breaks
 * silently when someone raises a cap, because a probe whose "unmapped"
 * target has quietly become mapped stops testing anything and starts
 * reading the stack. Deriving both from the same constants the loader uses
 * means a cap change moves the probes with it.
 *
 * Headroom above this, for the record: the LibOS heap
 * (LIBOS_HEAP_VADDR_BASE, src/libos_page_alloc.c) starts at
 * EXO_USER_VA_BASE + 0x1000000 and the layout as sized above ends at
 * EXO_USER_VA_BASE + 0x192000 -- a factor of ten clear. It is not asserted
 * here only because that base is defined in a .c file this header cannot
 * include; raise the caps far enough and it is the next thing to check. */
#define LIBOS_LAUNCH_LAYOUT_END  (LIBOS_LAUNCH_STACK_VADDR + LIBOS_LAUNCH_STACK_PAGES * 0x1000ULL)
#define LIBOS_LAUNCH_UNMAPPED_VADDR (LIBOS_LAUNCH_LAYOUT_END + 0x1000ULL)

/*
 * The RSP a freshly launched LibOS starts with -- deliberately 8 below the
 * top of its stack, not at it (SCRUM-66).
 *
 * The x86-64 SysV ABI requires RSP to be 16-byte aligned *at the point of a
 * call*. `call` then pushes an 8-byte return address, so every compiled
 * function is entitled to assume RSP == 8 (mod 16) on entry, which is what
 * makes its `push %rbp; mov %rsp,%rbp` prologue leave %rbp 16-byte aligned,
 * which in turn is what makes an aligned SSE access to a local slot such as
 * `movdqa -0x40(%rbp),%xmm2` legal.
 *
 * A LibOS is not called: libos_enter()/context_prime_irq() arrive by iretq,
 * which sets RSP to exactly this value and pushes no return address. Enter
 * with a page-aligned RSP -- 0 (mod 16) -- and every frame in the process is
 * skewed by 8 from what the compiler assumed, so the first aligned SSE
 * access to a stack local raises #GP.
 *
 * That is not hypothetical: it is precisely how this constant was found.
 * Doom is the first ring-3 target compiled with SSE enabled (it has to be --
 * see docker/scripts/build-doom.sh on the float ABI), and it faulted on the
 * `movdqa -0x40(%rbp),%xmm2` in doom_wad_mount() the very first time it ran.
 * Every earlier target is built -mno-sse and never emits an alignment-
 * sensitive instruction, which is why a launch ABI that had been subtly
 * wrong all along had never once been caught.
 */
#define LIBOS_LAUNCH_STACK_TOP (LIBOS_LAUNCH_LAYOUT_END - 8ULL)

#ifndef __ASSEMBLER__
/* The layout, plus the guaranteed-unmapped page the probes fault on, has to
 * stay inside the LibOS window. This replaces the old assertion that it fit
 * below EXO_USER_VA_BASE + 0x20000: that ceiling existed only to protect
 * libos_launch_probe.s's hardcoded fault target, which now derives from
 * LIBOS_LAUNCH_UNMAPPED_VADDR and moves on its own (SCRUM-66).
 *
 * What still has to hold is the window itself: everything here lives between
 * EXO_USER_VA_BASE and EXO_USER_VA_END because exo_page_map() refuses any
 * vaddr outside it (docs/syscall_spec.md sec3.7), so an image that spilled
 * past the end could not map its own heap alongside its own bss. */
_Static_assert(LIBOS_LAUNCH_UNMAPPED_VADDR + 0x1000ULL <= EXO_USER_VA_END,
              "libos_launch layout no longer fits inside the LibOS VA "
              "window [EXO_USER_VA_BASE, EXO_USER_VA_END)");

/*
 * entry_vaddr and stack_top_vaddr are runtime fields, not compile-time
 * constants, so nothing here can `_Static_assert` them equal to
 * LIBOS_LAUNCH_CODE_VADDR / (LIBOS_LAUNCH_STACK_VADDR + VMM_PAGE_SIZE) the
 * way the layout invariant above can. libos_build_image() is the only place
 * that assigns them, and it always assigns exactly those two expressions —
 * that single assignment site is what keeps the promise, not anything a
 * caller can check.
 */
typedef struct {
    uint64_t pml4_phys;        /* the new address space's PML4 (for teardown) */
    uint64_t entry_vaddr;      /* always LIBOS_LAUNCH_CODE_VADDR, for libos_enter() */
    uint64_t stack_top_vaddr;  /* always LIBOS_LAUNCH_STACK_TOP, ditto        */
    uint64_t code_paddrs[LIBOS_LAUNCH_MAX_CODE_PAGES]; /* backing pages, in order */
    uint32_t code_pages;       /* how many of the above are actually mapped   */
    uint64_t data_paddrs[LIBOS_LAUNCH_MAX_DATA_PAGES]; /* ditto, for data+bss  */
    uint32_t data_pages;
    uint64_t stack_paddrs[LIBOS_LAUNCH_STACK_PAGES]; /* backing pages, in order */
    uint32_t stack_pages;      /* how many of the above are actually mapped   */
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
 * Overwrite the first `len` bytes of a successfully-built LibOS's .data
 * region in place (SCRUM-175) -- the generalized alternative to a per-app
 * side-channel params page (an earlier, WAD-viewer-specific version of this
 * mechanism hand-mapped a second page at its own fixed VA; see
 * src/syscall_launch.c's history and src/libos_wad_params.h). The convention
 * this depends on: a LibOS app declares its own params struct as literally
 * the first global in its own `.data` (non-zero-initialized, so it survives
 * -- GCC never stores real bytes for a zero initializer, so a `= 0` global
 * lands in `.bss` instead), and its translation unit is linked first for the
 * same reason `.text.entry` placement already requires it (see
 * tests/kernel/ring3_link_target.ld.in) -- a TU's globals land in `.data` in
 * source order, and `ld`'s `*(.data)` rule then collects input sections in
 * link order, so "first global in the first-linked TU" is what lands at
 * offset 0 of the whole `.data` blob.
 *
 * img->data_paddrs[0] is the physical page backing that offset --
 * identity-mapped, so the kernel writes it directly with no
 * vmm_map_page_in() of its own, unlike the old side-channel page. Call this
 * after libos_build_image() returns VMM_OK and before switching to or
 * launching the image.
 *
 * Returns VMM_EINVAL if img->data_pages == 0 (nothing was mapped at
 * LIBOS_LAUNCH_DATA_VADDR to patch) or len > VMM_PAGE_SIZE (a params struct
 * spanning a page boundary would need a real multi-page copy this does not
 * attempt -- no caller needs one yet, since every params struct so far is a
 * handful of fields).
 *
 * The patched struct is NOT read-only, unlike the old per-app side-channel
 * page it replaces (that one was deliberately mapped VMM_PRESENT|VMM_USER
 * with no VMM_WRITE, so a stray write from the launched app would fault
 * immediately). It cannot be here: it lives in the same VMM_WRITE region as
 * the rest of the app's `.data`, and the whole point of this convention is
 * one general-purpose writable region rather than a bespoke read-only page
 * per app. This is an accepted widening, not an oversight -- a LibOS is
 * already fully trusted with everything else in its own address space (its
 * heap, its stack, every other `.data`/`.bss` global), so a bug in the
 * launched app corrupting its own params struct post-launch is no different
 * in kind from it corrupting any other piece of its own state, and the
 * kernel was never going to catch that either way. Do not add a caller that
 * relies on this struct staying byte-for-byte what was patched here for any
 * security-relevant decision -- treat it as the app's own mutable state from
 * the moment this call returns.
 */
int libos_launch_patch_params(const libos_image_t *img,
                              const void *params, size_t len);

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
 * 0x002 (IF clear) -- a hardware interrupt arriving while CPL-3 code runs
 * does switch to TSS.RSP0 the same way the SCRUM-46 page-fault test proves
 * an exception does (SCRUM-170 proved it live for IRQ0 too, via
 * libos_enter_irq() below), but this entry point still launches with
 * interrupts globally masked -- every existing fault/launch test depends on
 * that exact behavior, so it stays that way; a launched context that needs
 * IRQs to actually land uses libos_enter_irq() instead.
 *
 * libos_return() unwinds with IF still clear regardless of what it was in
 * the caller -- fine today because every caller runs before kernel_main's
 * `sti`, but a caller running after it must re-enable interrupts itself on
 * the way back; this function does not save/restore RFLAGS across the trip.
 *
 * `libos_saved_rsp` (src/libos_enter.s) is a single global, so only one
 * libos_enter()/libos_return() round trip can be in flight at a time -- the
 * same non-reentrancy syscall_entry.s's saved-user-RSP slot calls out, and
 * the same fix applies: `swapgs` plus a per-CPU block once something needs
 * more than one context live at once. SCRUM-107 (src/context.c/h) added the
 * table that *tracks* multiple contexts' saved register state; it does not
 * touch this global or perform a real switch -- that, and this reentrancy
 * fix, are SCRUM-108's job.
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

/*
 * Same as libos_enter() (src/libos_enter.s), except RFLAGS.IF is set on the
 * way in instead of clear, so PIT/keyboard IRQs keep landing -- on
 * TSS.RSP0, then back to CPL 3 via their own iretq, per idt_set_gate()'s
 * IST=0 gates -- for as long as the launched context runs. libos_enter()
 * itself is left with IF clear for every existing fault/launch test; this
 * is for a launched context that needs exo_get_ticks() to actually advance
 * or exo_kbd_poll() to actually see input while it runs, which is not
 * possible with interrupts globally masked the whole time.
 *
 * tests/kernel/test_irq_entry_k.c (SCRUM-170) drives this entry point and
 * proves it live: a probe launched through it busy-waits on real
 * exo_get_ticks() advancement, and src/pit.c's irq0_handler records
 * (TESTING builds only) the stack pointer it observed on entry, which the
 * test checks against tss_rsp0()'s range -- the same frame-location check
 * test_tss_k.c makes for a CPL-3 page fault, done here for a hardware
 * interrupt instead.
 */
uint64_t libos_enter_irq(uint64_t entry_vaddr, uint64_t stack_top_vaddr);

/* The far side of libos_enter() -- see above. Signature matches
 * exo_handler_t (src/syscall.h) so it can be registered directly:
 * exo_syscall_register(SOME_NUM, libos_return). */
int64_t libos_return(uint64_t result, uint64_t a2, uint64_t a3, uint64_t a4,
                     uint64_t a5, uint64_t a6);
#endif /* __ASSEMBLER__ */

/*
 * The syscall number every test harness in this tree borrows to register
 * libos_return() -- there being no dedicated ABI number for "leave the
 * launched context" yet, since nothing calls libos_return() in production
 * (see above). EXO_SYS_EXIT is the natural borrow: exo_exit() is the closest
 * real ABI meaning ("this context is done"), and it has no handler of its
 * own bound today (SCRUM-155). One named constant here rather than a
 * `#define SYS_LIBOS_RETURN 20` repeated in every test file that needs it
 * (and every launch probe's `.set SYS_LIBOS_RETURN, 20`) is what keeps them
 * from drifting apart if the borrow ever moves to a different number.
 */
#define LIBOS_RETURN_SYSCALL_NUM EXO_SYS_EXIT

#endif
