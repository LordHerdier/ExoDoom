#pragma once

/*
 * syscall_kbd.h — the exo_kbd_poll handler (#6).
 *
 * Binds EXO_SYS_KBD_POLL to the kernel's existing keyboard ring
 * (src/ps2.c/h, src/kbd_ring.c/h) so a ring-3 LibOS can drain key events the
 * same way the ring-0 automap viewer in src/kernel.c already does by calling
 * ps2.h's exo_kbd_poll() directly. First consumer: the ring-3 WAD/automap
 * viewer (src/libos_wad_viewer.c), which needs <-/-> from ring 3.
 */

void syscall_kbd_init(void);
