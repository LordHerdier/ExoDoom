#include "libos_launch.h"
#include "vmm.h"
#include "page_alloc.h"
#include "string.h"

/*
 * libos_launch.c — build the address space libos_enter() jumps into
 * (SCRUM-47/-49). See libos_launch.h for the design.
 */

/* Free every page in paddrs[0..count) back to the PMM. Shared by
 * build_fail() below and by libos_destroy_image(). */
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
 * `paddrs` must point at a caller-owned array with room for every page this
 * call maps; `*count_out` is set to how many were actually used.
 *
 * Precondition, enforced by the caller (libos_build_image), not here:
 * `copy_len + zero_len` fits within `paddrs`'s capacity. This function
 * trusts that rather than re-checking it, since libos_build_image already
 * rejects an oversized request before either of its two calls into this one.
 *
 * `src` may be NULL only when `copy_len` is 0 -- enforced below, since a
 * caller passing a NULL `src` with a nonzero `copy_len` would otherwise reach
 * memcpy() with a NULL source. `copy_len + zero_len == 0` is a valid
 * "nothing to load here" call: `*count_out` comes back 0 and nothing is
 * allocated or mapped.
 *
 * On failure, frees whatever this call itself already allocated/mapped --
 * the caller only has to unwind sections built before this one, the same
 * per-step cleanup shape libos_build_image() already used.
 */
static int load_region(page_owner_t owner, uint64_t *pml4, uint64_t vaddr,
                       uint64_t flags, const void *src, size_t copy_len,
                       size_t zero_len, uint64_t *paddrs, uint32_t *count_out)
{
    *count_out = 0;

    if (src == NULL && copy_len != 0) {
        return VMM_EINVAL;
    }

    size_t total = copy_len + zero_len;
    uint32_t pages = (uint32_t)((total + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE);

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

/* Undo everything libos_build_image() may have built past a successful
 * vmm_bind_address_space(): the stack page (if `stack_page` is non-NULL --
 * some failure paths run before it is allocated), whatever code/data pages
 * `out->code_pages`/`out->data_pages` already record, and the address space
 * itself. Centralising this is what keeps the four failure sites below in
 * sync as regions are added -- each one differs only in whether a stack page
 * exists yet and what status code it reports. */
static int build_fail(page_owner_t owner, libos_image_t *out,
                      void *stack_page, int rc)
{
    if (stack_page != NULL) {
        free_page_owned(stack_page, owner);
    }
    free_pages(out->code_paddrs, out->code_pages, owner);
    free_pages(out->data_paddrs, out->data_pages, owner);
    vmm_destroy_address_space(owner);
    return rc;
}

int libos_build_image(page_owner_t owner,
                      const void *code, size_t code_len,
                      const void *data, size_t data_len, size_t bss_len,
                      libos_image_t *out)
{
    /* code_len == 0 is rejected, not just oversized code: entry_vaddr would
     * otherwise point at a page this call never mapped, and libos_enter()
     * would iretq straight into an unmapped page. */
    if (code_len == 0 ||
        code_len > LIBOS_LAUNCH_MAX_CODE_PAGES * VMM_PAGE_SIZE) {
        return VMM_EINVAL;
    }
    /* Each term is checked individually against the same bound before the
     * sum is: `data_len + bss_len` alone could wrap size_t and slip under
     * the combined check on its own, but two operands already known to be
     * <= LIBOS_LAUNCH_MAX_DATA_PAGES * VMM_PAGE_SIZE (a few KiB) can never
     * overflow a 64-bit size_t when added together. */
    if (data_len > LIBOS_LAUNCH_MAX_DATA_PAGES * VMM_PAGE_SIZE ||
        bss_len  > LIBOS_LAUNCH_MAX_DATA_PAGES * VMM_PAGE_SIZE ||
        data_len + bss_len > LIBOS_LAUNCH_MAX_DATA_PAGES * VMM_PAGE_SIZE) {
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
     * to build_fail() instead of needing its own teardown. */
    rc = vmm_bind_address_space(owner, pml4_phys);
    if (rc != VMM_OK) {
        /* Only EINVAL (a bad `owner`) or ENOMEM (registry full) reach here,
         * both caller-side conditions rather than something this call did --
         * there is no bound owner yet for vmm_destroy_address_space() to key
         * on, so the fresh PML4 leaks. Not exercised: v1 has one LibOS id
         * and VMM_MAX_ADDRESS_SPACES headroom for it. */
        return rc;
    }

    /* Zeroed before anything that can fail below reads them -- build_fail()
     * relies on both being valid counts from this point on. */
    out->code_pages = 0;
    out->data_pages = 0;

    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;

    /* Present + user, deliberately not writable: this is instructions,
     * copied once here, never written again. */
    rc = load_region(owner, pml4, LIBOS_LAUNCH_CODE_VADDR,
                     VMM_PRESENT | VMM_USER,
                     code, code_len, 0,
                     out->code_paddrs, &out->code_pages);
    if (rc != VMM_OK) {
        return build_fail(owner, out, NULL, rc);
    }

    /* Present + user + writable: `.data` (copied) followed by `.bss`
     * (zeroed) in one contiguous mapped range. */
    rc = load_region(owner, pml4, LIBOS_LAUNCH_DATA_VADDR,
                     VMM_PRESENT | VMM_USER | VMM_WRITE,
                     data, data_len, bss_len,
                     out->data_paddrs, &out->data_pages);
    if (rc != VMM_OK) {
        return build_fail(owner, out, NULL, rc);
    }

    void *stack_page = alloc_page_owned(owner);
    if (stack_page == NULL) {
        return build_fail(owner, out, NULL, VMM_ENOMEM);
    }
    memset(stack_page, 0, VMM_PAGE_SIZE);

    rc = vmm_map_page_in(pml4, LIBOS_LAUNCH_STACK_VADDR,
                         (uint64_t)(uintptr_t)stack_page,
                         VMM_PRESENT | VMM_USER | VMM_WRITE);
    if (rc != VMM_OK) {
        return build_fail(owner, out, stack_page, rc);
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
