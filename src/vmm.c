/*
 * vmm.c — kernel page tables built from PMM pages (SCRUM-15).
 *
 * See vmm.h for what this replaces and why.  Two invariants hold throughout
 * this file and everything that calls it:
 *
 *   - The map is an *identity* map: virtual == physical.  That is what makes
 *     it safe to walk and edit page tables through their physical addresses
 *     (`(uint64_t *)phys`), here and in the tables' own construction.  When
 *     SCRUM-48 gives a LibOS its own address space, the kernel half stays
 *     identity-mapped, so this stays true for the kernel.
 *   - Tables are allocated with alloc_page(), i.e. PAGE_OWNER_KERNEL, so a
 *     LibOS calling exo_page_free on one gets -EXO_EPERM (SCRUM-152).  Never
 *     switch these to alloc_page_owned() with a LibOS id.
 */

#include <stddef.h>
#include <stdint.h>

#include "vmm.h"
#include "exo_syscall.h"  /* EXO_USER_VA_BASE/END — the LibOS window this file
                           * carves out of every address space it builds */
#include "memory.h"
#include "mmap.h"
#include "multiboot2.h"
#include "page_alloc.h"
#include "serial.h"

/* Bits 51:12 of an entry — the physical address of the next table or leaf. */
#define ENTRY_ADDR_MASK 0x000FFFFFFFFFF000ULL
/* Bits 11:0 — every flag this kernel sets.  Anything a caller passes outside
 * this mask is dropped rather than trusted: bit 63 (NX) faults the walk while
 * EFER.NXE is clear, and the rest are reserved. */
#define ENTRY_FLAG_MASK 0x0000000000000FFFULL

#define PML4_IDX(v) (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FF)
#define PD_IDX(v)   (((v) >> 21) & 0x1FF)
#define PT_IDX(v)   (((v) >> 12) & 0x1FF)

/*
 * The LibOS window's PML4 index range, derived from the same constants
 * exo_syscall.h defines rather than restated as literals — the two must
 * never drift apart (see docs/syscall_spec.md's account of the 4 GiB window
 * base that "looked correct" until SCRUM-15 mapped more RAM under it).
 * EXO_USER_VA_BASE/END both land exactly on a 512 GiB (PML4 slot) boundary,
 * so no rounding is needed: START is inclusive, END is exclusive.
 */
#define VMM_LIBOS_PML4_START ((unsigned)PML4_IDX(EXO_USER_VA_BASE))
#define VMM_LIBOS_PML4_END   ((unsigned)PML4_IDX(EXO_USER_VA_END))

_Static_assert(VMM_LIBOS_PML4_START > 0,
               "the LibOS window must not reach into PML4[0], the slot every "
               "address space shares with the kernel's own map");

extern uint8_t _load_start[];

/*
 * U/S on the kernel's own map, mirroring the gate in boot.s.
 *
 * A shipped kernel maps nothing user-accessible.  A TESTING build has to,
 * because tests/kernel/ring3_probe.s executes at CPL 3 against these very
 * tables and the U/S bit is only honoured when set at every level of the walk
 * — the same reason build.sh assembles boot.s with --defsym RING3_PROBE=1.
 * Keeping the two in step matters: vmm_init() loads CR3 before run_tests(), so
 * from that point on it is *this* map the probe runs against, not boot.s's.
 *
 * The exposure is identical to the boot map's (all mapped memory readable and
 * writable from ring 3) and closes the same way: SCRUM-48 gives the LibOS its
 * own address space, SCRUM-55/-56 assert the wall.
 */
#ifdef TESTING
#define KERNEL_MAP_USER VMM_USER
#else
#define KERNEL_MAP_USER 0
#endif

/* Kernel data/code leaves: present, writable, and user only in test builds. */
#define KERNEL_LEAF (VMM_PRESENT | VMM_WRITE | KERNEL_MAP_USER)

/*
 * Links are permissive, but not blindly so: the U/S bit is ANDed across the
 * whole walk, so a link without it silently overrides a leaf that has it.  A
 * link therefore carries USER exactly when the leaf being installed under it
 * asks for it — otherwise vmm_map_page(..., VMM_USER) would return VMM_OK and
 * still fault at CPL 3, which is the shape of bug SCRUM-153 would inherit.
 * R/W is always set at the link level and left for the leaf to restrict.
 */
