/*
 * test_vmm_k.c — kernel page tables (SCRUM-15).
 *
 * These run *on* the map under test: vmm_init() has already loaded CR3 by the
 * time run_tests() is called, so every assertion here is about the live
 * address space, not a simulation of one.  That cuts both ways — a test that
 * unmaps the wrong page takes the machine down rather than failing — so the
 * mapping tests work in a scratch virtual window far above anything the
 * identity map uses, and put back what they borrow.
 */

#include "kunit.h"
#include "vmm.h"
#include "page_alloc.h"
#include "memory.h"
#include "serial.h"
#include "exo_syscall.h"

#include <stdint.h>

/*
 * A virtual window nothing else touches: 64 GiB up, above the 4 GiB the
 * identity map can reach even in principle (QEMU gives us 256 MiB of RAM and a
 * framebuffer just under 4 GiB).  Canonical, and guaranteed unmapped.
 */
#define SCRATCH_VA   0x0000001000000000ULL
/* Second window, 2 MiB-aligned, for the large-page and split tests. */
#define SCRATCH_VA_2M 0x0000001040000000ULL

extern uint8_t _load_start[];

/* The map exists, CR3 points at it, and its tables belong to the kernel. */
static void test_kernel_map_is_live(void)
{
    uint64_t pml4 = vmm_kernel_pml4();

    CU_ASSERT_NOT_EQUAL(pml4, 0);
    CU_ASSERT_EQUAL(pml4 % VMM_PAGE_SIZE, 0);

    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    /* CR3's low 12 bits are PCD/PWT, not part of the address. */
    CU_ASSERT_EQUAL(cr3 & ~0xFFFULL, pml4);

    /* Table pages come from alloc_page(), so a LibOS cannot free one
     * (SCRUM-152).  This is the map's half of the secure-binding guarantee. */
    CU_ASSERT_EQUAL(page_owner((void *)(uintptr_t)pml4), PAGE_OWNER_KERNEL);
    CU_ASSERT_TRUE(vmm_table_pages() > 0);
}

/* The kernel image, its stack and the bump pool are identity-mapped. */
static void test_kernel_image_identity_mapped(void)
{
    uint64_t paddr = 0, flags = 0;
    uint64_t text = (uint64_t)(uintptr_t)&test_kernel_image_identity_mapped;

    CU_ASSERT_EQUAL(vmm_translate(text, &paddr, &flags), VMM_OK);
    CU_ASSERT_EQUAL(paddr, text);
    CU_ASSERT_TRUE((flags & VMM_PRESENT) != 0);

    uint64_t local = (uint64_t)(uintptr_t)&paddr;         /* on the stack */
    CU_ASSERT_EQUAL(vmm_translate(local, &paddr, NULL), VMM_OK);
    CU_ASSERT_EQUAL(paddr, local);

    uint64_t load = (uint64_t)(uintptr_t)_load_start;
    CU_ASSERT_EQUAL(vmm_translate(load, &paddr, NULL), VMM_OK);
    CU_ASSERT_EQUAL(paddr, load);

    /* One page below the bump pointer: the PMM bitmap and owner table live
     * here, and the map has to cover them or the allocator's own metadata
     * would fault. */
    uint64_t heap = (uint64_t)memory_base_address() - VMM_PAGE_SIZE;
    CU_ASSERT_EQUAL(vmm_translate(heap, &paddr, NULL), VMM_OK);
    CU_ASSERT_EQUAL(paddr, heap);
}

/*
 * Test builds map the identity range user-accessible, matching the
 * --defsym RING3_PROBE=1 gate boot.s gets from build.sh: the ring-3 probe in
 * test_syscall_k.c runs against *these* tables, and the U/S bit is only
 * honoured when set at every level.  The C-side mirror of build.sh's step 1b.
 * (A shipped kernel sets no USER bit; this file only ever builds with
 * -DTESTING, so that direction is build.sh's to assert.)
 */
static void test_testing_build_maps_user_accessible(void)
{
    uint64_t flags = 0;
    uint64_t text = (uint64_t)(uintptr_t)&test_testing_build_maps_user_accessible;

    CU_ASSERT_EQUAL(vmm_translate(text, NULL, &flags), VMM_OK);
    CU_ASSERT_TRUE((flags & VMM_USER) != 0);
}

