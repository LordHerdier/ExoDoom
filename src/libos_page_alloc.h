#ifndef LIBOS_PAGE_ALLOC_H
#define LIBOS_PAGE_ALLOC_H

#include <stdint.h>

/*
 * libos_page_alloc — LibOS-side page allocator (SCRUM-37).
 *
 * The first rung of the "Phase 5 — LibOS heap" ladder in docs/memory.md §8:
 * a page-granularity allocator that gets its memory through the memory
 * syscalls (EXO_SYS_PAGE_ALLOC/_MAP/_UNMAP/_FREE, src/exo_syscall.h), never
 * by touching src/page_alloc.c directly. src/libos_heap.c (SCRUM-38) is the
 * byte-granularity allocator built on top of this one, the same way
 * src/heap.c sits on alloc_page().
 *
 * This drives the real dispatcher (exo_syscall_dispatch(), src/syscall.c)
 * rather than the inline `syscall`-instruction stubs in exo_syscall.h — see
 * libos_page_alloc.c's top comment for why: those stubs return via
 * `sysretq`, which unconditionally forces CPL 3, so calling one from code
 * that must resume at CPL 0 (this allocator, linked into the kernel binary
 * today, with no separate LibOS build target to run it from ring 3 instead —
 * that gap is what blocks SCRUM-51/SCRUM-66 from running a *compiled* libc
 * shim in ring 3, see SCRUM-172's sibling discussion) silently drops the CPU
 * to ring 3 for everything that runs afterward. Calling the dispatcher
 * directly is the same convention tests/kernel/test_syscall_mem_k.c and
 * test_syscall_serial_k.c already use to test a handler from kernel context.
 *
 * Not in scope here: re-pointing src/stdlib.c's malloc/free/realloc at this
 * allocator (or at src/libos_heap.c above it). That wiring needs the
 * kernel/LibOS dual-build split SCRUM-51 is blocked on, so it stays out of
 * both SCRUM-37 and SCRUM-38.
 */

/* Reset all internal state — every slot freed, high-water mark back to zero.
 * Exists so KUnit suites can run against a clean allocator without a reboot;
 * production code never needs to call it. */
void libos_page_alloc_init(void);

/* Allocate one page: EXO_SYS_PAGE_ALLOC for the physical page, then
 * EXO_SYS_PAGE_MAP to place it at a fresh virtual address in
 * [LIBOS_HEAP_VADDR_BASE, LIBOS_HEAP_VADDR_BASE + LIBOS_PAGE_ALLOC_MAX_PAGES *
 * 0x1000), read/write/user. Returns that virtual address, or NULL if the
 * underlying syscall failed or the slot table is exhausted. On a mapping
 * failure the physical page is freed before returning, so a failed call
 * never leaks a page the caller has no address to reach. */
void *libos_page_alloc(void);

/* Free a page obtained from libos_page_alloc(): EXO_SYS_PAGE_UNMAP then
 * EXO_SYS_PAGE_FREE. Returns 0 on success, -1 if `vaddr` was never returned
 * by libos_page_alloc() or has already been freed. */
int libos_page_free(void *vaddr);

/* Slot table cap: 16 MiB of LibOS heap headroom, ~2x the ~8 MiB Doom heap
 * estimate in docs/memory.md §8 (§9 has the full breakdown). Exposed so
 * tests can size exhaustion runs without hardcoding the number twice. The
 * virtual base this allocator hands out of (LIBOS_HEAP_VADDR_BASE) is
 * deliberately *not* exposed here: it is expressed in terms of
 * EXO_USER_VA_BASE from exo_syscall.h, which this header does not include to
 * keep it usable from callers that don't otherwise need the syscall ABI.
 * Callers that need it include exo_syscall.h themselves, the same way
 * libos_page_alloc.c does. */
#define LIBOS_PAGE_ALLOC_MAX_PAGES 4096

#endif /* LIBOS_PAGE_ALLOC_H */
