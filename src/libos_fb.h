#ifndef LIBOS_FB_H
#define LIBOS_FB_H

#include <stdint.h>

/*
 * libos_fb — LibOS-side framebuffer mapping helper (SCRUM-36).
 *
 * docs/syscall_spec.md's #4 (exo_fb_acquire) hands back the framebuffer's
 * physical address and geometry and binds it to the caller; the mapping
 * itself is deliberately left to the LibOS via the existing exo_page_map
 * (#2) rather than a dedicated kernel-side "fb_map" syscall — see
 * exo_fb_acquire's own comment in src/exo_syscall.h. This file is that
 * composition: acquire, then map every page of the reported range into a
 * fixed LibOS-window address, so a caller gets a single ready-to-write
 * pointer instead of having to drive both syscalls and the page-count math
 * itself.
 *
 * Built without -DEXO_KERNEL, like the rest of the libc shim
 * (src/stdlib.c, src/stdio.c, ...) — it calls the `syscall`-instruction
 * stubs in exo_syscall.h, which only exist on that side of the #ifdef.
 */

typedef struct {
    void    *vaddr;   /* mapped base, in the caller's own address space */
    uint32_t width;    /* pixels */
    uint32_t height;   /* pixels */
    uint32_t pitch;    /* bytes per scanline */
    uint8_t  bpp;      /* bits per pixel */
} libos_fb_t;

/* Acquire the framebuffer and map its whole [phys_addr, phys_addr + pitch *
 * height) range into the caller's address space at a fixed LibOS-window
 * address. Returns 0 and fills *fb_out on success. Returns a negative
 * EXO_E* (see exo_syscall.h) on failure: whatever exo_fb_acquire()
 * propagates (-EXO_EBUSY, -EXO_ENODEV, ...), or -EXO_EFAULT for a NULL
 * fb_out, or whatever the first failing exo_page_map() call returns, in
 * which case every page mapped so far by this call is unmapped again
 * before returning — a half-mapped framebuffer is never left behind. */
int libos_fb_map(libos_fb_t *fb_out);

#endif /* LIBOS_FB_H */
