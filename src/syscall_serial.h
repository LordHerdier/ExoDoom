#pragma once

/*
 * syscall_serial.h — exo_serial_write handler (SCRUM-50).
 *
 * Binds exo_serial_write (#8) to the dispatcher in src/syscall.c, the same
 * split syscall_mem.c/syscall_fb.c already have against their own subsystems:
 * src/serial.c knows how to drive COM1, this file knows what a LibOS is
 * allowed to ask it to send and how to answer in -EXO_E* terms.
 *
 * Call from kernel_main after syscall_init() (the handler table lives there)
 * and ahead of the TESTING branch, so the handler is bound for both a normal
 * boot and the in-kernel test run — same placement rule as syscall_mem_init()
 * and syscall_fb_init().
 */
void syscall_serial_init(void);
