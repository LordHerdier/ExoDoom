#include "libos_wad_map.h"
#include "vmm.h"

#include <stdint.h>

int libos_map_wad(uint64_t *pml4, uint64_t wad_paddr, uint64_t wad_size)
{
    if (wad_size == 0 || wad_size > LIBOS_WAD_MAX_BYTES)
        return VMM_EINVAL;

    uint64_t pages = (wad_size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t vaddr = LIBOS_WAD_VADDR + i * VMM_PAGE_SIZE;
        uint64_t paddr = wad_paddr + i * VMM_PAGE_SIZE;

        /* Present + user, deliberately not writable -- see libos_wad_map.h. */
        int rc = vmm_map_page_in(pml4, vaddr, paddr, VMM_PRESENT | VMM_USER);
        if (rc != VMM_OK) {
            for (uint64_t j = 0; j < i; j++)
                vmm_unmap_page_in(pml4, LIBOS_WAD_VADDR + j * VMM_PAGE_SIZE);
            return rc;
        }
    }

    return VMM_OK;
}