static uint64_t link_flags_for(uint64_t leaf_flags) {
    return VMM_PRESENT | VMM_WRITE | (leaf_flags & VMM_USER);
}

static uint64_t *kernel_pml4 = NULL;
static uint32_t table_pages = 0;
/* Set once CR3 holds our PML4.  Before that, TLB flushes are pointless (the
 * boot map is live and none of our entries are cached). */
static int map_active = 0;

/* ── Low-level helpers ────────────────────────────────────────────────── */

static uint64_t align_down(uint64_t v, uint64_t align) {
    return v & ~(align - 1);
}

static uint64_t align_up(uint64_t v, uint64_t align) {
    return (v + align - 1) & ~(align - 1);
}

/* Bits 63:48 must repeat bit 47, or the CPU faults on the address before it
 * ever reaches the tables. */
static int is_canonical(uint64_t v) {
    uint64_t top = v >> 47;
    return top == 0 || top == 0x1FFFF;
}

static uint64_t *alloc_table(void) {
    void *p = alloc_page();          /* PAGE_OWNER_KERNEL — see file header */
    if (p == NULL) {
        serial_print("vmm: out of pages for a page table\n");
        return NULL;
    }

    uint64_t *table = (uint64_t *)p; /* identity map: physical == virtual */
    for (unsigned i = 0; i < 512; i++) {
        table[i] = 0;
    }

    table_pages++;
    return table;
}

static uint64_t *entry_table(uint64_t entry) {
    return (uint64_t *)(uintptr_t)(entry & ENTRY_ADDR_MASK);
}

/* True when `pml4` is the tree currently loaded in CR3.  The TLB only ever
 * caches translations sourced from the active CR3, so editing a *different*
 * tree — a LibOS address space while the kernel's own map is still active,
 * or vice versa — leaves nothing stale to flush there; flushing anyway would
 * invalidate the wrong tree's entries instead. */
static int is_active_tree(uint64_t *pml4) {
    if (!map_active) {
        return 0;
    }
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return (cr3 & ~0xFFFULL) == (uint64_t)(uintptr_t)pml4;
}

static void flush_page(uint64_t *pml4, uint64_t vaddr) {
    if (is_active_tree(pml4)) {
        __asm__ volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
    }
}

/* Reload CR3 — the cheap way to drop every TLB entry for a 2 MB block after a
 * split, instead of 512 invlpg. */
static void flush_all(uint64_t *pml4) {
    if (is_active_tree(pml4)) {
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
    }
}

/*
 * Fetch the next level down, creating it when `leaf_flags` is non-zero (the
 * flags of the leaf that will eventually sit under this link).  Returns NULL
 * when the entry is absent and we are not creating, when allocation fails, or
 * when the entry is a leaf rather than a link — callers that can meet a leaf
 * (2 MB pages) check for it themselves before calling.
 *
 * An existing link is *upgraded* to user-accessible if the new leaf needs it:
 * a link created for a supervisor mapping would otherwise veto a user leaf
 * mapped later under the same PDPT/PD.
 */
static uint64_t *next_level(uint64_t *pml4, uint64_t *table, unsigned index, uint64_t leaf_flags) {
    uint64_t entry = table[index];

    if (entry & VMM_PRESENT) {
        if (entry & VMM_HUGE) {
            return NULL;
        }
        if ((leaf_flags & VMM_USER) && !(entry & VMM_USER)) {
            table[index] = entry | VMM_USER;
            flush_all(pml4);
        }
        return entry_table(entry);
    }
    if (leaf_flags == 0) {
        return NULL;            /* walk only: do not create */
    }

    uint64_t *child = alloc_table();
    if (child == NULL) {
        return NULL;
    }

    table[index] = (uint64_t)(uintptr_t)child | link_flags_for(leaf_flags);
    return child;
}

/*
 * Replace a 2 MB leaf with a page table describing the same 512 pages, so a
 * single 4 KiB page inside it can be remapped without disturbing its
 * neighbours.  The leaf's flags carry over verbatim (minus PS), which is what
 * makes the split invisible to everything except the page being changed.
 */
