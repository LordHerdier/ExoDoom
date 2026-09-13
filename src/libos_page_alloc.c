/*
 * libos_page_alloc.c — LibOS-side page allocator (SCRUM-37). See
 * libos_page_alloc.h for the design writeup.
 *
 * This drives the real dispatch path, exo_syscall_dispatch(EXO_SYS_PAGE_*,
 * ...) — the same route a ring-3 `syscall` takes once syscall_entry.s has
 * marshalled its arguments — the same convention
 * tests/kernel/test_syscall_mem_k.c and test_syscall_serial_k.c use to test
 * a handler from kernel context.
 *
 * It deliberately does NOT go through the inline exo_page_alloc()/
 * exo_page_map()/... stubs in exo_syscall.h, even though those are the
 * "real" LibOS-side call convention. Those stubs execute the actual
 * `syscall` instruction, and syscall_entry.s always returns via `sysretq`,
 * which unconditionally forces CPL 3 on the way out (src/syscall_entry.s:
 * "CS = STAR[63:48] + 16 ... forced to CPL 3 — the CPU does not consult
 * RCX/R11 for anything but RIP and RFLAGS"). `syscall` itself doesn't care
 * what privilege level issued it, but there is no matching leniency on
 * return: calling the stub from code that must resume at CPL 0 — this
 * allocator, running as part of the kernel binary today — silently drops
 * the CPU to CPL 3 for everything that runs afterward. That is exactly what
 * happened the first time this file called the stubs directly: later
 * kernel-only operations in the same boot started failing in ways that hung
 * the test run instead of failing cleanly.
 *
 * Calling exo_syscall_dispatch() instead is a plain C call — no CPL
 * transition at all — so it proves the same handler-side behavior
 * (ownership stamps, page-table effects, error codes) without that hazard.
 * The inline stubs remain the correct call convention for genuine ring-3
 * code; using them here is out of reach until a real LibOS build/launch
 * harness exists for more than a single hand-written probe function
 * (SCRUM-51/SCRUM-66 territory), because only libos_enter()/libos_return()
 * (src/libos_launch.h) currently manage that CPL transition safely.
 */

#include "libos_page_alloc.h"
#include "exo_syscall.h"
#include "syscall.h"
#include "serial.h"

#include <stddef.h>

/* Base of the virtual window this allocator hands out of: comfortably clear
 * of the fixed libos_launch region and its documented
 * EXO_USER_VA_BASE + 0x20000 fault-target invariant (src/libos_launch.h). */
#define LIBOS_HEAP_VADDR_BASE (EXO_USER_VA_BASE + 0x1000000ULL)
#define PAGE_SIZE 0x1000ULL

static int64_t do_page_alloc(void)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
}

static int64_t do_page_free(uint64_t paddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_FREE, paddr, 0, 0, 0, 0, 0);
}

static int64_t do_page_map(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_MAP, vaddr, paddr, flags,
                                0, 0, 0);
}

static int64_t do_page_unmap(uint64_t vaddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, vaddr, 0, 0, 0, 0, 0);
}

/* Slot i backs the page at LIBOS_HEAP_VADDR_BASE + i * PAGE_SIZE.
 * paddr[i] == 0 means the slot is not currently in use (page 0 is never
 * handed out by exo_page_alloc(), so 0 is a safe sentinel). */
static uint64_t slot_paddr[LIBOS_PAGE_ALLOC_MAX_PAGES];

/* Stack of freed slot indices, for reuse before growing high_water. */
static uint32_t free_slots[LIBOS_PAGE_ALLOC_MAX_PAGES];
static uint32_t free_count;

/* One past the highest slot index ever handed out. */
static uint32_t high_water;

void libos_page_alloc_init(void)
{
    for (uint32_t i = 0; i < LIBOS_PAGE_ALLOC_MAX_PAGES; i++)
        slot_paddr[i] = 0;
    free_count = 0;
    high_water = 0;
}

static void *vaddr_for_slot(uint32_t slot)
{
    return (void *)(uintptr_t)(LIBOS_HEAP_VADDR_BASE + (uint64_t)slot * PAGE_SIZE);
}

void *libos_page_alloc(void)
{
    uint32_t slot;

    if (free_count > 0) {
        slot = free_slots[--free_count];
    } else {
        if (high_water >= LIBOS_PAGE_ALLOC_MAX_PAGES)
            return NULL;
        slot = high_water++;
    }

    int64_t paddr = do_page_alloc();
    if (paddr < 0) {
        free_slots[free_count++] = slot;
        return NULL;
    }

    void *vaddr = vaddr_for_slot(slot);
    int64_t rc = do_page_map((uint64_t)(uintptr_t)vaddr, (uint64_t)paddr,
                             EXO_PAGE_WRITE | EXO_PAGE_USER);
    if (rc < 0) {
        if (do_page_free((uint64_t)paddr) < 0)
            serial_print("libos_page_alloc: leaked a page unwinding a failed map\n");
        free_slots[free_count++] = slot;
        return NULL;
    }

    slot_paddr[slot] = (uint64_t)paddr;
    return vaddr;
}

int libos_page_free(void *vaddr)
{
    uint64_t addr = (uint64_t)(uintptr_t)vaddr;

    if (addr < LIBOS_HEAP_VADDR_BASE)
        return -1;
    uint64_t offset = addr - LIBOS_HEAP_VADDR_BASE;
    if (offset % PAGE_SIZE != 0)
        return -1;
    uint64_t slot64 = offset / PAGE_SIZE;
    if (slot64 >= high_water)
        return -1;
    uint32_t slot = (uint32_t)slot64;

    if (slot_paddr[slot] == 0)
        return -1;   /* not currently allocated (double free / bogus vaddr) */

    uint64_t paddr = slot_paddr[slot];

    if (do_page_unmap(addr) < 0)
        return -1;

    /* The slot is done either way past this point: once exo_page_unmap
     * succeeds, `addr` no longer resolves to anything, so there is no
     * "unmap" to retry even if the free below fails. Reclaiming the slot
     * regardless is what keeps a rejected exo_page_free from stranding it
     * (and its now-unreachable physical page) forever -- the alternative
     * is a slot that can never be freed *or* reused. The failure is still
     * surfaced through the return value and a diagnostic, matching
     * page_alloc.c's convention of reporting PMM errors via serial_print
     * rather than silently swallowing them. */
    int64_t free_rc = do_page_free(paddr);

    slot_paddr[slot] = 0;
    free_slots[free_count++] = slot;

    if (free_rc < 0) {
        serial_print("libos_page_free: unmapped but exo_page_free failed, "
                     "page leaked\n");
        return -1;
    }
    return 0;
}
