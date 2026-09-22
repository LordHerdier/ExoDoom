#pragma once

/*
 * fb_compositor.h — copies the running LibOS's virtual framebuffer onto the
 * real hardware one (SCRUM-112).
 *
 * "Foreground" is simply context_current() (src/context.h): the cooperative
 * model this kernel has today only ever runs one context at a time, and
 * whichever one that is is exactly the one the acceptance criterion
 * ("switching between apps shows each app's own display state") wants on
 * screen. No separate foreground/background state exists or is needed —
 * every context_switch_request() (exo_yield, wadview launch, exo_exit) that
 * changes context_current() changes what fb_compositor_tick() shows on the
 * next call, with no extra bookkeeping.
 */

/*
 * Copy context_current()'s shadow framebuffer (src/fb_shadow.h) onto the
 * real hardware framebuffer (src/fb_binding.h's published geometry). A
 * quiet no-op if either is absent: no real framebuffer published yet, or
 * the running context never called exo_fb_acquire. Called by
 * fb_compositor_service() below -- not directly from irq0_handler()
 * anymore, see that function's own comment (SCRUM-181).
 */
void fb_compositor_tick(void);

/*
 * SCRUM-181: consumes pit_take_composite_pending() (src/pit.h) and, if a
 * tick is due, runs fb_compositor_tick(). Meant to be called from ordinary
 * syscall-dispatch context (src/syscall.c's exo_syscall_dispatch()), which
 * runs constantly since every LibOS polls exo_kbd_poll()/exo_get_ticks() in
 * its own loop -- not from inside an ISR, so the copy no longer holds
 * irq0_handler()'s own hardware IDT gate (and the PIT's EOI) hostage for
 * the whole memcpy the way it used to. IF stays off for the call, same as
 * the rest of the syscall it runs inside of; see fb_compositor.c for why it
 * must NOT be re-enabled here.
 */
void fb_compositor_service(void);
