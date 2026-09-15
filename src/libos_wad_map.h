#ifndef LIBOS_WAD_MAP_H
#define LIBOS_WAD_MAP_H

#include <stdint.h>

/*
 * libos_wad_map — kernel-side, read-only WAD exposure for a launched LibOS.
 *
 * There is no exo_wad_acquire syscall (v1 has no filesystem, and the WAD
 * arrives as a multiboot module rather than something a LibOS opens): the
 * kernel identity-maps the WAD module into its own map at vmm_init() and
 * page_alloc_init() reserves its pages as PAGE_OWNER_KERNEL
 * (src/mmap.c's mmap_find_module(), src/page_alloc.c), so exo_page_map's
 * ownership check (may_map_phys(), src/syscall_mem.c) would refuse a LibOS
 * that tried to map it itself the same way it refuses any other page it does
 * not own.
 *
 * Rather than inventing a second binding table just for one read-only asset
 * (the framebuffer's fb_binding.c exists because the framebuffer is
 * acquired/released/revoked across contexts; the WAD is neither), this maps
 * the WAD's *existing* physical pages directly into a freshly built LibOS
 * address space, the same way libos_build_image() itself maps kernel-
 * allocated pages into that address space without going through the
 * syscall layer -- both run at ring 0, before the LibOS the mapping is for
 * has any code running to ask permission of. The mapping is read-only
 * (no VMM_WRITE), so a launched LibOS can parse the WAD in place but never
 * corrupt the one copy backing it.
 *
 * First (and so far only) caller: kernel_main, staging freedoom2.wad for
 * src/libos_wad_viewer.c's ring-3 flat/automap demo.
 */

/* Fixed LibOS-window address for the read-only-mapped WAD, clear of every
 * other fixed window this codebase hands out inside [EXO_USER_VA_BASE,
 * EXO_USER_VA_END): libos_launch.h's code/data/stack sit below +0x20000,
 * src/libos_page_alloc.c's heap window is [+0x1000000, +0x2000000), and
 * src/libos_fb.c's framebuffer window is +0x4000000 (a few MB, VESA-sized).
 * +0x8000000 (128 MiB in) leaves that window a wide untouched gap and still
 * leaves this one clear of the identity-map/physical-memory concern
 * EXO_USER_VA_BASE itself exists to avoid (src/exo_syscall.h). */
#define LIBOS_WAD_VADDR       (0x0000400000000000ULL + 0x8000000ULL)

/* Generous headroom above freedoom2.wad's real size (~28 MiB) without
 * getting close to the next fixed window a future subsystem might claim;
 * libos_map_wad() itself only ever maps as many pages as the real WAD
 * needs, this is just the ceiling it refuses to map past. */
#define LIBOS_WAD_MAX_BYTES   (64ULL * 1024ULL * 1024ULL)

/*
 * Map [wad_paddr, wad_paddr + wad_size) read-only + user at LIBOS_WAD_VADDR
 * inside `pml4` (a root libos_build_image() already created and bound --
 * this does not create or bind an address space of its own). `wad_paddr`
 * and `wad_size` are expected straight from mmap_find_module(); both must
 * be page-aligned enough that rounding wad_size up to a page covers every
 * byte the WAD directory can reference (wad_init() itself bounds-checks
 * every lump against wad_size, so an over-mapped final page of zero bytes
 * past the real end is harmless).
 *
 * Returns VMM_OK, VMM_EINVAL if wad_size is 0 or the mapped range would
 * exceed LIBOS_WAD_MAX_BYTES, or whatever vmm_map_page_in() returned for the
 * page that failed -- in which case every page mapped so far by this call is
 * unmapped again before returning, mirroring libos_fb_map()'s partial-
 * mapping cleanup.
 */
int libos_map_wad(uint64_t *pml4, uint64_t wad_paddr, uint64_t wad_size);

#endif /* LIBOS_WAD_MAP_H */
