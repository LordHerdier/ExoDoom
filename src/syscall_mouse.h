#pragma once

/*
 * syscall_mouse.h — the exo_mouse_poll handler (SCRUM-52, #7 in
 * docs/syscall_spec.md).
 *
 * Binds EXO_SYS_MOUSE_POLL to the kernel's PS/2 mouse accumulator
 * (src/ps2_mouse.c/h), the same way syscall_kbd.c exposes the keyboard ring
 * through the syscall ABI instead of a direct call.
 */

void syscall_mouse_init(void);