static int split_large_page(uint64_t *pml4, uint64_t *pd, unsigned index) {
    uint64_t entry = pd[index];
    uint64_t base  = entry & ENTRY_ADDR_MASK;
    uint64_t flags = (entry & ENTRY_FLAG_MASK) & ~VMM_HUGE;

    uint64_t *pt = alloc_table();
    if (pt == NULL) {
        return VMM_ENOMEM;
    }

    for (unsigned i = 0; i < 512; i++) {
        pt[i] = (base + (uint64_t)i * VMM_PAGE_SIZE) | flags;
    }

    /*
     * Break before make.  Changing the page size of a live translation in one
     * store lets the TLB hold a 2 MB and a 4 KiB entry for the same linear
     * address at once, which the SDM (Vol. 3 §4.10.4.4) leaves undefined.
     * QEMU never notices; real hardware may.  So: drop the leaf, flush, then
     * install the table.
     */
    pd[index] = 0;
    flush_all(pml4);

    pd[index] = (uint64_t)(uintptr_t)pt | link_flags_for(flags);
    flush_all(pml4);
    return VMM_OK;
}

/* ── Mapping ──────────────────────────────────────────────────────────── */

int vmm_map_page_in(uint64_t *pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    if (pml4 == NULL) {
        return VMM_EINVAL;
    }
    if ((vaddr % VMM_PAGE_SIZE) != 0 || (paddr % VMM_PAGE_SIZE) != 0 ||
        !is_canonical(vaddr)) {
        return VMM_EINVAL;
    }

    /* Sanitised once, then used for both the leaf and the links below. */
    uint64_t leaf = (flags & ENTRY_FLAG_MASK) | VMM_PRESENT;

    uint64_t *pdpt = next_level(pml4, pml4, PML4_IDX(vaddr), leaf);
    if (pdpt == NULL) {
        return VMM_ENOMEM;
    }

    /* 1 GB leaves are never created here; meeting one means someone else's
     * mapping covers this address and splitting it is not implemented. */
    if (pdpt[PDPT_IDX(vaddr)] & VMM_HUGE) {
        return VMM_EEXIST;
    }

    uint64_t *pd = next_level(pml4, pdpt, PDPT_IDX(vaddr), leaf);
    if (pd == NULL) {
        return VMM_ENOMEM;
    }

    uint64_t pde = pd[PD_IDX(vaddr)];
    if ((pde & VMM_PRESENT) && (pde & VMM_HUGE)) {
        /* Decide before splitting: a 2 MB leaf pointing somewhere else is the
         * same conflict as a 4 KiB one, and splitting first would spend a page
         * table on a request that is about to be refused. */
        uint64_t covered = (pde & ENTRY_ADDR_MASK) + (vaddr & (VMM_LARGE_PAGE_SIZE - 1));
        if (covered != paddr) {
            return VMM_EEXIST;
        }

        int rc = split_large_page(pml4, pd, PD_IDX(vaddr));
        if (rc != VMM_OK) {
            return rc;
        }
    }

    uint64_t *pt = next_level(pml4, pd, PD_IDX(vaddr), leaf);
    if (pt == NULL) {
        return VMM_ENOMEM;
    }

    uint64_t existing = pt[PT_IDX(vaddr)];
    if ((existing & VMM_PRESENT) && (existing & ENTRY_ADDR_MASK) != paddr) {
        /* Repointing a live mapping is always a bug at this layer; the caller
         * unmaps first if that is really what it meant. */
        return VMM_EEXIST;
    }

    pt[PT_IDX(vaddr)] = paddr | leaf;
    flush_page(pml4, vaddr);
    return VMM_OK;
}

int vmm_map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    return vmm_map_page_in(kernel_pml4, vaddr, paddr, flags);
}

/*
 * Map a 2 MB-aligned block as a single leaf.  Returns VMM_EEXIST when the
 * block is already described at 4 KiB granularity (or points elsewhere), which
 * vmm_map_range_in() treats as "fall back to 4 KiB pages" rather than an error.
 */
