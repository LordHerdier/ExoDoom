#include "syscall_fb.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "fb_binding.h"
#include "fb_shadow.h"
#include "multiboot2.h"

#include <stddef.h>
#include <stdint.h>

/*
 * syscall_fb.c — the exo_fb_acquire handler (SCRUM-154, multiplexed by
 * SCRUM-112).
 *
 * #4 no longer hands out the real hardware framebuffer: it hands the caller
 * its own private, RAM-backed surface matching the real framebuffer's
 * geometry (src/fb_shadow.c). Every context that calls this gets one, held
 * for as long as it lives — no exclusivity, no -EXO_EBUSY, unlike the
 * SCRUM-154 original this replaces. What actually reaches the screen is
 * decided by src/fb_compositor.c, not by who called this; src/fb_binding.c
 * still publishes the real framebuffer's geometry (fb_binding_geometry(),
 * the source fb_shadow_acquire() sizes new buffers from) but its
 * acquire/release/owner half is no longer exercised from this path — the
 * real framebuffer is now unmappable by any LibOS, full stop.
 */

/* #4 — claim a private virtual framebuffer and describe it.  Returns:
 *   0              *info_out filled with the caller's own surface
 *   -EXO_EFAULT    info_out is not a usable pointer
 *   -EXO_ENOMEM    no contiguous run of pages that size is free
 *   -EXO_ENODEV    the bootloader gave this machine no framebuffer
 *
 * The pointer is checked before anything is allocated, so a caller that
 * passes garbage does not walk away owning pages it never received.
 *
 * A re-acquire by the same caller succeeds and re-fills the struct with the
 * same buffer (fb_shadow_acquire() is idempotent per caller) — DG_Init
 * calling twice after a soft restart gets its own surface back, not a
 * fresh one. */
static int64_t sys_fb_acquire(uint64_t info_out, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    /* Same LibOS-window bounds check exo_serial_write uses (src/syscall.h) —
     * a kernel address here is otherwise indistinguishable from a real one:
     * the map is an identity map, so every kernel address is present and
     * writable, and without this check the kernel would fill a caller-chosen
     * 24 bytes of its own memory on request (SCRUM-54). */
    if (!exo_range_in_user_window(info_out, sizeof(exo_fb_info_t)))
        return -EXO_EFAULT;

    exo_fb_info_t *out = (exo_fb_info_t *)(uintptr_t)info_out;

    switch (fb_shadow_acquire(syscall_current_context(), out)) {
    case FB_SHADOW_OK:     return 0;
    case FB_SHADOW_ENOMEM: return -EXO_ENOMEM;
    default:                return -EXO_ENODEV;
    }
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