/* Page 0 is left unmapped so a NULL dereference faults instead of quietly
 * reading the interrupt vector table. */
static void test_null_page_unmapped(void)
{
    CU_ASSERT_EQUAL(vmm_translate(0, NULL, NULL), VMM_ENOENT);
    /* ...but the rest of low memory is mapped. */
    uint64_t paddr = 0;
    CU_ASSERT_EQUAL(vmm_translate(0x1000, &paddr, NULL), VMM_OK);
    CU_ASSERT_EQUAL(paddr, 0x1000);
}

/* Nothing is mapped in the scratch window before the tests below use it. */
static void test_unmapped_address_translates_to_enoent(void)
{
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_VA, NULL, NULL), VMM_ENOENT);
    CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_VA), VMM_ENOENT);
}

/* Map a real page at a scratch address, prove the mapping carries data, then
 * unmap it.  The round trip through two virtual addresses is what proves the
 * MMU is actually consulting the new tables. */
static void test_map_write_unmap_roundtrip(void)
{
    void *page = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(page);
    if (page == NULL) return;
    uint64_t phys = (uint64_t)(uintptr_t)page;

    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_VA, phys, VMM_PRESENT | VMM_WRITE),
                    VMM_OK);

    uint64_t resolved = 0;
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_VA, &resolved, NULL), VMM_OK);
    CU_ASSERT_EQUAL(resolved, phys);

    /* Offsets within the page survive translation. */
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_VA + 0x2A0, &resolved, NULL), VMM_OK);
    CU_ASSERT_EQUAL(resolved, phys + 0x2A0);

    /* Write through the new mapping, read back through the identity one. */
    volatile uint64_t *via_scratch = (volatile uint64_t *)(uintptr_t)SCRATCH_VA;
    volatile uint64_t *via_identity = (volatile uint64_t *)(uintptr_t)phys;

    *via_scratch = 0xC0FFEE0DDF00DULL;
    CU_ASSERT_EQUAL(*via_identity, 0xC0FFEE0DDF00DULL);

    CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_VA), VMM_OK);
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_VA, NULL, NULL), VMM_ENOENT);

    /* Unmapping does not free the page — the PMM still has it. */
    CU_ASSERT_EQUAL(page_owner(page), PAGE_OWNER_KERNEL);
    free_page(page);
}

/* Remapping the same physical page is idempotent; repointing a live mapping
 * somewhere else is refused rather than silently honoured. */
static void test_remap_rules(void)
{
    void *a = alloc_page();
    void *b = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(a);
    CU_ASSERT_PTR_NOT_NULL(b);
    if (a == NULL || b == NULL) return;

    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_VA, (uint64_t)(uintptr_t)a,
                                 VMM_PRESENT | VMM_WRITE), VMM_OK);
    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_VA, (uint64_t)(uintptr_t)a,
                                 VMM_PRESENT | VMM_WRITE), VMM_OK);
    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_VA, (uint64_t)(uintptr_t)b,
                                 VMM_PRESENT | VMM_WRITE), VMM_EEXIST);

    /* After an unmap the address is free to point elsewhere. */
    CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_VA), VMM_OK);
    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_VA, (uint64_t)(uintptr_t)b,
                                 VMM_PRESENT | VMM_WRITE), VMM_OK);
    CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_VA), VMM_OK);

    free_page(a);
    free_page(b);
}

/* Unaligned and non-canonical addresses are rejected, not truncated. */
static void test_alignment_and_canonical_checks(void)
{
    void *page = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(page);
    if (page == NULL) return;
    uint64_t phys = (uint64_t)(uintptr_t)page;

    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_VA + 1, phys, VMM_PRESENT), VMM_EINVAL);
    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_VA, phys + 8, VMM_PRESENT), VMM_EINVAL);
    /* Bits 63:48 do not repeat bit 47 — the CPU would fault on this address
     * before the walk even started. */
    CU_ASSERT_EQUAL(vmm_map_page(0x0001000000000000ULL, phys, VMM_PRESENT),
                    VMM_EINVAL);
    CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_VA + 1), VMM_EINVAL);

    free_page(page);
}

