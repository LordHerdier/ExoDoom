#include <stdint.h>
#include <stddef.h>
#include "page_alloc.h"
#include "memory.h"
#include "serial.h"
#include "mmap.h"
#include "multiboot2.h"

#define PAGE_SIZE 4096

static uintptr_t managed_base = 0;
static uint32_t total_pages = 0;
static uint8_t* bitmap = NULL;
/* Parallel to `bitmap`, same indexing: owners[i] tags page i (SCRUM-152). */
static page_owner_t* owners = NULL;


static void bitmap_set(uint32_t index) {
    bitmap[index / 8] |= (1u << (index % 8));
}

static void bitmap_clear(uint32_t index) {
    bitmap[index / 8] &= ~(1u << (index % 8));
}

static int bitmap_test(uint32_t index) {
    return (bitmap[index / 8] >> (index % 8)) & 1u;
}

/* A tag's owner id, with the SCRUM-156 revocation mark stripped off.  Every
 * ownership comparison in this file goes through it: a marked page still
 * belongs to its owner, so a raw tag compare would read a page under
 * revocation as owned by nobody the caller can name. */
static page_owner_t owner_id(page_owner_t tag) {
    return tag & PAGE_OWNER_ID_MASK;
}

static uintptr_t align_up_uintptr(uintptr_t value, uintptr_t align) {
    return (value + align - 1) & ~(align - 1);
}

static uintptr_t align_down_uintptr(uintptr_t value, uintptr_t align) {
    return value & ~(align - 1);
}

static void reserve_region(uintptr_t start, uintptr_t end) {
    start = align_down_uintptr(start, PAGE_SIZE);
    end   = align_up_uintptr(end, PAGE_SIZE);

    for (uintptr_t addr = start; addr < end; addr += PAGE_SIZE) {
        if (addr < managed_base) continue;

        uint32_t index = (uint32_t)((addr - managed_base) / PAGE_SIZE);
        if (index < total_pages) {
            bitmap_set(index);
            owners[index] = PAGE_OWNER_KERNEL;
        }
    }
}

void page_alloc_init(const struct mb2_info* mb) {
    if (bitmap != NULL) {
        return;
    }

    uint32_t count = 0;
    const mmap_region_t* regions = mmap_get_regions(&count);

    for (uint32_t i = 0; i < count; i++) {
        if (regions[i].type == MULTIBOOT_MMAP_AVAILABLE &&
            regions[i].base >= 0x100000 &&
            regions[i].length >= PAGE_SIZE) {

            managed_base = align_up_uintptr((uintptr_t)regions[i].base, PAGE_SIZE);
            uintptr_t region_end = (uintptr_t)(regions[i].base + regions[i].length);
            uintptr_t usable_len = region_end - managed_base;

            total_pages = (uint32_t)(usable_len / PAGE_SIZE);

            uint32_t bitmap_bytes = (total_pages + 7) / 8;
            bitmap = (uint8_t*)kmalloc(bitmap_bytes);

            for (uint32_t j = 0; j < bitmap_bytes; j++) {
                bitmap[j] = 0;
            }

            /* Owner table: one tag per page, all FREE to start.  Allocated
             * before reserve_region() so it can stamp KERNEL as it marks
             * reserved pages used.  Its own backing lives below
             * memory_base_address(), so it is inside the reserved range and
             * ends up tagged KERNEL — it is never handed out. */
            owners = (page_owner_t*)kmalloc(total_pages * sizeof(page_owner_t));
            for (uint32_t j = 0; j < total_pages; j++) {
                owners[j] = PAGE_OWNER_FREE;
            }

            uintptr_t reserve_start = managed_base;
            uintptr_t reserve_end   = (uintptr_t)memory_base_address();

            reserve_region(reserve_start, reserve_end);

            serial_print("page_alloc: kernel/heap reserved\n");

            const struct mb2_tag *tag = mb2_first_tag(mb);
            const uintptr_t tags_end = (uintptr_t)mb + mb->total_size;
            unsigned mods = 0;

            while ((uintptr_t)tag < tags_end && tag->type != MB2_TAG_END) {
                if (tag->type == MB2_TAG_MODULE) {
                    const struct mb2_tag_module *m =
                        (const struct mb2_tag_module *)tag;

                    reserve_region((uintptr_t)m->mod_start,
                                   (uintptr_t)m->mod_end);
                    mods++;
                }

                tag = mb2_next_tag(tag);
            }

            if (mods > 0) {
                serial_print("page_alloc: modules reserved\n");
            }
            serial_print("page_alloc: initialized\n");
            return;
        }
    }

    serial_print("page_alloc: no usable region found\n");
}

