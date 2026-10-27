#pragma once

/*
 * fb_compositor.h — copies LibOS virtual framebuffers onto the real hardware
 * one (SCRUM-112, split panes SCRUM-197).
 *
 * Two modes, chosen every tick:
 *
 *   Full-screen (the default, and all that existed before SCRUM-197): show
 *   context_current()'s shadow framebuffer. The cooperative model this kernel
 *   has today only ever runs one context at a time, and whichever one that is
 *   is the one the acceptance criterion ("switching between apps shows each
 *   app's own display state") wants on screen. Every context_switch_request()
 *   (exo_yield, wadview launch, exo_exit) that changes context_current()
 *   changes what the next tick shows, with no extra bookkeeping.
 *
 *   Split: when src/fb_layout.h has panes and context_current() is one of
 *   them, show EVERY pane, each context's shadow framebuffer in its own rect.
 *   The context that is running only decides which mode applies, not which
 *   panes are drawn, so two panes stay on screen while the CPU alternates
 *   between them. A context with no pane (the shell) is shown full-screen as
 *   before, which is also how you get back out of a split.
 */

/*
 * Composite one frame onto the real hardware framebuffer (src/fb_binding.h's
 * published geometry) from the shadow framebuffers (src/fb_shadow.h): the
 * running context's, or every pane's, per the header comment. A quiet no-op
 * if no real framebuffer is published yet; a context that never called
 * exo_fb_acquire contributes nothing (a blank pane in split mode). Called by
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
