#pragma once

#include "page_alloc.h"   /* page_owner_t */
#include "libos_launch.h" /* libos_image_t */

#include <stdint.h>

/*
 * libos_test_common.h — shared helpers for the ring-3 launch/entry suites
 * (SCRUM-47/-49/-50): test_libos_launch_k.c and test_libos_main_k.c both
 * build a real address space, launch into it, and must tear it down the
 * same way even when an assertion fails mid-test and skips the suite's own
 * cleanup. Centralised here so the two suites (and whatever the next one
 * turns out to be) can't drift on how that works, or on which scratch
 * owner id is safe to use.
 *
 * Scratch owner ids for suites that call vmm_bind_address_space(): distinct
 * from each other, checked below rather than left as a comment for the next
 * one to trust. Suites that only alloc/free plain pages (test_ownership_k.c,
 * test_page_map_k.c, test_fb_binding_k.c, test_revoke_k.c) all reuse
 * PAGE_OWNER_LIBOS + 1 as their own local "other LibOS" id -- safe, since
 * none of them touch the address-space registry, so there is nothing here
 * for them to collide with.
 */
#define TEST_OWNER_VMM_REGISTRY  ((page_owner_t)(PAGE_OWNER_LIBOS + 3))
#define TEST_OWNER_VMM_ADDRSPACE ((page_owner_t)(PAGE_OWNER_LIBOS + 4))
#define TEST_OWNER_LIBOS_LAUNCH  ((page_owner_t)(PAGE_OWNER_LIBOS + 5))
#define TEST_OWNER_LIBOS_MAIN    ((page_owner_t)(PAGE_OWNER_LIBOS + 6))
#define TEST_OWNER_LIBOS_C_PROBE ((page_owner_t)(PAGE_OWNER_LIBOS + 7))
#define TEST_OWNER_PORT_IO_FAULT ((page_owner_t)(PAGE_OWNER_LIBOS + 8))
#define TEST_OWNER_KERNEL_MEM_FAULT ((page_owner_t)(PAGE_OWNER_LIBOS + 9))
#define TEST_OWNER_IRQ_ENTRY     ((page_owner_t)(PAGE_OWNER_LIBOS + 10))
#define TEST_OWNER_SSE           ((page_owner_t)(PAGE_OWNER_LIBOS + 11))

/*
 * test_libc_shim_probe_k.c (SCRUM-51) deliberately does NOT get its own
 * TEST_OWNER_* sentinel here, unlike every suite above. Those suites only
 * ever call exo_serial_write (#8) from their launched ring-3 code, which
 * src/syscall_serial.c never routes through an address space -- any owner
 * id works. The libc shim probe is the first to call exo_page_alloc/
 * exo_page_map (#0/#2) as *real* ring-3 syscalls, and those handlers
 * resolve "whose address space is this" via syscall_current_context()
 * (src/syscall.c), which for v1's single-LibOS kernel is hardcoded to
 * return the one id PAGE_OWNER_LIBOS -- not whatever owner a test happened
 * to launch under. Launching under a distinct sentinel (as every suite
 * above does) would make the syscall handler resolve a *different*
 * address space than the one actually loaded in CR3 -- silently mapping
 * into the kernel's own page tables instead -- so that suite binds
 * PAGE_OWNER_LIBOS itself. Safe because no suite here calls
 * vmm_bind_address_space() on bare PAGE_OWNER_LIBOS; the ownership suites
 * that do touch it (test_ownership_k.c and friends) only tag plain pages,
 * never bind an address space to it.
 */