void* alloc_page_owned(page_owner_t owner) {
    if (bitmap == NULL || total_pages == 0) {
        serial_print("alloc_page: allocator not initialized\n");
        return NULL;
    }

    for (uint32_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            /* Masked, so a caller cannot hand out a page that is already
             * marked for revocation (SCRUM-156). */
            owners[i] = owner_id(owner);
            return (void*)(managed_base + ((uintptr_t)i * PAGE_SIZE));
        }
    }

    serial_print("alloc_page: out of pages\n");
    return NULL;
}

void* alloc_page(void) {
    return alloc_page_owned(PAGE_OWNER_KERNEL);
}

// Resolve `addr` to a managed page index.  Returns 0 and writes *index on a
// valid page-aligned in-range address; returns non-zero otherwise.
static int page_index_of(void* addr, uint32_t* index) {
    uintptr_t page = (uintptr_t)addr;

    if (page < managed_base || ((page - managed_base) % PAGE_SIZE) != 0) {
        return -1;
    }

    uint32_t i = (uint32_t)((page - managed_base) / PAGE_SIZE);
    if (i >= total_pages) {
        return -1;
    }

    *index = i;
    return 0;
}

page_owner_t page_owner(void* addr) {
    uint32_t index;
    if (bitmap == NULL || total_pages == 0 || page_index_of(addr, &index) != 0) {
        return PAGE_OWNER_FREE;
    }
    return owner_id(owners[index]);
}

int free_page_owned(void* addr, page_owner_t owner) {
    if (bitmap == NULL || total_pages == 0) {
        serial_print("free_page: allocator not initialized\n");
        return PAGE_FREE_EINVAL;
    }

    uint32_t index;
    if (page_index_of(addr, &index) != 0) {
        serial_print("free_page: invalid page address\n");
        return PAGE_FREE_EINVAL;
    }

    if (!bitmap_test(index)) {
        serial_print("free_page: double free detected\n");
        return PAGE_FREE_EINVAL;
    }

    /* Bit is set (page is allocated): the tag decides whether this caller may
     * free it.  A mismatch is a LibOS reaching for kernel or peer memory.
     * Compared by id: a page marked for revocation (SCRUM-156) is still the
     * caller's, and returning it is precisely the compliance the mark asks
     * for — the reset to PAGE_OWNER_FREE below clears owner and mark at once. */
    if (owner_id(owners[index]) != owner_id(owner)) {
        serial_print("free_page: ownership violation\n");
        return PAGE_FREE_EPERM;
    }

    bitmap_clear(index);
    owners[index] = PAGE_OWNER_FREE;
    return PAGE_FREE_OK;
}

int free_page_checked(void* addr) {
    /* Kernel-internal free: pages taken by alloc_page() are KERNEL-owned. */
    return free_page_owned(addr, PAGE_OWNER_KERNEL) == PAGE_FREE_OK ? 0 : -1;
}

void free_page(void* addr) {
    (void)free_page_checked(addr);
}

/* ---- Revocation / repossession (SCRUM-156) -------------------------------
 *
 * The ownership-table half of the protocol in docs/syscall_spec.md §3.6.
 * src/revoke.c sequences these; this file only knows how to mark a page and
 * how to take it back.
 */

/* Resolve `addr` to a managed page that `owner` currently holds.  All four
 * entry points below need exactly this test and must agree on it: a reclaim
 * that applied a looser rule than the mark would take a page the request never
 * covered. */