static int map_large_page(uint64_t *pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    uint64_t leaf = (flags & ENTRY_FLAG_MASK) | VMM_PRESENT;

    uint64_t *pdpt = next_level(pml4, pml4, PML4_IDX(vaddr), leaf);
    if (pdpt == NULL) {
        return VMM_ENOMEM;
    }
    if (pdpt[PDPT_IDX(vaddr)] & VMM_HUGE) {
        return VMM_EEXIST;
    }

    uint64_t *pd = next_level(pml4, pdpt, PDPT_IDX(vaddr), leaf);
    if (pd == NULL) {
        return VMM_ENOMEM;
    }

    uint64_t pde = pd[PD_IDX(vaddr)];
    if (pde & VMM_PRESENT) {
        if (!(pde & VMM_HUGE) || (pde & ENTRY_ADDR_MASK) != paddr) {
            return VMM_EEXIST;
        }
    }

    pd[PD_IDX(vaddr)] = paddr | leaf | VMM_HUGE;
    flush_all(pml4);
    return VMM_OK;
}

int vmm_map_range_in(uint64_t *pml4, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags) {
    /* The same three checks vmm_map_page_in makes, up front: the 2 MB path
     * below bypasses it entirely, and would otherwise walk a NULL PML4 (a
     * fault with no handler behind it) or silently truncate a non-canonical
     * address into an unrelated one. */
    if (pml4 == NULL) {
        return VMM_EINVAL;
    }
    if ((vaddr % VMM_PAGE_SIZE) != 0 || (paddr % VMM_PAGE_SIZE) != 0 ||
        !is_canonical(vaddr) || !is_canonical(vaddr + size - 1)) {
        return VMM_EINVAL;
    }

    size = align_up(size, VMM_PAGE_SIZE);

    for (uint64_t off = 0; off < size; ) {
        uint64_t va = vaddr + off;
        uint64_t pa = paddr + off;
        uint64_t remaining = size - off;

        /* A 2 MB leaf costs one PDE instead of a 512-entry table plus the page
         * to hold it, and one TLB entry instead of 512. */
        if ((va % VMM_LARGE_PAGE_SIZE) == 0 && (pa % VMM_LARGE_PAGE_SIZE) == 0 &&
            remaining >= VMM_LARGE_PAGE_SIZE) {

            int rc = map_large_page(pml4, va, pa, flags);
            if (rc == VMM_OK) {
                off += VMM_LARGE_PAGE_SIZE;
                continue;
            }
            if (rc != VMM_EEXIST) {
                return rc;
            }
            /* Already described at 4 KiB granularity — fall through and map
             * this block page by page, which is idempotent where the mapping
             * already matches. */
        }

        int rc = vmm_map_page_in(pml4, va, pa, flags);
        if (rc != VMM_OK) {
            return rc;
        }
        off += VMM_PAGE_SIZE;
    }

    return VMM_OK;
}

int vmm_map_range(uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags) {
    return vmm_map_range_in(kernel_pml4, vaddr, paddr, size, flags);
}

int vmm_unmap_page_in(uint64_t *pml4, uint64_t vaddr) {
    if (pml4 == NULL) {
        return VMM_EINVAL;
    }
    if ((vaddr % VMM_PAGE_SIZE) != 0 || !is_canonical(vaddr)) {
        return VMM_EINVAL;
    }

    /* 0 = walk only, never create: unmapping must not allocate. */
    uint64_t *pdpt = next_level(pml4, pml4, PML4_IDX(vaddr), 0);
    if (pdpt == NULL) {
        return VMM_ENOENT;
    }
    if (pdpt[PDPT_IDX(vaddr)] & VMM_HUGE) {
        return VMM_EINVAL;      /* 1 GB leaf: splitting is not implemented */
    }

    uint64_t *pd = next_level(pml4, pdpt, PDPT_IDX(vaddr), 0);
    if (pd == NULL) {
        return VMM_ENOENT;
    }

    uint64_t pde = pd[PD_IDX(vaddr)];
    if ((pde & VMM_PRESENT) && (pde & VMM_HUGE)) {
        int rc = split_large_page(pml4, pd, PD_IDX(vaddr));
        if (rc != VMM_OK) {
            return rc;
        }
    }

    uint64_t *pt = next_level(pml4, pd, PD_IDX(vaddr), 0);
    if (pt == NULL) {
        return VMM_ENOENT;
    }
    if (!(pt[PT_IDX(vaddr)] & VMM_PRESENT)) {
        return VMM_ENOENT;
    }

    /* The now-empty page table is left in place: reclaiming it means proving
     * all 512 entries are clear on every unmap, and the kernel map is built
     * once and barely edited.  Per-LibOS address spaces tear down whole trees
     * at once instead (vmm_destroy_address_space, SCRUM-48). */
    pt[PT_IDX(vaddr)] = 0;
    flush_page(pml4, vaddr);
    return VMM_OK;
}

