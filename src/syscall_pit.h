#pragma once

/*
 * syscall_pit.h — exo_get_ticks handler (SCRUM-172).
 *
 * Binds exo_get_ticks (#5) to the dispatcher in src/syscall.c: the number and
 * the LibOS-side stub have existed since SCRUM-24, but nothing answered it —
 * split out of SCRUM-51 because that story's "timer works from ring 3"
 * acceptance criterion had nothing to port to without this handler first.
 *
 * Call from kernel_main after syscall_init() (the handler table lives there)
 * and ahead of the TESTING branch, same placement rule as syscall_mem_init(),
 * syscall_fb_init() and syscall_serial_init(). Unlike those, it also needs
 * pit_init() to have already run — the tick count it reports is
 * src/pit.c's kernel_get_ticks_ms(), which stays at 0 without IRQ0 wired and
 * interrupts enabled. kernel_main moves that wiring ahead of the TESTING
 * branch too, for the same reason.
 */
void syscall_pit_init(void);
