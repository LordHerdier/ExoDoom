#pragma once

/*
 * syscall_rtc.h — exo_rtc_read handler (SCRUM-249).
 *
 * Binds exo_rtc_read (#35). Call from kernel_main after syscall_init() and
 * rtc_init(), ahead of the TESTING branch like every other syscall *_init().
 */
void syscall_rtc_init(void);