int vmm_unmap_page(uint64_t vaddr) {
    return vmm_unmap_page_in(kernel_pml4, vaddr);
}

int vmm_translate_in(uint64_t *pml4, uint64_t vaddr, uint64_t *paddr_out, uint64_t *flags_out) {
    if (pml4 == NULL || !is_canonical(vaddr)) {
        return VMM_EINVAL;
    }

    uint64_t entry = pml4[PML4_IDX(vaddr)];
    if (!(entry & VMM_PRESENT)) {
        return VMM_ENOENT;
    }

    uint64_t *pdpt = entry_table(entry);
    entry = pdpt[PDPT_IDX(vaddr)];
    if (!(entry & VMM_PRESENT)) {
        return VMM_ENOENT;
    }
    if (entry & VMM_HUGE) {     /* 1 GB leaf */
        if (paddr_out) *paddr_out = (entry & ENTRY_ADDR_MASK) | (vaddr & 0x3FFFFFFF);
        if (flags_out) *flags_out = entry & ENTRY_FLAG_MASK;
        return VMM_OK;
    }

    uint64_t *pd = entry_table(entry);
    entry = pd[PD_IDX(vaddr)];
    if (!(entry & VMM_PRESENT)) {
        return VMM_ENOENT;
    }
    if (entry & VMM_HUGE) {     /* 2 MB leaf */
        if (paddr_out) *paddr_out = (entry & ENTRY_ADDR_MASK) | (vaddr & 0x1FFFFF);
        if (flags_out) *flags_out = entry & ENTRY_FLAG_MASK;
        return VMM_OK;
    }

    uint64_t *pt = entry_table(entry);
    entry = pt[PT_IDX(vaddr)];
    if (!(entry & VMM_PRESENT)) {
        return VMM_ENOENT;
    }

    if (paddr_out) *paddr_out = (entry & ENTRY_ADDR_MASK) | (vaddr & 0xFFF);
    if (flags_out) *flags_out = entry & ENTRY_FLAG_MASK;
    return VMM_OK;
}

int vmm_translate(uint64_t vaddr, uint64_t *paddr_out, uint64_t *flags_out) {
    return vmm_translate_in(kernel_pml4, vaddr, paddr_out, flags_out);
}

uint64_t vmm_kernel_pml4(void) {
    return (uint64_t)(uintptr_t)kernel_pml4;
}

int vmm_is_active(void) {
    return map_active;
}

uint32_t vmm_table_pages(void) {
    return table_pages;
}

/* ── Construction ─────────────────────────────────────────────────────── */

/* Identity-map a physical range, rounded out to page boundaries. */
static int map_identity(uint64_t start, uint64_t end, uint64_t flags) {
    start = align_down(start, VMM_PAGE_SIZE);
    end   = align_up(end, VMM_PAGE_SIZE);

    if (end <= start) {
        return VMM_OK;
    }
    return vmm_map_range(start, start, end - start, flags);
}

/* Confirm a critical address survives the new map before CR3 is loaded.  A
 * missing kernel mapping here would otherwise present as a triple fault the
 * instruction after the CR3 write, with nothing on the wire to explain it. */
static int check_mapped(const char *what, uint64_t vaddr) {
    uint64_t paddr;
    if (vmm_translate(vaddr, &paddr, NULL) != VMM_OK || paddr != vaddr) {
        serial_print("vmm: FATAL: ");
        serial_print(what);
        serial_print(" is not identity-mapped (0x");
        serial_print_hex64(vaddr);
        serial_print(")\n");
        return 0;
    }
    return 1;
}