static int owned_page_index(void* addr, page_owner_t owner, uint32_t* index) {
    uint32_t i;

    if (bitmap == NULL || total_pages == 0 || page_index_of(addr, &i) != 0) {
        return PAGE_REVOKE_EINVAL;
    }

    /* Free, or allocated to a different context: `owner` does not hold it.
     * PAGE_OWNER_FREE is refused explicitly — it is the "nobody" sentinel, and
     * without this it would match the tag of every free page. */
    if (!bitmap_test(i) || owner_id(owner) == PAGE_OWNER_FREE ||
        owner_id(owners[i]) != owner_id(owner)) {
        return PAGE_REVOKE_ENOENT;
    }

    *index = i;
    return PAGE_REVOKE_OK;
}

int page_revoke_mark(void* addr, page_owner_t owner) {
    uint32_t index;
    int rc = owned_page_index(addr, owner, &index);

    if (rc != PAGE_REVOKE_OK) {
        return rc;
    }

    owners[index] |= PAGE_OWNER_REVOKED;
    return PAGE_REVOKE_OK;
}

int page_revoke_clear(void* addr, page_owner_t owner) {
    uint32_t index;
    int rc = owned_page_index(addr, owner, &index);

    if (rc != PAGE_REVOKE_OK) {
        return rc;
    }

    owners[index] = owner_id(owners[index]);
    return PAGE_REVOKE_OK;
}

int page_revoke_pending(void* addr) {
    uint32_t index;

    if (bitmap == NULL || total_pages == 0 || page_index_of(addr, &index) != 0) {
        return 0;
    }

    /* A free page's tag is PAGE_OWNER_FREE, mark included, so this answers 0
     * for it without a separate allocated check. */
    return (owners[index] & PAGE_OWNER_REVOKED) != 0;
}

int page_reclaim(void* addr, page_owner_t owner) {
    uint32_t index;
    int rc = owned_page_index(addr, owner, &index);

    if (rc != PAGE_REVOKE_OK) {
        /* ENOENT here is the LibOS having complied (or the page having moved
         * on to another context).  Either way there is nothing to take, and
         * taking it anyway would free a page somebody else legitimately
         * holds. */
        return rc;
    }

    bitmap_clear(index);
    owners[index] = PAGE_OWNER_FREE;   /* clears owner and mark in one store */
    return PAGE_REVOKE_OK;
}

uint32_t page_reclaim_all(page_owner_t owner) {
    if (bitmap == NULL || total_pages == 0) {
        return 0;
    }

    /* Neither sentinel names a revocable context.  KERNEL matters most: a
     * sweep of it would free the bitmap, the owner table, the kernel image and
     * the WAD module out from under the running system. */
    if (owner_id(owner) == PAGE_OWNER_FREE ||
        owner_id(owner) == PAGE_OWNER_KERNEL) {
        serial_print("revoke: refusing to sweep a reserved owner id\n");
        return 0;
    }

    uint32_t reclaimed = 0;

    for (uint32_t i = 0; i < total_pages; i++) {
        if (bitmap_test(i) && owner_id(owners[i]) == owner_id(owner)) {
            bitmap_clear(i);
            owners[i] = PAGE_OWNER_FREE;
            reclaimed++;
        }
    }

    return reclaimed;
}

uint32_t page_count_owned(page_owner_t owner) {
    if (bitmap == NULL || total_pages == 0 ||
        owner_id(owner) == PAGE_OWNER_FREE) {
        return 0;
    }

    uint32_t count = 0;

    for (uint32_t i = 0; i < total_pages; i++) {
        if (bitmap_test(i) && owner_id(owners[i]) == owner_id(owner)) {
            count++;
        }
    }

    return count;
}

uintptr_t page_alloc_pool_end(void) {
    if (bitmap == NULL) {
        return 0;
    }
    return managed_base + (uintptr_t)total_pages * PAGE_SIZE;
}
