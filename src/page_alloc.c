#include <stdint.h>
#include <stddef.h>
#include "page_alloc.h"
#include "memory.h"
#include "serial.h"
#include "mmap.h"
#include "multiboot2.h"

#define PAGE_SIZE 4096

/* SCRUM-158: every MULTIBOOT_MMAP_AVAILABLE region above 1 MB gets its own
 * bitmap + owner table, rather than the allocator managing only the first
 * such region. mmap.h's MAX_MMAP_REGIONS is 32; usable regions above 1M are
 * always a small subset of the full mmap, so a lower cap here is fine --
 * anything past it is silently skipped with a serial_print, the same
 * failure style as "no usable region found" below. */
#define MAX_PAGE_REGIONS 8

typedef struct {
    uintptr_t     base;
    uint32_t      total_pages;
    uint8_t*      bitmap;
    /* Parallel to `bitmap`, same indexing: owners[i] tags page i (SCRUM-152). */
    page_owner_t* owners;
} page_region_t;

static page_region_t regions[MAX_PAGE_REGIONS];
static uint32_t region_count = 0;
/* Set once page_alloc_init() has finished -- including its own kmalloc calls,
 * which is why this is not simply `region_count != 0`. */
static int pmm_live = 0;


static void bitmap_set(page_region_t* r, uint32_t index) {
    r->bitmap[index / 8] |= (1u << (index % 8));
}

static void bitmap_clear(page_region_t* r, uint32_t index) {
    r->bitmap[index / 8] &= ~(1u << (index % 8));
}