/*
 * A 2 MiB leaf, then a 4 KiB map inside it.  The split has to preserve the 511
 * pages it does not touch, which is the property that lets exo_page_map
 * (SCRUM-153) hand out single pages inside bulk-mapped regions.
 *
 * The alias maps kernel physical memory (2 MiB at _load_start, which is
 * 2 MiB-aligned by the linker script) at a scratch virtual address; nothing
 * writes through it, and it is torn down page by page at the end.
 */
static void test_large_page_split_preserves_neighbours(void)
{
    uint64_t phys_2m = (uint64_t)(uintptr_t)_load_start;
    CU_ASSERT_EQUAL(phys_2m % VMM_LARGE_PAGE_SIZE, 0);
    if (phys_2m % VMM_LARGE_PAGE_SIZE != 0) return;

    /* Taken before the alias exists: bailing out between the map and the
     * teardown loop would leave a writable second view of the kernel image
     * mapped for the rest of the boot. */
    void *page = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(page);
    if (page == NULL) return;

    CU_ASSERT_EQUAL(vmm_map_range(SCRATCH_VA_2M, phys_2m, VMM_LARGE_PAGE_SIZE,
                                  VMM_PRESENT | VMM_WRITE), VMM_OK);

    /* One PDE covers the whole block: the map cost a table page for the PDPT
     * and PD, but no page table. */
    uint64_t resolved = 0;
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_VA_2M + 0x1F0000, &resolved, NULL),
                    VMM_OK);
    CU_ASSERT_EQUAL(resolved, phys_2m + 0x1F0000);

    /* Now repoint a single page in the middle of it. */
    uint64_t odd_va = SCRATCH_VA_2M + 0x8000;

    CU_ASSERT_EQUAL(vmm_unmap_page(odd_va), VMM_OK);   /* splits the leaf */
    CU_ASSERT_EQUAL(vmm_map_page(odd_va, (uint64_t)(uintptr_t)page,
                                 VMM_PRESENT | VMM_WRITE), VMM_OK);

    CU_ASSERT_EQUAL(vmm_translate(odd_va, &resolved, NULL), VMM_OK);
    CU_ASSERT_EQUAL(resolved, (uint64_t)(uintptr_t)page);

    /* Its neighbours still point where the 2 MiB leaf put them. */
    CU_ASSERT_EQUAL(vmm_translate(odd_va - VMM_PAGE_SIZE, &resolved, NULL),
                    VMM_OK);
    CU_ASSERT_EQUAL(resolved, phys_2m + 0x8000 - VMM_PAGE_SIZE);
    CU_ASSERT_EQUAL(vmm_translate(odd_va + VMM_PAGE_SIZE, &resolved, NULL),
                    VMM_OK);
    CU_ASSERT_EQUAL(resolved, phys_2m + 0x8000 + VMM_PAGE_SIZE);
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_VA_2M + 0x1F0000, &resolved, NULL),
                    VMM_OK);
    CU_ASSERT_EQUAL(resolved, phys_2m + 0x1F0000);

    /* Tear the alias down again so no test leaves a second view of kernel
     * memory behind. */
    for (uint64_t off = 0; off < VMM_LARGE_PAGE_SIZE; off += VMM_PAGE_SIZE) {
        CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_VA_2M + off), VMM_OK);
    }
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_VA_2M, NULL, NULL), VMM_ENOENT);

    free_page(page);
}

/* vmm_map_range validates the same way vmm_map_page does — it has its own
 * 2 MiB path that never reaches those checks. */
static void test_map_range_validates(void)
{
    uint64_t phys = (uint64_t)(uintptr_t)_load_start;

    CU_ASSERT_EQUAL(vmm_map_range(SCRATCH_VA + 1, phys, VMM_PAGE_SIZE,
                                  VMM_PRESENT), VMM_EINVAL);
    CU_ASSERT_EQUAL(vmm_map_range(SCRATCH_VA, phys + 1, VMM_PAGE_SIZE,
                                  VMM_PRESENT), VMM_EINVAL);
    /* Non-canonical, and 2 MiB-aligned so it would take the large-page path. */
    CU_ASSERT_EQUAL(vmm_map_range(0x0001000040000000ULL, phys,
                                  VMM_LARGE_PAGE_SIZE, VMM_PRESENT),
                    VMM_EINVAL);
}

/* A conflicting 4 KiB map inside a 2 MiB leaf is refused *before* the leaf is
 * split, so a rejected request costs no page table. */
