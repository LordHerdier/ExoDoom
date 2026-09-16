#pragma once

/* The most bytes a single exo_serial_write call will send. The whole call
 * runs with interrupts off (`syscall`'s FMASK clears IF, src/syscall.c), so
 * an uncapped write would stall the timer and keyboard IRQs for as long as
 * the busy-wait UART takes to drain it — 4096 bytes at 38400 baud is a
 * little over a millisecond, generous for a printf/fprintf line. Exposed
 * here (rather than kept `static` in syscall_serial.c) so
 * test_syscall_serial_k.c can assert the real limit instead of a restated
 * literal.
 *
 * ONE PLACE RESTATES THIS VALUE: src/doom_panic.c's DOOM_PANIC_SERIAL_MAX.
 * That file is compiled for ring 3 as well as for the kernel, and a ring-3
 * TU cannot include this kernel-only header. It does not drift silently --
 * its kernel build _Static_assert's the two against each other, so changing
 * the cap here without changing it there fails the build rather than
 * shortening a panic message nobody is watching. Change both. */
#define SERIAL_WRITE_MAX_LEN 4096u

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