static int bitmap_test(page_region_t* r, uint32_t index) {
    return (r->bitmap[index / 8] >> (index % 8)) & 1u;
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

/* Resolve `addr` to a managed page.  Returns 0 and writes rindex/pindex on
 * a valid page-aligned address inside some managed region; returns non-zero
 * otherwise.  Every region's base is page-aligned (page_alloc_init() calls
 * align_up_uintptr on it) and PAGE_SIZE is a power of two, so
 * `(page - reg->base) % PAGE_SIZE` is equivalent to `page % PAGE_SIZE` for
 * every region -- the alignment check is region-independent and only needs
 * doing once. */
static int region_index_of(void* addr, uint32_t* rindex, uint32_t* pindex) {
    uintptr_t page = (uintptr_t)addr;

    if (page % PAGE_SIZE != 0) {
        return -1;
    }

    for (uint32_t r = 0; r < region_count; r++) {
        page_region_t* reg = &regions[r];
        if (page < reg->base) {
            continue;
        }

        uint32_t i = (uint32_t)((page - reg->base) / PAGE_SIZE);
        if (i < reg->total_pages) {
            *rindex = r;
            *pindex = i;
            return 0;
        }
    }

    return -1;
}

static void reserve_region(uintptr_t start, uintptr_t end) {
    start = align_down_uintptr(start, PAGE_SIZE);
    end   = align_up_uintptr(end, PAGE_SIZE);

    for (uintptr_t addr = start; addr < end; addr += PAGE_SIZE) {
        uint32_t r, i;
        if (region_index_of((void*)addr, &r, &i) == 0) {
            bitmap_set(&regions[r], i);
            regions[r].owners[i] = PAGE_OWNER_KERNEL;
        }
    }
}

void page_alloc_init(const struct mb2_info* mb) {
    if (region_count != 0) {
        return;
    }

    /* Skip kmalloc's bump pointer past every GRUB module before the
     * per-region kmalloc() calls below (bitmap, owner table) touch it. GRUB
     * commonly places the first module immediately after the kernel image --
     * the same address memory_init() starts bump-allocating from -- so
     * without this, those calls can land squarely on top of module bytes
     * nothing has reserved yet. reserve_region() for modules further down is
     * too late to prevent that: it marks pages busy in the PMM's bitmap, it
     * does not undo bytes the bump allocator already overwrote allocating
     * that very bitmap. */
    {
        uintptr_t max_mod_end = 0;
        const struct mb2_tag *tag = mb2_first_tag(mb);
        const uintptr_t tags_end = (uintptr_t)mb + mb->total_size;
        while ((uintptr_t)tag < tags_end && tag->type != MB2_TAG_END) {
            if (tag->type == MB2_TAG_MODULE) {
                const struct mb2_tag_module *m = (const struct mb2_tag_module *)tag;
                if ((uintptr_t)m->mod_end > max_mod_end)
                    max_mod_end = (uintptr_t)m->mod_end;
            }
            tag = mb2_next_tag(tag);
        }
        uintptr_t base = memory_base_address();
        if (max_mod_end > base) {
            kmalloc(max_mod_end - base);
        }
    }

    uint32_t count = 0;
    const mmap_region_t* mmap_regions = mmap_get_regions(&count);

    uint32_t i;
    for (i = 0; i < count && region_count < MAX_PAGE_REGIONS; i++) {
        if (mmap_regions[i].type != MULTIBOOT_MMAP_AVAILABLE ||
            mmap_regions[i].base < 0x100000 ||
            mmap_regions[i].length < PAGE_SIZE) {
            continue;
        }

        uintptr_t base = align_up_uintptr((uintptr_t)mmap_regions[i].base, PAGE_SIZE);
        uintptr_t region_end = (uintptr_t)(mmap_regions[i].base + mmap_regions[i].length);
        if (region_end <= base) {
            /* Rounding the base up to the next page pushed it past this
             * region's own end -- a sub-page sliver too small to host even
             * one page once aligned. Skip it rather than underflow
             * `region_end - base` below. */
            continue;
        }
        uintptr_t usable_len = region_end - base;

        uint32_t pages = (uint32_t)(usable_len / PAGE_SIZE);
        if (pages == 0) {
            continue;
        }

        uint32_t bitmap_bytes = (pages + 7) / 8;
        uint8_t* rbitmap = (uint8_t*)kmalloc(bitmap_bytes);
        for (uint32_t j = 0; j < bitmap_bytes; j++) {
            rbitmap[j] = 0;
        }

        /* Owner table: one tag per page, all FREE to start.  Allocated
         * before reserve_region() so it can stamp KERNEL as it marks
         * reserved pages used.  A region's own bitmap/owner-table backing
         * lives below memory_base_address() only for the first region (the
         * one containing the kernel/bump pool), so it is inside the
         * reserved range and ends up tagged KERNEL -- it is never handed
         * out; later regions' backing is bump-allocated too but from
         * outside their own managed range, since kmalloc draws from the
         * kernel's single bump pool regardless of which region it is
         * building metadata for. */
        page_owner_t* rowners = (page_owner_t*)kmalloc(pages * sizeof(page_owner_t));
        for (uint32_t j = 0; j < pages; j++) {
            rowners[j] = PAGE_OWNER_FREE;
        }

        regions[region_count].base        = base;
        regions[region_count].total_pages = pages;
        regions[region_count].bitmap      = rbitmap;
        regions[region_count].owners      = rowners;
        region_count++;
    }

    if (region_count == MAX_PAGE_REGIONS) {
        for (uint32_t j = i; j < count; j++) {
            if (mmap_regions[j].type == MULTIBOOT_MMAP_AVAILABLE &&
                mmap_regions[j].base >= 0x100000 &&
                mmap_regions[j].length >= PAGE_SIZE) {
                serial_print("page_alloc: MAX_PAGE_REGIONS reached, "
                             "remaining usable regions skipped\n");
                break;
            }
        }
    }

    if (region_count == 0) {
        serial_print("page_alloc: no usable region found\n");
        return;
    }

    serial_print("page_alloc: regions registered\n");

    /* Kernel image + bump pool live at the start of the first managed
     * region. */
    uintptr_t reserve_start = regions[0].base;
    uintptr_t reserve_end   = (uintptr_t)memory_base_address();

    reserve_region(reserve_start, reserve_end);

    serial_print("page_alloc: kernel/heap reserved\n");

    /* The multiboot2 info struct + its tag list is metadata GRUB
     * handed us, not something the kernel's own bump pool claimed --
     * nothing above reserves *its* backing pages. vmm_init() maps it
     * (map_identity) but a map is not a reservation: without this,
     * the very next alloc_page() call (vmm_init()'s own page-table
     * pages, or any syscall/demo allocation afterward) is free to
     * hand out and overwrite the tag list before anything late in
     * boot -- a module lookup, say -- gets a chance to read it. */
    reserve_region((uintptr_t)mb, (uintptr_t)mb + mb->total_size);
    serial_print("page_alloc: multiboot info reserved\n");

    /* This kernel's boot setup loads exactly one GRUB module (the
     * WAD, see grub.cfg) -- mmap_find_module() only reports the
     * first MB2_TAG_MODULE tag, which is why one call is enough
     * here. */
    uint64_t mod_start, mod_end;
    if (mmap_find_module(&mod_start, &mod_end) == 0) {
        reserve_region((uintptr_t)mod_start, (uintptr_t)mod_end);
        serial_print("page_alloc: modules reserved\n");
    }
    serial_print("page_alloc: initialized\n");
    pmm_live = 1;
}

void* alloc_page_owned(page_owner_t owner) {
    if (region_count == 0) {
        serial_print("alloc_page: allocator not initialized\n");
        return NULL;
    }

    for (uint32_t r = 0; r < region_count; r++) {
        page_region_t* reg = &regions[r];
        for (uint32_t i = 0; i < reg->total_pages; i++) {
            if (!bitmap_test(reg, i)) {
                bitmap_set(reg, i);
                /* Masked, so a caller cannot hand out a page that is already
                 * marked for revocation (SCRUM-156). */
                reg->owners[i] = owner_id(owner);
                return (void*)(reg->base + ((uintptr_t)i * PAGE_SIZE));
            }
        }
    }

    serial_print("alloc_page: out of pages\n");
    return NULL;
}

void* alloc_page(void) {
    return alloc_page_owned(PAGE_OWNER_KERNEL);
}

/* Runs are never allowed to span two regions -- two mmap regions are
 * physically separate spans of RAM, so "contiguous" only means anything
 * within one of them. This is a real, user-visible narrowing of what this
 * function can satisfy once more than one region exists: a run of `count`
 * pages that would only fit by straddling a region boundary is reported as
 * unavailable rather than silently handed out as two non-contiguous runs. */
void* alloc_pages_contig_owned(page_owner_t owner, uint32_t count) {
    if (region_count == 0 || count == 0) {
        return NULL;
    }

    for (uint32_t r = 0; r < region_count; r++) {
        page_region_t* reg = &regions[r];

        for (uint32_t start = 0; start + count <= reg->total_pages; start++) {
            if (bitmap_test(reg, start)) {
                continue;
            }

            uint32_t run = 1;
            while (run < count && !bitmap_test(reg, start + run)) {
                run++;
            }

            if (run == count) {
                for (uint32_t i = 0; i < count; i++) {
                    bitmap_set(reg, start + i);
                    reg->owners[start + i] = owner_id(owner);
                }
                return (void*)(reg->base + ((uintptr_t)start * PAGE_SIZE));
            }

            /* Skip past the whole run we just scanned -- nothing inside it can
             * start a shorter free run than the one already ruled out. */
            start += run;
        }
    }

    serial_print("alloc_pages_contig: no contiguous run available\n");
    return NULL;
}

page_owner_t page_owner(void* addr) {
    uint32_t r, i;
    if (region_count == 0 || region_index_of(addr, &r, &i) != 0) {
        return PAGE_OWNER_FREE;
    }
    return owner_id(regions[r].owners[i]);
}

uint32_t reclaim_pages_owned(page_owner_t owner) {
    if (region_count == 0) {
        return 0;
    }

    /* Context reclamation must never reclaim free or kernel-owned pages. */
    if (owner == PAGE_OWNER_FREE || owner == PAGE_OWNER_KERNEL) {
        return 0;
    }

    uint32_t reclaimed = 0;

    for (uint32_t r = 0; r < region_count; r++) {
        page_region_t* reg = &regions[r];
        for (uint32_t i = 0; i < reg->total_pages; i++) {
            if (bitmap_test(reg, i) && reg->owners[i] == owner) {
                bitmap_clear(reg, i);
                reg->owners[i] = PAGE_OWNER_FREE;
                reclaimed++;
            }
        }
    }

    return reclaimed;
}

int free_page_owned(void* addr, page_owner_t owner) {
    if (region_count == 0) {
        serial_print("free_page: allocator not initialized\n");
        return PAGE_FREE_EINVAL;
    }

    uint32_t r, index;
    if (region_index_of(addr, &r, &index) != 0) {
        serial_print("free_page: invalid page address\n");
        return PAGE_FREE_EINVAL;
    }
    page_region_t* reg = &regions[r];

    if (!bitmap_test(reg, index)) {
        serial_print("free_page: double free detected\n");
        return PAGE_FREE_EINVAL;
    }

    /* Bit is set (page is allocated): the tag decides whether this caller may
     * free it.  A mismatch is a LibOS reaching for kernel or peer memory.
     * Compared by id: a page marked for revocation (SCRUM-156) is still the
     * caller's, and returning it is precisely the compliance the mark asks
     * for — the reset to PAGE_OWNER_FREE below clears owner and mark at once. */
    if (owner_id(reg->owners[index]) != owner_id(owner)) {
        serial_print("free_page: ownership violation\n");
        return PAGE_FREE_EPERM;
    }

    bitmap_clear(reg, index);
    reg->owners[index] = PAGE_OWNER_FREE;
    return PAGE_FREE_OK;
}

int free_page_checked(void* addr) {
    /* Kernel-internal free: pages taken by alloc_page() are KERNEL-owned. */
    return free_page_owned(addr, PAGE_OWNER_KERNEL) == PAGE_FREE_OK ? 0 : -1;
}

void free_page(void* addr) {
    (void)free_page_checked(addr);
}

int page_alloc_is_live(void) {
    return pmm_live;
}

uint32_t page_alloc_region_count(void) {
    return region_count;
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
static int owned_page_index(void* addr, page_owner_t owner, uint32_t* rindex, uint32_t* pindex) {
    uint32_t r, i;

    if (region_count == 0 || region_index_of(addr, &r, &i) != 0) {
        return PAGE_REVOKE_EINVAL;
    }

    /* Neither sentinel names a revocable context, and both are refused before
     * the ownership compare rather than after it — each would otherwise *pass*
     * that compare against real pages.  PAGE_OWNER_FREE matches the tag of
     * every free page; PAGE_OWNER_KERNEL matches every reserved one, so
     * page_reclaim(kp, PAGE_OWNER_KERNEL) would hand the page bitmap, the owner
     * table or the kernel image back to the pool and leave the kernel's own
     * free_page() to double-free it.  page_reclaim_all() refuses the same two
     * ids; the single-resource path must not be the weaker of the pair. */
    if (owner_id(owner) == PAGE_OWNER_FREE ||
        owner_id(owner) == PAGE_OWNER_KERNEL) {
        return PAGE_REVOKE_ENOENT;
    }

    page_region_t* reg = &regions[r];

    /* Free, or allocated to a different context: `owner` does not hold it. */
    if (!bitmap_test(reg, i) || owner_id(reg->owners[i]) != owner_id(owner)) {
        return PAGE_REVOKE_ENOENT;
    }

    *rindex = r;
    *pindex = i;
    return PAGE_REVOKE_OK;
}

int page_revoke_mark(void* addr, page_owner_t owner) {
    uint32_t r, index;
    int rc = owned_page_index(addr, owner, &r, &index);

    if (rc != PAGE_REVOKE_OK) {
        return rc;
    }

    regions[r].owners[index] |= PAGE_OWNER_REVOKED;
    return PAGE_REVOKE_OK;
}

int page_revoke_clear(void* addr, page_owner_t owner) {
    uint32_t r, index;
    int rc = owned_page_index(addr, owner, &r, &index);

    if (rc != PAGE_REVOKE_OK) {
        return rc;
    }

    regions[r].owners[index] = owner_id(regions[r].owners[index]);
    return PAGE_REVOKE_OK;
}

int page_revoke_pending(void* addr) {
    uint32_t r, index;

    if (region_count == 0 || region_index_of(addr, &r, &index) != 0) {
        return 0;
    }

    /* A free page's tag is PAGE_OWNER_FREE, mark included, so this answers 0
     * for it without a separate allocated check. */
    return (regions[r].owners[index] & PAGE_OWNER_REVOKED) != 0;
}

int page_reclaim(void* addr, page_owner_t owner) {
    uint32_t r, index;
    int rc = owned_page_index(addr, owner, &r, &index);

    if (rc != PAGE_REVOKE_OK) {
        /* ENOENT here is the LibOS having complied (or the page having moved
         * on to another context).  Either way there is nothing to take, and
         * taking it anyway would free a page somebody else legitimately
         * holds. */
        return rc;
    }

    bitmap_clear(&regions[r], index);
    regions[r].owners[index] = PAGE_OWNER_FREE;   /* clears owner and mark in one store */
    return PAGE_REVOKE_OK;
}

uint32_t page_reclaim_all(page_owner_t owner) {
    if (region_count == 0) {
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

    for (uint32_t r = 0; r < region_count; r++) {
        page_region_t* reg = &regions[r];
        for (uint32_t i = 0; i < reg->total_pages; i++) {
            if (bitmap_test(reg, i) && owner_id(reg->owners[i]) == owner_id(owner)) {
                bitmap_clear(reg, i);
                reg->owners[i] = PAGE_OWNER_FREE;
                reclaimed++;
            }
        }
    }

    return reclaimed;
}

uint32_t page_count_owned(page_owner_t owner) {
    if (region_count == 0 || owner_id(owner) == PAGE_OWNER_FREE) {
        return 0;
    }

    uint32_t count = 0;

    for (uint32_t r = 0; r < region_count; r++) {
        page_region_t* reg = &regions[r];
        for (uint32_t i = 0; i < reg->total_pages; i++) {
            if (bitmap_test(reg, i) && owner_id(reg->owners[i]) == owner_id(owner)) {
                count++;
            }
        }
    }

    return count;
}

uintptr_t page_alloc_pool_end(void) {
    if (region_count == 0) {
        return 0;
    }
    page_region_t* last = &regions[region_count - 1];
    return last->base + (uintptr_t)last->total_pages * PAGE_SIZE;
}
