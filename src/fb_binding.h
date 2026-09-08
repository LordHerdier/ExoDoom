#pragma once
#include <stdint.h>

#include "page_alloc.h"   /* page_owner_t — the resource ownership tag type */

/*
 * fb_binding.h — framebuffer secure binding (SCRUM-154, epic SCRUM-151).
 *
 * The framebuffer is the one hardware resource Doom cannot run without, and
 * until now it was not a *bound* resource at all: its physical range simply
 * sat in the identity map, reachable by any LibOS that knew the address.  This
 * module makes it an owned resource in the same sense as a physical page
 * (src/page_alloc.c):
 *
 *   - exo_fb_acquire **establishes** the binding — one owner at a time,
 *     -EXO_EBUSY for anyone else (docs/syscall_spec.md §3.3).
 *   - exo_page_map **enforces** it — mapping framebuffer physical pages
 *     requires holding the binding, otherwise -EXO_EPERM.
 *   - exo_exit **reclaims** it, so a LibOS that dies holding the screen does
 *     not lock the machine's display forever.
 *
 * Framebuffer pages need their own table rather than the PMM's owner tags
 * because they are not PMM pages: MMIO lives outside the usable-RAM region
 * page_alloc_init() manages, so page_owner() reports PAGE_OWNER_FREE for every
 * one of them.  That is exactly the hole SCRUM-153's generic check cannot
 * close, and why its acceptance criteria hand framebuffer pages to this
 * module.
 *
 * The layering matches page_alloc.c / syscall_mem.c: this file is
 * ABI-agnostic and speaks FB_BIND_* / FB_MAP_* status codes, and
 * src/syscall_fb.c maps them onto the -EXO_E* convention.
 */

/* Geometry of the framebuffer the kernel was handed at boot.  A kernel-side
 * mirror of the ABI struct exo_fb_info_t (src/exo_syscall.h): the syscall
 * layer copies field by field rather than casting, so the ABI layout can
 * change without dragging the binding table with it. */
typedef struct {
    uint64_t phys_addr;   /* framebuffer base, physical                     */
    uint32_t width;       /* pixels                                         */
    uint32_t height;      /* pixels                                         */
    uint32_t pitch;       /* bytes per scanline, may exceed width * bpp / 8  */
    uint8_t  bpp;         /* bits per pixel                                 */
} fb_geometry_t;

/* Status codes from fb_binding_acquire(). */
#define FB_BIND_OK       0    /* caller now holds the framebuffer            */
#define FB_BIND_EBUSY  (-1)   /* a different context holds it                */
#define FB_BIND_ENODEV (-2)   /* this machine has no usable framebuffer      */

/* Verdicts from fb_binding_check_map().  Deliberately three-valued: a caller
 * must not be able to read "not framebuffer memory" as "permitted". */
#define FB_MAP_NOT_FB    0    /* paddr is not framebuffer memory — the caller
                               * falls through to generic page ownership     */
#define FB_MAP_ALLOW     1    /* framebuffer memory and `who` holds it       */
#define FB_MAP_DENY      2    /* framebuffer memory, held by someone else or
                               * by nobody — the caller must return -EPERM   */

/*
 * Publish the framebuffer the kernel owns and drop any existing binding.
 * Called once from kernel_main with the geometry from the multiboot2
 * framebuffer tag, ahead of the TESTING branch so acquire works identically
 * on a normal boot and under the test runner.
 *
 * `geom` may be NULL, and a degenerate or wrapping geometry (zero base, zero
 * extent, or a range that runs off the end of the physical address space) is
 * treated the same way: the machine has no framebuffer and every acquire
 * answers FB_BIND_ENODEV.  Returns FB_BIND_OK if a framebuffer was published,
 * FB_BIND_ENODEV otherwise.
 */
int fb_binding_init(const fb_geometry_t *geom);

/* The published geometry, or NULL when there is no framebuffer.  The pointer
 * is to module-static storage and stays valid until the next
 * fb_binding_init(). */
const fb_geometry_t *fb_binding_geometry(void);

/*
 * Bind the framebuffer to `who`.
 *
 *   FB_BIND_OK      `who` now owns it — including the case where it already
 *                   did, so a LibOS that acquires twice is not punished for
 *                   it.  Only "another LibOS holds it" is -EBUSY per §3.2 #4.
 *   FB_BIND_EBUSY   somebody else owns it
 *   FB_BIND_ENODEV  no framebuffer exists
 */
int fb_binding_acquire(page_owner_t who);

/*
 * Release the binding if `who` holds it; a no-op otherwise, so reclamation can
 * call it unconditionally for a context that may never have acquired.  This is
 * the hook SCRUM-155's exo_exit reclamation calls when tearing a context down,
 * and the mechanism half of the revocation model (SCRUM-156).
 */
void fb_binding_release(page_owner_t who);

/* Current owner, or PAGE_OWNER_FREE when the framebuffer is unheld. */
page_owner_t fb_binding_owner(void);

/* Whether `paddr` falls inside the framebuffer's physical range, page-granular:
 * the range is widened to whole 4 KiB pages, because mapping permission is
 * decided per page and the base need not be page-aligned. */
int fb_binding_contains(uint64_t paddr);

/*
 * The exo_page_map / exo_page_unmap permission check for one physical page
 * (SCRUM-154 AC2).  SCRUM-153's handler asks this *first* and only consults
 * generic page ownership when it answers FB_MAP_NOT_FB:
 *
 *     switch (fb_binding_check_map(paddr, syscall_current_context())) {
 *     case FB_MAP_ALLOW:  break;                     // FB owner, proceed
 *     case FB_MAP_DENY:   return -EXO_EPERM;
 *     case FB_MAP_NOT_FB: // fall through to page_owner(paddr) == caller
 *     }
 *
 * There is no kernel bypass: PAGE_OWNER_KERNEL is denied like anyone else
 * unless it actually holds the binding.  The kernel reaches the framebuffer
 * through the identity map (src/fb.c), never through exo_page_map, so a bypass
 * would only be a hole for a LibOS that learns to spoof a context id.
 */
int fb_binding_check_map(uint64_t paddr, page_owner_t who);