int vmm_init(const struct mb2_info *mb, const struct mb2_tag_framebuffer *fb) {
    if (kernel_pml4 != NULL) {
        return VMM_OK;
    }

    kernel_pml4 = alloc_table();
    if (kernel_pml4 == NULL) {
        serial_print("vmm: cannot allocate PML4\n");
        return VMM_ENOMEM;
    }

    int rc;

    /*
     * 1. Low memory, 4 KiB granularity.
     *
     * Page 0 is deliberately left out: nothing real lives there, and leaving
     * it unmapped turns a NULL dereference into a fault instead of a silent
     * read of the interrupt vector table.  (Until SCRUM-17 lands a page-fault
     * handler that fault is fatal — but a fatal fault at the site of the bug
     * beats corrupted BIOS structures far from it.)
     */
    rc = map_identity(VMM_PAGE_SIZE, 0x100000, KERNEL_LEAF);
    if (rc != VMM_OK) goto fail;

    /*
     * 2. Kernel image and the bump-allocator pool behind it, also at 4 KiB
     * granularity — the split points SCRUM-16 needs to give .text and .rodata
     * their own permissions have to exist before it can set them.
     */
    rc = map_identity((uint64_t)(uintptr_t)_load_start,
                      (uint64_t)memory_base_address(), KERNEL_LEAF);
    if (rc != VMM_OK) goto fail;

    /*
     * 3. Every usable RAM region.  This is what covers the PMM's pool (page
     * tables included), the WAD module and, on a normal boot, the multiboot
     * info struct.  2 MB leaves wherever alignment allows.
     */
    uint32_t count = 0;
    const mmap_region_t *regions = mmap_get_regions(&count);

    for (uint32_t i = 0; i < count; i++) {
        if (regions[i].type != MULTIBOOT_MMAP_AVAILABLE) {
            continue;
        }

        uint64_t start = regions[i].base;
        uint64_t end   = regions[i].base + regions[i].length;

        if (start < VMM_PAGE_SIZE) {
            start = VMM_PAGE_SIZE;      /* keep the NULL guard above */
        }
        if (end <= start) {
            continue;
        }

        rc = map_identity(start, end, KERNEL_LEAF);
        if (rc != VMM_OK) {
            serial_print("vmm: failed to map a usable RAM region\n");
            goto fail;
        }
    }

    /*
     * 4. The multiboot info struct.  Usually inside a usable region already,
     * but firmware is free to place it in reserved memory, and the kernel
     * reads it after the switch (kernel.c walks the tags for the framebuffer
     * geometry).
     */
    if (mb != NULL) {
        rc = map_identity((uint64_t)(uintptr_t)mb,
                          (uint64_t)(uintptr_t)mb + mb->total_size, KERNEL_LEAF);
        if (rc != VMM_OK) goto fail;
    }

    /*
     * 5. The framebuffer aperture.  It sits in the PCI MMIO hole above RAM, so
     * no mmap region covers it — without this the console dies the moment CR3
     * is loaded.  Mapped cached (no PCD): QEMU's framebuffer is coherent, and
     * uncached writes would cost Doom's blit dearly.  Real hardware wants
     * write-combining via MTRR/PAT, which is SCRUM-16's problem.
     */
    if (fb != NULL && fb->addr != 0) {
        uint64_t fb_size = (uint64_t)fb->pitch * fb->height;

        rc = map_identity(fb->addr, fb->addr + fb_size, KERNEL_LEAF);
        if (rc != VMM_OK) {
            serial_print("vmm: failed to map the framebuffer\n");
            goto fail;
        }
    }

    /* Everything the CPU touches across the CR3 write, checked while the boot
     * map is still live and a failure can still be reported. */
    uint64_t stack_probe = (uint64_t)(uintptr_t)&rc;
    int ok = check_mapped("kernel text", (uint64_t)(uintptr_t)&vmm_init)
           & check_mapped("kernel stack", stack_probe)
           & check_mapped("PML4", (uint64_t)(uintptr_t)kernel_pml4);

    if (mb != NULL) {
        ok &= check_mapped("multiboot info", (uint64_t)(uintptr_t)mb);
    }
    if (fb != NULL && fb->addr != 0) {
        ok &= check_mapped("framebuffer", fb->addr);
    }

    if (!ok) {
        rc = VMM_ENOENT;
        goto fail;
    }

    __asm__ volatile ("mov %0, %%cr3" :: "r"((uint64_t)(uintptr_t)kernel_pml4)
                      : "memory");
    map_active = 1;

    serial_print("vmm: kernel page tables active (pml4=0x");
    serial_print_hex64((uint64_t)(uintptr_t)kernel_pml4);
    serial_print(", ");
    serial_print_u32(table_pages);
    serial_print(" table pages)\n");

    return VMM_OK;

fail:
    /*
     * CR3 still holds the boot map, so the kernel keeps running -- but the
     * half-built tree must not stay reachable.  Leaving kernel_pml4 set would
     * make vmm_map_page/vmm_translate edit and report on a tree the CPU is not
     * using, and skip TLB flushes while doing it: wrong answers, no error.
     * The tables allocated so far stay allocated (kernel-owned, never handed
     * out again); a failure here means the machine is out of memory at boot
     * and reclaiming eight pages changes nothing.
     */
    kernel_pml4 = NULL;
    table_pages = 0;
    return rc;
}