static void test_conflicting_map_does_not_split(void)
{
    uint64_t phys_2m = (uint64_t)(uintptr_t)_load_start;

    CU_ASSERT_EQUAL(vmm_map_range(SCRATCH_VA_2M, phys_2m, VMM_LARGE_PAGE_SIZE,
                                  VMM_PRESENT | VMM_WRITE), VMM_OK);

    uint32_t before = vmm_table_pages();
    /* Points somewhere else than the leaf says: conflict. */
    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_VA_2M + 0x8000, phys_2m,
                                 VMM_PRESENT | VMM_WRITE), VMM_EEXIST);
    CU_ASSERT_EQUAL(vmm_table_pages(), before);

    /* Still a single leaf: the neighbours are untouched. */
    uint64_t resolved = 0;
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_VA_2M + 0x8000, &resolved, NULL),
                    VMM_OK);
    CU_ASSERT_EQUAL(resolved, phys_2m + 0x8000);

    for (uint64_t off = 0; off < VMM_LARGE_PAGE_SIZE; off += VMM_PAGE_SIZE) {
        CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_VA_2M + off), VMM_OK);
    }
}

/*
 * ── Per-LibOS address spaces (SCRUM-48) ──────────────────────────────────
 *
 * A window in the LibOS mapping range, apart from every other suite's
 * scratch addresses (test_page_map_k.c uses EXO_USER_VA_BASE + 0x20000000/
 * 0x24000000). These tests build and tear down real address spaces of their
 * own rather than borrowing the kernel's, and none of them touch CR3 for
 * longer than a single test — vmm_switch_address_space() is proven to work
 * and then immediately reverted.
 */
#define ADDRSPACE_SCRATCH_VA (EXO_USER_VA_BASE + 0x10000000ULL)

/* A context id these tests own for their own create/bind/destroy lifecycle,
 * distinct from OTHER_LIBOS (test_page_map_k.c, +1) and
 * REGISTRY_SCRATCH_OWNER (below, +3) so none of the suites' registry state
 * collides. */
#define ADDRSPACE_TEST_OWNER ((page_owner_t)(PAGE_OWNER_LIBOS + 4))

/* kernel_main binds the one v1 LibOS to the kernel's own map (SCRUM-47 has
 * not landed a private one yet) — this is the observable proof that the
 * boot-time wiring in kernel.c actually ran. */
static void test_libos_bound_to_kernel_map_by_default(void)
{
    CU_ASSERT_EQUAL(vmm_address_space_for(PAGE_OWNER_LIBOS), vmm_kernel_pml4());
}

/* A new address space's PML4[0] is the kernel's own PDPT link, not a copy of
 * what it points to — the property that makes kernel memory agree between
 * address spaces without synchronizing anything. */
static void test_create_address_space_shares_kernel_subtree(void)
{
    uint64_t new_phys = 0;
    CU_ASSERT_EQUAL(vmm_create_address_space(&new_phys), VMM_OK);
    CU_ASSERT_NOT_EQUAL(new_phys, 0);
    CU_ASSERT_EQUAL(new_phys % VMM_PAGE_SIZE, 0);

    uint64_t *new_pml4 = (uint64_t *)(uintptr_t)new_phys;
    uint64_t *kernel_pml4_ptr = (uint64_t *)(uintptr_t)vmm_kernel_pml4();
    CU_ASSERT_EQUAL(new_pml4[0], kernel_pml4_ptr[0]);

    uint64_t text =
        (uint64_t)(uintptr_t)&test_create_address_space_shares_kernel_subtree;
    uint64_t resolved_new = 0, resolved_kernel = 0;
    CU_ASSERT_EQUAL(vmm_translate_in(new_pml4, text, &resolved_new, NULL),
                    VMM_OK);
    CU_ASSERT_EQUAL(vmm_translate(text, &resolved_kernel, NULL), VMM_OK);
    CU_ASSERT_EQUAL(resolved_new, resolved_kernel);

    CU_ASSERT_EQUAL(vmm_bind_address_space(ADDRSPACE_TEST_OWNER, new_phys),
                    VMM_OK);
    CU_ASSERT_EQUAL(vmm_destroy_address_space(ADDRSPACE_TEST_OWNER), VMM_OK);
}

/* The LibOS window starts empty in a new address space, and a mapping made
 * inside it is invisible to the kernel's own map — the actual isolation this
 * ticket exists for, not just presence of a second PML4. */
