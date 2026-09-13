#pragma once

#include "page_alloc.h"   /* page_owner_t */

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

_Static_assert(TEST_OWNER_VMM_REGISTRY  != TEST_OWNER_VMM_ADDRSPACE &&
              TEST_OWNER_VMM_REGISTRY  != TEST_OWNER_LIBOS_LAUNCH  &&
              TEST_OWNER_VMM_REGISTRY  != TEST_OWNER_LIBOS_MAIN    &&
              TEST_OWNER_VMM_REGISTRY  != TEST_OWNER_LIBOS_C_PROBE &&
              TEST_OWNER_VMM_ADDRSPACE != TEST_OWNER_LIBOS_LAUNCH  &&
              TEST_OWNER_VMM_ADDRSPACE != TEST_OWNER_LIBOS_MAIN    &&
              TEST_OWNER_VMM_ADDRSPACE != TEST_OWNER_LIBOS_C_PROBE &&
              TEST_OWNER_LIBOS_LAUNCH  != TEST_OWNER_LIBOS_MAIN    &&
              TEST_OWNER_LIBOS_LAUNCH  != TEST_OWNER_LIBOS_C_PROBE &&
              TEST_OWNER_LIBOS_MAIN    != TEST_OWNER_LIBOS_C_PROBE,
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