/* ── Address spaces beyond the kernel's own (SCRUM-48) ───────────────────
 *
 * A LibOS's PML4 shares kernel_pml4[0] — the same physical PDPT, not a copy
 * of what it points to — so kernel memory needs no synchronization between
 * address spaces, and PML4 indices [VMM_LIBOS_PML4_START, VMM_LIBOS_PML4_END)
 * (the LibOS window, EXO_USER_VA_BASE..END in src/exo_syscall.h) start zero
 * for the caller to map into. That split falls out of the addresses involved
 * rather than anything this file enforces: the window sits at 64 TiB+, which
 * PML4_IDX() puts at index 128, while everything vmm_init() maps — low
 * memory, the kernel image, usable RAM, the framebuffer aperture — lives
 * under index 0. The two ranges have never once needed the same top-level
 * slot.
 */

int vmm_create_address_space(uint64_t *pml4_phys_out) {
    if (kernel_pml4 == NULL) {
        return VMM_EINVAL;      /* nothing to share yet */
    }

    uint64_t *pml4 = alloc_table();     /* PAGE_OWNER_KERNEL, same as kernel_pml4 */
    if (pml4 == NULL) {
        return VMM_ENOMEM;
    }

    pml4[0] = kernel_pml4[0];
    *pml4_phys_out = (uint64_t)(uintptr_t)pml4;
    return VMM_OK;
}

/* Free every present entry under a PT — leaves only, so nothing below them
 * to recurse into.  Does not free the physical pages the leaves point at;
 * see vmm_destroy_address_space()'s header comment for why. */
static void free_pt(uint64_t *pt) {
    free_page(pt);
    table_pages--;
}

static void free_pd(uint64_t *pd) {
    for (unsigned i = 0; i < 512; i++) {
        uint64_t entry = pd[i];
        if (!(entry & VMM_PRESENT) || (entry & VMM_HUGE)) {
            continue;           /* empty, or a 2 MiB leaf — nothing below it */
        }
        free_pt(entry_table(entry));
    }
    free_page(pd);
    table_pages--;
}

static void free_pdpt(uint64_t *pdpt) {
    for (unsigned i = 0; i < 512; i++) {
        uint64_t entry = pdpt[i];
        if (!(entry & VMM_PRESENT) || (entry & VMM_HUGE)) {
            continue;           /* empty, or a 1 GiB leaf — never created here */
        }
        free_pd(entry_table(entry));
    }
    free_page(pdpt);
    table_pages--;
}

