#include "libos_launch.h"
#include "vmm.h"
#include "page_alloc.h"
#include "string.h"

/*
 * libos_launch.c — build the address space libos_enter() jumps into
 * (SCRUM-47/-49). See libos_launch.h for the design.
 */

/* Free every page in paddrs[0..count) back to the PMM. Shared by the
 * mid-build failure paths below and by libos_destroy_image(). */
static void free_pages(const uint64_t *paddrs, uint32_t count, page_owner_t owner)
{
    for (uint32_t i = 0; i < count; i++) {
        free_page_owned((void *)(uintptr_t)paddrs[i], owner);
    }
}

/*
 * Allocate enough pages to cover `copy_len + zero_len` bytes, copy `copy_len`
 * bytes of `src` into the front of them and zero the rest (the `.bss` tail),
 * then map them contiguously starting at `vaddr` with `flags` in `pml4`.
 * `paddrs` must point at a caller-owned array of at least `max_pages`
 * entries; `*count_out` is set to how many were actually used.
 *
 * `src` may be NULL only when `copy_len` is 0. `copy_len + zero_len == 0` is
 * a valid "nothing to load here" call: `*count_out` comes back 0 and nothing
 * is allocated or mapped.
 *
 * On failure, frees whatever this call itself already allocated/mapped --
 * the caller only has to unwind sections built before this one, the same
 * per-step cleanup shape libos_build_image() already used.
 */
static int load_region(page_owner_t owner, uint64_t *pml4, uint64_t vaddr,
                       uint64_t flags, const void *src, size_t copy_len,
                       size_t zero_len, uint64_t *paddrs, uint32_t max_pages,
                       uint32_t *count_out)
{
    size_t total = copy_len + zero_len;
    uint32_t pages = (uint32_t)((total + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE);

    *count_out = 0;
    if (pages > max_pages) {
        return VMM_EINVAL;
    }

    const unsigned char *src_bytes = (const unsigned char *)src;
    size_t remaining_copy = copy_len;

    for (uint32_t i = 0; i < pages; i++) {
        void *page = alloc_page_owned(owner);
        if (page == NULL) {
            free_pages(paddrs, *count_out, owner);
            *count_out = 0;
            return VMM_ENOMEM;
        }
        paddrs[i] = (uint64_t)(uintptr_t)page;
        (*count_out)++;

        size_t this_copy = remaining_copy < VMM_PAGE_SIZE ?
                           remaining_copy : (size_t)VMM_PAGE_SIZE;
        if (this_copy > 0) {
            memcpy(page, src_bytes, this_copy);
        }
        if (this_copy < VMM_PAGE_SIZE) {
            memset((unsigned char *)page + this_copy, 0,
                  (size_t)VMM_PAGE_SIZE - this_copy);
        }
        src_bytes += this_copy;
        remaining_copy -= this_copy;

        int rc = vmm_map_page_in(pml4, vaddr + (uint64_t)i * VMM_PAGE_SIZE,
                                 paddrs[i], flags);
        if (rc != VMM_OK) {
            free_pages(paddrs, *count_out, owner);
            *count_out = 0;
            return rc;
        }
    }

    return VMM_OK;
}

int libos_build_image(page_owner_t owner,
                      const void *code, size_t code_len,
                      const void *data, size_t data_len, size_t bss_len,
                      libos_image_t *out)
{
    if (code_len > LIBOS_LAUNCH_MAX_CODE_PAGES * VMM_PAGE_SIZE) {
        return VMM_EINVAL;
    }
    if (data_len + bss_len > LIBOS_LAUNCH_MAX_DATA_PAGES * VMM_PAGE_SIZE) {
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
     * to the same cleanup instead of needing its own teardown. */
    rc = vmm_bind_address_space(owner, pml4_phys);
    if (rc != VMM_OK) {
        /* Only EINVAL (a bad `owner`) or ENOMEM (registry full) reach here,
         * both caller-side conditions rather than something this call did --
         * there is no bound owner yet for vmm_destroy_address_space() to key
         * on, so the fresh PML4 leaks. Not exercised: v1 has one LibOS id
         * and VMM_MAX_ADDRESS_SPACES headroom for it. */
        return rc;
    }

    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;

    /* Present + user, deliberately not writable: this is instructions,
     * copied once here, never written again. */
    rc = load_region(owner, pml4, LIBOS_LAUNCH_CODE_VADDR,
                     VMM_PRESENT | VMM_USER,
                     code, code_len, 0,
                     out->code_paddrs, LIBOS_LAUNCH_MAX_CODE_PAGES,
                     &out->code_pages);
    if (rc != VMM_OK) {
        vmm_destroy_address_space(owner);
        return rc;
    }

    /* Present + user + writable: `.data` (copied) followed by `.bss`
     * (zeroed) in one contiguous mapped range. */
    rc = load_region(owner, pml4, LIBOS_LAUNCH_DATA_VADDR,
                     VMM_PRESENT | VMM_USER | VMM_WRITE,
                     data, data_len, bss_len,
                     out->data_paddrs, LIBOS_LAUNCH_MAX_DATA_PAGES,
                     &out->data_pages);
    if (rc != VMM_OK) {
        free_pages(out->code_paddrs, out->code_pages, owner);
        vmm_destroy_address_space(owner);
        return rc;
    }

    void *stack_page = alloc_page_owned(owner);
    if (stack_page == NULL) {
        free_pages(out->code_paddrs, out->code_pages, owner);
        free_pages(out->data_paddrs, out->data_pages, owner);
        vmm_destroy_address_space(owner);
        return VMM_ENOMEM;
    }
    memset(stack_page, 0, VMM_PAGE_SIZE);

    rc = vmm_map_page_in(pml4, LIBOS_LAUNCH_STACK_VADDR,
                         (uint64_t)(uintptr_t)stack_page,
                         VMM_PRESENT | VMM_USER | VMM_WRITE);
    if (rc != VMM_OK) {
        free_page_owned(stack_page, owner);
        free_pages(out->code_paddrs, out->code_pages, owner);
        free_pages(out->data_paddrs, out->data_pages, owner);
        vmm_destroy_address_space(owner);
        return rc;
    }

    out->pml4_phys       = pml4_phys;
    out->entry_vaddr      = LIBOS_LAUNCH_CODE_VADDR;
    out->stack_top_vaddr  = LIBOS_LAUNCH_STACK_VADDR + VMM_PAGE_SIZE;
    out->stack_paddr      = (uint64_t)(uintptr_t)stack_page;
    return VMM_OK;
}

void libos_destroy_image(page_owner_t owner, const libos_image_t *img)
{
    free_pages(img->code_paddrs, img->code_pages, owner);
    free_pages(img->data_paddrs, img->data_pages, owner);
    free_page_owned((void *)(uintptr_t)img->stack_paddr, owner);
    vmm_destroy_address_space(owner);
}