static void test_new_address_space_window_is_empty_and_isolated(void)
{
    uint64_t new_phys = 0;
    CU_ASSERT_EQUAL(vmm_create_address_space(&new_phys), VMM_OK);
    uint64_t *new_pml4 = (uint64_t *)(uintptr_t)new_phys;

    CU_ASSERT_EQUAL(vmm_translate_in(new_pml4, ADDRSPACE_SCRATCH_VA, NULL, NULL),
                    VMM_ENOENT);

    void *page = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(page);
    if (page != NULL) {
        uint64_t phys = (uint64_t)(uintptr_t)page;

        CU_ASSERT_EQUAL(vmm_map_page_in(new_pml4, ADDRSPACE_SCRATCH_VA, phys,
                                        VMM_PRESENT | VMM_WRITE), VMM_OK);

        uint64_t resolved = 0;
        CU_ASSERT_EQUAL(vmm_translate_in(new_pml4, ADDRSPACE_SCRATCH_VA,
                                         &resolved, NULL), VMM_OK);
        CU_ASSERT_EQUAL(resolved, phys);

        /* The kernel's own map never learns about this mapping. */
        CU_ASSERT_EQUAL(vmm_translate(ADDRSPACE_SCRATCH_VA, NULL, NULL),
                        VMM_ENOENT);

        free_page(page);
    }

    CU_ASSERT_EQUAL(vmm_bind_address_space(ADDRSPACE_TEST_OWNER, new_phys),
                    VMM_OK);
    CU_ASSERT_EQUAL(vmm_destroy_address_space(ADDRSPACE_TEST_OWNER), VMM_OK);
}

/* vmm_switch_address_space actually loads CR3, and the kernel keeps running
 * (serial output survives) on a foreign address space — the precondition
 * SCRUM-47 needs before it can risk an iret into ring 3 against one. */
static void test_switch_address_space_round_trip(void)
{
    uint64_t new_phys = 0;
    CU_ASSERT_EQUAL(vmm_create_address_space(&new_phys), VMM_OK);

    uint64_t original_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(original_cr3));

    CU_ASSERT_EQUAL(vmm_switch_address_space(new_phys), VMM_OK);

    uint64_t cr3_after_switch;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_after_switch));
    CU_ASSERT_EQUAL(cr3_after_switch & ~0xFFFULL, new_phys);

    /* If the kernel's own code/stack were not reachable from this address
     * space, this would triple-fault the machine rather than fail an
     * assertion — the shared PML4[0] subtree is what keeps it alive. */
    serial_print("vmm: (test) alive on a LibOS address space\n");

    CU_ASSERT_EQUAL(vmm_switch_address_space(vmm_kernel_pml4()), VMM_OK);

    uint64_t cr3_restored;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_restored));
    CU_ASSERT_EQUAL(cr3_restored, original_cr3);

    CU_ASSERT_EQUAL(vmm_bind_address_space(ADDRSPACE_TEST_OWNER, new_phys),
                    VMM_OK);
    CU_ASSERT_EQUAL(vmm_destroy_address_space(ADDRSPACE_TEST_OWNER), VMM_OK);
}

/* vmm_switch_address_space refuses 0 (vmm_address_space_for()'s "unbound"
 * sentinel) rather than loading CR3 with the deliberately-unmapped NULL-guard
 * page — the failure mode that would otherwise triple-fault the machine. */
static void test_switch_address_space_rejects_zero(void)
{
    CU_ASSERT_EQUAL(vmm_switch_address_space(0), VMM_EINVAL);

    /* Refused, so CR3 must be exactly where it was. */
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    CU_ASSERT_EQUAL(cr3 & ~0xFFFULL, vmm_kernel_pml4());
}

/* Destroying an owner with nothing bound — including a second call right
 * after the first already tore the address space down — is refused rather
 * than walking whatever now occupies the stale physical address. */
