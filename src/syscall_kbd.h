#pragma once

/*
 * syscall_kbd.h — the exo_kbd_poll handler (SCRUM-39, #6 in
 * docs/syscall_spec.md).
 *
 * Binds EXO_SYS_KBD_POLL to the kernel's existing keyboard ring
 * (src/ps2.c/h, src/kbd_ring.c/h) so a ring-3 LibOS can drain key events the
 * same way the ring-0 automap viewer in src/kernel.c already does by calling
 * ps2.h's exo_kbd_poll() directly -- this just exposes that same ring
 * through the syscall ABI instead of a direct call, for any LibOS that
 * needs keyboard input from ring 3 (docs/syscall_spec.md's DG_GetKey, and
 * any interactive LibOS demo before Doom itself).
 */

void syscall_kbd_init(void);
