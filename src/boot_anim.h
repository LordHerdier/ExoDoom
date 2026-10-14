#pragma once

#include <stdint.h>
#include <stddef.h>

#include "fb.h"

/*
 * boot_anim — the animation kernel_main plays before the boot log.
 *
 * The frames arrive as a GRUB multiboot 2 module (src/grub.cfg's second
 * `module2` line), exactly like the IWAD: identity-mapped by vmm_init(),
 * reserved by page_alloc_init(), found by mmap_find_module_named().  Nothing
 * is copied -- src/gif.c decodes straight out of the mapped module into the
 * one canvas this file owns, and this file scales that canvas onto the
 * framebuffer.
 *
 * The split is the same one src/pcm_mixer.c has from src/hda.c: the decoder
 * knows the format and no hardware, this knows the hardware and no format.
 * That is what makes the decoder testable without a framebuffer.
 */

/* The module's command line, i.e. the third word of src/grub.cfg's `module2`
 * line and the basename of the asset docker/scripts/build.sh copies onto the
 * ISO. All three spellings have to agree, and that script checks they do --
 * the same guard it already runs for the IWAD, for the same reason: a
 * multiboot module arrives as bytes with no name attached, so a mismatch is
 * not a build error, it is an animation that silently never plays. */
#define BOOT_ANIM_MODULE "tenor.gif"

#define BOOT_ANIM_OK        0
#define BOOT_ANIM_ENOENT   -1  /* no framebuffer, or no module to play */
#define BOOT_ANIM_EDECODE  -2  /* the module is not a GIF this decoder reads */

/* How long the animation is allowed to hold up the boot.  Two passes of a
 * two-second loop, with a wall-clock cap so a pathological file (a huge delay
 * field, thousands of frames) cannot turn a boot into a hang -- the cap is
 * checked between frames, so it bounds the whole call, not each one. */
#define BOOT_ANIM_LOOPS   1u
#define BOOT_ANIM_MAX_MS  8000u

/*
 * Play [data, size) on `fb` for `loops` passes or `max_ms`, whichever ends
 * first, then leave the screen cleared to black.  A keypress ends it early.
 *
 * Enables interrupts for its own duration and restores the caller's IF on the
 * way out: frame pacing is kernel_get_ticks_ms(), which only advances from
 * IRQ0, so calling this with interrupts masked would otherwise hlt forever on
 * the first frame.  That makes this safe to call from the pre-`sti` part of
 * kernel_main, which is where a boot animation has to go to be one.
 */
int boot_anim_play(framebuffer_t *fb, const uint8_t *data, size_t size,
                   uint32_t loops, uint32_t max_ms);