static void free_private_subtree(uint64_t *pml4) {
    /* Indices below VMM_LIBOS_PML4_START are never this address space's own
     * tables — index 0 is the shared kernel subtree, and nothing between it
     * and the window is ever populated — so only the window's own range is
     * ever this address space's to free. */
    for (unsigned i = VMM_LIBOS_PML4_START; i < VMM_LIBOS_PML4_END; i++) {
        uint64_t entry = pml4[i];
        if (entry & VMM_PRESENT) {
            free_pdpt(entry_table(entry));
        }
        pml4[i] = 0;
    }
}

/*
 * Tear down `owner`'s address space and remove it from the registry in one
 * call — the two used to be separate (vmm_unbind_address_space +
 * a raw-pml4_phys destroy), which let a caller free the tables while
 * leaving the registry pointing at what is now free memory, or free the
 * same tables twice if it (or anything else) still held the physical
 * address around. Routing destruction through the registry closes both: a
 * second call finds `owner` already unbound and does nothing, and nothing
 * outside this file ever sees a raw PML4 physical address it could replay.
 *
 * Returns VMM_OK, or VMM_ENOENT if `owner` has no address space bound
 * (including a second call after the first already tore it down).
 */
int vmm_destroy_address_space(page_owner_t owner) {
    uint64_t pml4_phys = vmm_address_space_for(owner);
    if (pml4_phys == 0) {
        return VMM_ENOENT;
    }

    /* Unbind first: if anything below faults or is interrupted, the registry
     * still ends up pointing at nothing rather than at memory this function
     * is in the middle of freeing. */
    vmm_unbind_address_space(owner);

    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;
    free_private_subtree(pml4);
    free_page(pml4);
    table_pages--;
    return VMM_OK;
}

int vmm_switch_address_space(uint64_t pml4_phys) {
    if (pml4_phys == 0) {
        return VMM_EINVAL;
    }
    __asm__ volatile ("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
    return VMM_OK;
}

/* ── Per-context address-space registry ──────────────────────────────────
 *
 * See vmm.h: keyed on page_owner_t, same id every other ownership table in
 * the kernel already uses, and deliberately a flat linear-scan table rather
 * than anything smarter — SCRUM-147 is expected to replace this once there
 * is more than one entry worth optimizing for.
 */

typedef struct {
    page_owner_t owner;         /* PAGE_OWNER_FREE marks an empty slot */
    uint64_t     pml4_phys;
} addrspace_binding_t;

static addrspace_binding_t address_spaces[VMM_MAX_ADDRESS_SPACES];

/* Mirrors page_alloc.c's owner_id(): mask off the revocation bit (SCRUM-156)
 * so a context under revocation still resolves to its own address space —
 * revocation marks a resource pending, it does not stop the owner running. */
static page_owner_t owner_id(page_owner_t owner) {
    return owner & PAGE_OWNER_ID_MASK;
}

int vmm_bind_address_space(page_owner_t owner, uint64_t pml4_phys) {
    page_owner_t id = owner_id(owner);
    if (id == PAGE_OWNER_FREE || id == PAGE_OWNER_KERNEL) {
        return VMM_EINVAL;
    }

    int free_slot = -1;
    for (int i = 0; i < VMM_MAX_ADDRESS_SPACES; i++) {
        if (owner_id(address_spaces[i].owner) == id) {
            address_spaces[i].pml4_phys = pml4_phys;
            return VMM_OK;
        }
        if (address_spaces[i].owner == PAGE_OWNER_FREE && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return VMM_ENOMEM;
    }

    address_spaces[free_slot].owner = owner;
    address_spaces[free_slot].pml4_phys = pml4_phys;
    return VMM_OK;
}

uint64_t vmm_address_space_for(page_owner_t owner) {
    page_owner_t id = owner_id(owner);
    for (int i = 0; i < VMM_MAX_ADDRESS_SPACES; i++) {
        if (owner_id(address_spaces[i].owner) == id) {
            return address_spaces[i].pml4_phys;
        }
    }
    return 0;
}

void vmm_unbind_address_space(page_owner_t owner) {
    page_owner_t id = owner_id(owner);
    for (int i = 0; i < VMM_MAX_ADDRESS_SPACES; i++) {
        if (owner_id(address_spaces[i].owner) == id) {
            address_spaces[i].owner = PAGE_OWNER_FREE;
            address_spaces[i].pml4_phys = 0;
            return;
        }
    }
}
