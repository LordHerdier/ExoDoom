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
 * vmm_bind_address_space(): whatever code, data and stack pages
 * `out->code_pages`/`out->data_pages`/`out->stack_pages` already record, and
 * the address space itself. Centralising this is what keeps the failure
 * sites below in sync as regions are added -- each one now differs only in
 * the status code it reports.
 *
 * The stack used to be passed in as a bare `void *stack_page`, because it
 * was exactly one page allocated inline rather than a region (SCRUM-66 made
 * it LIBOS_LAUNCH_STACK_PAGES of them, built through the same load_region()
 * as the other two). A count of zero makes free_pages() a no-op, so the
 * paths that fail before the stack exists need no special case for it. */
static int build_fail(page_owner_t owner, libos_image_t *out, int rc)
{
    free_pages(out->code_paddrs, out->code_pages, owner);
    free_pages(out->data_paddrs, out->data_pages, owner);
    free_pages(out->stack_paddrs, out->stack_pages, owner);
    vmm_destroy_address_space(owner);
    return rc;
}

int libos_build_image(page_owner_t owner,
                      const void *code, size_t code_len,
                      const void *data, size_t data_len, size_t bss_len,
                      libos_image_t *out)
{
    const size_t max_code_bytes = (size_t)LIBOS_LAUNCH_MAX_CODE_PAGES * VMM_PAGE_SIZE;
    const size_t max_data_bytes = (size_t)LIBOS_LAUNCH_MAX_DATA_PAGES * VMM_PAGE_SIZE;

    /* code_len == 0 is rejected, not just oversized code: entry_vaddr would
     * otherwise point at a page this call never mapped, and libos_enter()
     * would iretq straight into an unmapped page. */
    if (code_len == 0 || code_len > max_code_bytes) {
        return VMM_EINVAL;
    }
    /* Each term is checked individually against the same bound before the
     * sum is: `data_len + bss_len` alone could wrap size_t and slip under
     * the combined check on its own, but two operands already known to be
     * <= max_data_bytes (a few KiB) can never overflow a 64-bit size_t when
     * added together. */
    if (data_len > max_data_bytes ||
        bss_len  > max_data_bytes ||
        data_len + bss_len > max_data_bytes) {
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
     * relies on all three being valid counts from this point on. */
    out->code_pages  = 0;
    out->data_pages  = 0;
    out->stack_pages = 0;

    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;

    /* Present + user, deliberately not writable: this is instructions,
     * copied once here, never written again. */
    rc = load_region(owner, pml4, LIBOS_LAUNCH_CODE_VADDR,
                     VMM_PRESENT | VMM_USER,
                     code, code_len, 0,
                     out->code_paddrs, &out->code_pages);
    if (rc != VMM_OK) {
        return build_fail(owner, out, rc);
    }

    /* Present + user + writable: `.data` (copied) followed by `.bss`
     * (zeroed) in one contiguous mapped range. */
    rc = load_region(owner, pml4, LIBOS_LAUNCH_DATA_VADDR,
                     VMM_PRESENT | VMM_USER | VMM_WRITE,
                     data, data_len, bss_len,
                     out->data_paddrs, &out->data_pages);
    if (rc != VMM_OK) {
        return build_fail(owner, out, rc);
    }

    /* The stack is LIBOS_LAUNCH_STACK_PAGES of zero-filled, writable,
     * user-accessible pages (SCRUM-66 -- it was a single page before Doom
     * needed a real one). Built through the same load_region() as code and
     * data rather than an inline alloc/map: passing src=NULL with
     * copy_len=0 and zero_len=the whole span is exactly the .bss case that
     * function already implements, down to zeroing each page and unwinding
     * its own partial work if a later page fails to map. */
    rc = load_region(owner, pml4, LIBOS_LAUNCH_STACK_VADDR,
                     VMM_PRESENT | VMM_USER | VMM_WRITE,
                     NULL, 0,
                     (size_t)LIBOS_LAUNCH_STACK_PAGES * VMM_PAGE_SIZE,
                     out->stack_paddrs, &out->stack_pages);
    if (rc != VMM_OK) {
        return build_fail(owner, out, rc);
    }

    out->pml4_phys        = pml4_phys;
    out->entry_vaddr      = LIBOS_LAUNCH_CODE_VADDR;
    /* Eight bytes below the top of the highest stack page, not at it: the
     * SysV ABI's "RSP == 8 (mod 16) on entry" contract, which an iretq entry
     * has to honour by hand since it pushes no return address. See
     * LIBOS_LAUNCH_STACK_TOP's own comment (src/libos_launch.h) -- getting
     * this wrong costs an aligned-SSE #GP in the first ring-3 target that
     * enables SSE, and nothing at all in the ones that do not. The stack
     * grows down from here toward the guard page below
     * LIBOS_LAUNCH_STACK_VADDR. */
    out->stack_top_vaddr  = LIBOS_LAUNCH_STACK_TOP;
    return VMM_OK;
}

int libos_launch_patch_params(const libos_image_t *img,
                              const void *params, size_t len)
{
    if (img->data_pages == 0 || len > VMM_PAGE_SIZE) {
        return VMM_EINVAL;
    }

    memcpy((void *)(uintptr_t)img->data_paddrs[0], params, len);
    return VMM_OK;
}

void libos_destroy_image(page_owner_t owner, const libos_image_t *img)
{
    free_pages(img->code_paddrs, img->code_pages, owner);
    free_pages(img->data_paddrs, img->data_pages, owner);
    free_pages(img->stack_paddrs, img->stack_pages, owner);
    vmm_destroy_address_space(owner);
}
