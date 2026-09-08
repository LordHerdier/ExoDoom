#include "syscall_fb.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "fb_binding.h"
#include "multiboot2.h"

#include <stddef.h>
#include <stdint.h>

/*
 * syscall_fb.c — the exo_fb_acquire handler (SCRUM-154).
 *
 * #4 is the framebuffer's *establish* operation in the secure-binding model
 * (docs/syscall_spec.md §3.3): it hands the caller the geometry it needs to
 * drive the screen and records it as the framebuffer's owner in the same
 * breath.  Everything that enforces the binding afterwards — exo_page_map
 * (SCRUM-153) and exo_exit reclamation (SCRUM-155) — goes through
 * src/fb_binding.c rather than through this file.
 */

/* #4 — claim the framebuffer and describe it.  Returns:
 *   0              *info_out filled, the caller now owns the framebuffer
 *   -EXO_EFAULT    info_out is not a usable pointer
 *   -EXO_EBUSY     another LibOS holds it
 *   -EXO_ENODEV    the bootloader gave this machine no framebuffer
 *
 * The pointer is checked before the binding is taken, so a caller that passes
 * garbage does not walk away owning the screen it never received.
 *
 * A re-acquire by the current owner succeeds and re-fills the struct: §3.2 #4
 * makes -EXO_EBUSY the answer for "another LibOS holds it", and DG_Init
 * calling twice after a soft restart is not that. */
static int64_t sys_fb_acquire(uint64_t info_out, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    /* All the validation available today: the identity map means every
     * address a LibOS can name is writable, so a NULL check is the only
     * mistake the kernel can catch.  Real user-range validation arrives with
     * per-context address spaces (SCRUM-48, spec §3.2). */
    if (info_out == 0)
        return -EXO_EFAULT;

    switch (fb_binding_acquire(syscall_current_context())) {
    case FB_BIND_OK:     break;
    case FB_BIND_EBUSY:  return -EXO_EBUSY;
    default:             return -EXO_ENODEV;
    }

    const fb_geometry_t *geom = fb_binding_geometry();

    /* fb_binding_acquire() only returns FB_BIND_OK when a framebuffer is
     * published, so geom is non-NULL here; the check keeps a future caller
     * that reorders these two from dereferencing NULL in ring 0.  Hand the
     * binding back rather than leaving the caller owning a screen it was told
     * it did not get. */
    if (geom == NULL) {
        fb_binding_release(syscall_current_context());
        return -EXO_ENODEV;
    }

    exo_fb_info_t *out = (exo_fb_info_t *)(uintptr_t)info_out;

    /* Field by field, not a struct copy: fb_geometry_t is the kernel's own
     * view and exo_fb_info_t is ABI (src/exo_syscall.h), and they are allowed
     * to drift apart. */
    out->phys_addr = geom->phys_addr;
    out->width     = geom->width;
    out->height    = geom->height;
    out->pitch     = geom->pitch;
    out->bpp       = geom->bpp;

    /* ABI: the kernel zeroes the padding rather than leaking whatever the
     * LibOS left in the struct back to it as if it were kernel data. */
    out->reserved[0] = 0;
    out->reserved[1] = 0;
    out->reserved[2] = 0;

    return 0;
}

void syscall_fb_init(const struct mb2_tag_framebuffer *fb_tag)
{
    if (fb_tag == NULL) {
        fb_binding_init(NULL);
    } else {
        fb_geometry_t geom = {
            .phys_addr = fb_tag->addr,
            .width     = fb_tag->width,
            .height    = fb_tag->height,
            .pitch     = fb_tag->pitch,
            .bpp       = fb_tag->bpp,
        };

        fb_binding_init(&geom);
    }

    /* Registered even when there is no framebuffer: the syscall exists, it
     * just reports -EXO_ENODEV.  Leaving it unbound would report -EXO_ENOSYS
     * instead and tell the LibOS the kernel is too old rather than that the
     * machine is headless. */
    exo_syscall_register(EXO_SYS_FB_ACQUIRE, sys_fb_acquire);
}