_Static_assert(TEST_OWNER_VMM_REGISTRY     != TEST_OWNER_VMM_ADDRSPACE &&
              TEST_OWNER_VMM_REGISTRY     != TEST_OWNER_LIBOS_LAUNCH &&
              TEST_OWNER_VMM_REGISTRY     != TEST_OWNER_LIBOS_MAIN &&
              TEST_OWNER_VMM_REGISTRY     != TEST_OWNER_LIBOS_C_PROBE &&
              TEST_OWNER_VMM_REGISTRY     != TEST_OWNER_PORT_IO_FAULT &&
              TEST_OWNER_VMM_REGISTRY     != TEST_OWNER_KERNEL_MEM_FAULT &&
              TEST_OWNER_VMM_REGISTRY     != TEST_OWNER_IRQ_ENTRY &&
              TEST_OWNER_VMM_REGISTRY     != TEST_OWNER_SSE &&
              TEST_OWNER_VMM_ADDRSPACE    != TEST_OWNER_LIBOS_LAUNCH &&
              TEST_OWNER_VMM_ADDRSPACE    != TEST_OWNER_LIBOS_MAIN &&
              TEST_OWNER_VMM_ADDRSPACE    != TEST_OWNER_LIBOS_C_PROBE &&
              TEST_OWNER_VMM_ADDRSPACE    != TEST_OWNER_PORT_IO_FAULT &&
              TEST_OWNER_VMM_ADDRSPACE    != TEST_OWNER_KERNEL_MEM_FAULT &&
              TEST_OWNER_VMM_ADDRSPACE    != TEST_OWNER_IRQ_ENTRY &&
              TEST_OWNER_VMM_ADDRSPACE    != TEST_OWNER_SSE &&
              TEST_OWNER_LIBOS_LAUNCH     != TEST_OWNER_LIBOS_MAIN &&
              TEST_OWNER_LIBOS_LAUNCH     != TEST_OWNER_LIBOS_C_PROBE &&
              TEST_OWNER_LIBOS_LAUNCH     != TEST_OWNER_PORT_IO_FAULT &&
              TEST_OWNER_LIBOS_LAUNCH     != TEST_OWNER_KERNEL_MEM_FAULT &&
              TEST_OWNER_LIBOS_LAUNCH     != TEST_OWNER_IRQ_ENTRY &&
              TEST_OWNER_LIBOS_LAUNCH     != TEST_OWNER_SSE &&
              TEST_OWNER_LIBOS_MAIN       != TEST_OWNER_LIBOS_C_PROBE &&
              TEST_OWNER_LIBOS_MAIN       != TEST_OWNER_PORT_IO_FAULT &&
              TEST_OWNER_LIBOS_MAIN       != TEST_OWNER_KERNEL_MEM_FAULT &&
              TEST_OWNER_LIBOS_MAIN       != TEST_OWNER_IRQ_ENTRY &&
              TEST_OWNER_LIBOS_MAIN       != TEST_OWNER_SSE &&
              TEST_OWNER_LIBOS_C_PROBE    != TEST_OWNER_PORT_IO_FAULT &&
              TEST_OWNER_LIBOS_C_PROBE    != TEST_OWNER_KERNEL_MEM_FAULT &&
              TEST_OWNER_LIBOS_C_PROBE    != TEST_OWNER_IRQ_ENTRY &&
              TEST_OWNER_LIBOS_C_PROBE    != TEST_OWNER_SSE &&
              TEST_OWNER_PORT_IO_FAULT    != TEST_OWNER_KERNEL_MEM_FAULT &&
              TEST_OWNER_PORT_IO_FAULT    != TEST_OWNER_IRQ_ENTRY &&
              TEST_OWNER_PORT_IO_FAULT    != TEST_OWNER_SSE &&
              TEST_OWNER_KERNEL_MEM_FAULT != TEST_OWNER_IRQ_ENTRY &&
              TEST_OWNER_KERNEL_MEM_FAULT != TEST_OWNER_SSE &&
              TEST_OWNER_IRQ_ENTRY        != TEST_OWNER_SSE,
              "address-space-binding test owner ids must be pairwise distinct");

/*
 * Undo everything a ring-3 launch test may have built: clear the fault hook
 * and any handler registered at LIBOS_RETURN_SYSCALL_NUM, then, only if
 * `owner` still has a bound address space (a failed test may have left one),
 * switch back to the kernel's own map, sweep every resource `owner` still
 * holds, and destroy the address space. Safe to call unconditionally from a
 * suite cleanup regardless of whether the test that ran actually got far
 * enough to build one.
 */
void libos_test_teardown_owner(page_owner_t owner);

/*
 * Result of libos_test_launch() (below) -- the switch-in/switch-out VMM
 * status, the value libos_return() was given, and how many times the
 * fault hook fired. Left for the caller to CU_ASSERT on, the same division
 * libos_test_teardown_owner() already draws: this file performs kernel-side
 * mechanics, the test file makes the claims.
 */
typedef struct {
    int      switch_in_status;   /* vmm_switch_address_space(img->pml4_phys) */
    uint64_t result;             /* value passed to libos_return() */
    int      switch_out_status;  /* vmm_switch_address_space(vmm_kernel_pml4()) */
    int      fault_count;        /* times the fault hook fired during the run */
} libos_test_launch_result_t;

/*
 * The launch/run/unwind boilerplate every suite that drives libos_enter() to
 * completion needs (test_libos_main_k.c, test_libos_c_probe_k.c): registers
 * libos_return() and a fault-counting hook, switches into `img`'s address
 * space, runs libos_enter(img->entry_vaddr, img->stack_top_vaddr), switches
 * back to the kernel's own map, then unregisters both. Building `img` (via
 * libos_build_image()) and tearing it down (libos_destroy_image() or
 * libos_test_teardown_owner()) stay the caller's job -- this only covers the
 * run in between, which is identical across every such suite regardless of
 * what code is actually launched.
 */
libos_test_launch_result_t libos_test_launch(const libos_image_t *img);
