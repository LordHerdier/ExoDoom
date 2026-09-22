#include "fb_compositor.h"
#include "fb_binding.h"
#include "fb_shadow.h"
#include "context.h"
#include "string.h"
#include "pit.h"

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

void fb_compositor_service(void) {
    if (!pit_take_composite_pending()) {
        return;
    }

    /* SCRUM-181: this runs from ordinary syscall-dispatch context
     * (src/syscall.c) instead of from inside irq0_handler(), which is the
     * point -- the ISR itself now only ever sets a flag and sends EOI, no
     * longer holding a hardware IDT gate (and the PIT's own EOI) behind a
     * multi-MB memcpy.
     *
     * IF stays off for this call, same as the rest of the syscall it runs
     * inside of -- it must NOT be re-enabled here. context.h documents that
     * context_switch_request()'s staging through the single
     * context_switch_out_regs/_in_regs/_pending globals (SCRUM-108) is only
     * correct "because IA32_FMASK clears IF for the whole syscall/switch
     * window ... so no second entry can interleave"; src/ps2.c's Ctrl+Tab
     * IRQ1 handler leans on exactly that guarantee too (its own comment:
     * every *other* context_switch_request() caller is itself a syscall
     * handler, so "there was never a window for a second request to land on
     * top of an unconsumed one"). Re-enabling IF here would open exactly
     * that window mid-dispatch: an IRQ1 Ctrl+Tab could arm a switch, then
     * this same syscall's own handler (sys_yield, for one, which calls
     * context_switch_request() with no pending-guard at all) could arm a
     * second one on top of it, clobbering the first's saved-register
     * pointers -- silent register corruption and a resume into the wrong
     * context. Getting the memcpy out of the ISR is this ticket's fix;
     * making it preemptible too would need context_switch_request() itself
     * to be reentrancy-safe first, which it isn't yet. */
    fb_compositor_tick();
}
