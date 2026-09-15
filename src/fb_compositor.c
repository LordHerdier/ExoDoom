#include "fb_compositor.h"
#include "fb_binding.h"
#include "fb_shadow.h"
#include "context.h"
#include "string.h"

#include <stdint.h>

void fb_compositor_tick(void) {
    const fb_geometry_t *geom = fb_binding_geometry();
    if (geom == NULL) {
        return;
    }

    uint64_t shadow_phys;
    if (fb_shadow_lookup(context_current(), &shadow_phys) != 0) {
        return;
    }

    uint64_t fb_bytes = (uint64_t)geom->pitch * (uint64_t)geom->height;

    /* Both extents are inside the kernel's identity map (src/vmm.c always
     * maps the framebuffer aperture; shadow pages come from the ordinary
     * PMM, mapped 1:1 like every other page the kernel manages), so the
     * physical addresses are directly dereferenceable from kernel context. */
    memcpy((void *)(uintptr_t)geom->phys_addr,
           (void *)(uintptr_t)shadow_phys,
           (size_t)fb_bytes);
}