static void test_destroy_unbound_owner_is_enoent(void)
{
    CU_ASSERT_EQUAL(vmm_destroy_address_space(ADDRSPACE_TEST_OWNER), VMM_ENOENT);

    uint64_t new_phys = 0;
    CU_ASSERT_EQUAL(vmm_create_address_space(&new_phys), VMM_OK);
    CU_ASSERT_EQUAL(vmm_bind_address_space(ADDRSPACE_TEST_OWNER, new_phys),
                    VMM_OK);

    CU_ASSERT_EQUAL(vmm_destroy_address_space(ADDRSPACE_TEST_OWNER), VMM_OK);
    /* The first destroy already unbound it — a second call finds nothing. */
    CU_ASSERT_EQUAL(vmm_destroy_address_space(ADDRSPACE_TEST_OWNER), VMM_ENOENT);
    CU_ASSERT_EQUAL(vmm_address_space_for(ADDRSPACE_TEST_OWNER), 0);
}

/* PAGE_OWNER_FREE and PAGE_OWNER_KERNEL name no schedulable context — same
 * refusal src/revoke.c gives them for revocation. */
static void test_bind_rejects_free_and_kernel(void)
{
    CU_ASSERT_EQUAL(vmm_bind_address_space(PAGE_OWNER_FREE, 0x1000), VMM_EINVAL);
    CU_ASSERT_EQUAL(vmm_bind_address_space(PAGE_OWNER_KERNEL, 0x1000), VMM_EINVAL);
}

/* A scratch context id the registry never sees outside this test — the
 * bound value need not be a real PML4, since bind/lookup/unbind never
 * dereference it. */
#define REGISTRY_SCRATCH_OWNER ((page_owner_t)(PAGE_OWNER_LIBOS + 3))

static void test_bind_rebind_and_unbind(void)
{
    CU_ASSERT_EQUAL(vmm_bind_address_space(REGISTRY_SCRATCH_OWNER, 0x2000),
                    VMM_OK);
    CU_ASSERT_EQUAL(vmm_address_space_for(REGISTRY_SCRATCH_OWNER), 0x2000ULL);

    /* Rebinding overwrites the existing slot rather than filling a new one. */
    CU_ASSERT_EQUAL(vmm_bind_address_space(REGISTRY_SCRATCH_OWNER, 0x3000),
                    VMM_OK);
    CU_ASSERT_EQUAL(vmm_address_space_for(REGISTRY_SCRATCH_OWNER), 0x3000ULL);

    vmm_unbind_address_space(REGISTRY_SCRATCH_OWNER);
    CU_ASSERT_EQUAL(vmm_address_space_for(REGISTRY_SCRATCH_OWNER), 0);
}

void suite_vmm_tests(CU_pSuite s)
{
    CU_add_test(s, "kernel map is live in CR3", test_kernel_map_is_live);
    CU_add_test(s, "kernel image is identity mapped",
                test_kernel_image_identity_mapped);
    CU_add_test(s, "test build maps user-accessible",
                test_testing_build_maps_user_accessible);
    CU_add_test(s, "page 0 is unmapped", test_null_page_unmapped);
    CU_add_test(s, "unmapped address reports ENOENT",
                test_unmapped_address_translates_to_enoent);
    CU_add_test(s, "map/write/unmap round trip", test_map_write_unmap_roundtrip);
    CU_add_test(s, "remap rules", test_remap_rules);
    CU_add_test(s, "alignment and canonical checks",
                test_alignment_and_canonical_checks);
    CU_add_test(s, "map_range validates its arguments", test_map_range_validates);
    CU_add_test(s, "conflicting map does not split",
                test_conflicting_map_does_not_split);
    CU_add_test(s, "2 MiB split preserves neighbours",
                test_large_page_split_preserves_neighbours);

    CU_add_test(s, "LibOS context bound to kernel map by default",
                test_libos_bound_to_kernel_map_by_default);
    CU_add_test(s, "new address space shares kernel subtree",
                test_create_address_space_shares_kernel_subtree);
    CU_add_test(s, "new address space window is empty and isolated",
                test_new_address_space_window_is_empty_and_isolated);
    CU_add_test(s, "switch address space round trip",
                test_switch_address_space_round_trip);
    CU_add_test(s, "switch address space rejects zero",
                test_switch_address_space_rejects_zero);
    CU_add_test(s, "destroy of an unbound owner is ENOENT",
                test_destroy_unbound_owner_is_enoent);
    CU_add_test(s, "bind rejects FREE and KERNEL",
                test_bind_rejects_free_and_kernel);
    CU_add_test(s, "bind/rebind/unbind registry", test_bind_rebind_and_unbind);
}
