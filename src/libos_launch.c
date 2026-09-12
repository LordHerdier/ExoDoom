#include "libos_launch.h"
#include "vmm.h"
#include "page_alloc.h"
#include "string.h"

/*
 * libos_launch.c — build the address space libos_enter() jumps into
 * (SCRUM-47). See libos_launch.h for the design.
 */

int libos_build_image(page_owner_t owner, const void *code, size_t code_len,
                      libos_image_t *out)
{
    if (code_len > VMM_PAGE_SIZE) {
        return VMM_EINVAL;
    }

    uint64_t pml4_phys;
    int rc = vmm_create_address_space(&pml4_phys);
    if (rc != VMM_OK) {
        return rc;
    }

    /* Bound immediately, before anything else that can fail: from here on,
     * vmm_destroy_address_space(owner) is a complete undo of everything this
     * function has done so far, so every later failure just falls through
     * to the same cleanup label instead of needing its own teardown. */
    rc = vmm_bind_address_space(owner, pml4_phys);
    if (rc != VMM_OK) {
        /* Only EINVAL (a bad `owner`) or ENOMEM (registry full) reach here,
         * both caller-side conditions rather than something this call did --
         * there is no bound owner yet for vmm_destroy_address_space() to key
         * on, so the fresh PML4 leaks. Not exercised: v1 has one LibOS id
         * and VMM_MAX_ADDRESS_SPACES headroom for it. */
        return rc;
    }

    void *code_page = alloc_page_owned(owner);
    if (code_page == NULL) {
        vmm_destroy_address_space(owner);
        return VMM_ENOMEM;
    }

    void *stack_page = alloc_page_owned(owner);
    if (stack_page == NULL) {
        free_page_owned(code_page, owner);
        vmm_destroy_address_space(owner);
        return VMM_ENOMEM;
    }

    /* Still running on the kernel's own CR3: `code_page`/`stack_page` are
     * physical addresses the kernel's identity map already covers (they came
     * from the PMM's managed RAM region), so writing through them directly
     * needs no mapping of its own. */
    memcpy(code_page, code, code_len);
    memset(stack_page, 0, VMM_PAGE_SIZE);

    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;

    /* Present + user, deliberately not writable: this page is instructions,
     * copied once above, never written again. */
    rc = vmm_map_page_in(pml4, LIBOS_LAUNCH_CODE_VADDR,
                         (uint64_t)(uintptr_t)code_page,
                         VMM_PRESENT | VMM_USER);
    if (rc != VMM_OK) {
        free_page_owned(code_page, owner);
        free_page_owned(stack_page, owner);
        vmm_destroy_address_space(owner);
        return rc;
    }

    rc = vmm_map_page_in(pml4, LIBOS_LAUNCH_STACK_VADDR,
                         (uint64_t)(uintptr_t)stack_page,
                         VMM_PRESENT | VMM_USER | VMM_WRITE);
    if (rc != VMM_OK) {
        free_page_owned(code_page, owner);
        free_page_owned(stack_page, owner);
        vmm_destroy_address_space(owner);
        return rc;
    }

    out->pml4_phys      = pml4_phys;
    out->entry_vaddr     = LIBOS_LAUNCH_CODE_VADDR;
    out->stack_top_vaddr = LIBOS_LAUNCH_STACK_VADDR + VMM_PAGE_SIZE;
    out->code_paddr       = (uint64_t)(uintptr_t)code_page;
    out->stack_paddr      = (uint64_t)(uintptr_t)stack_page;
    return VMM_OK;
}

void libos_destroy_image(page_owner_t owner, const libos_image_t *img)
{
    free_page_owned((void *)(uintptr_t)img->code_paddr, owner);
    free_page_owned((void *)(uintptr_t)img->stack_paddr, owner);
    vmm_destroy_address_space(owner);
}
