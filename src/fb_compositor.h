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
 * the running context never called exo_fb_acquire. Called from
 * irq0_handler() (src/pit.c), throttled — see that call site's own comment
 * for why.
 */
void fb_compositor_tick(void);
