#pragma once

/*
 * syscall_fault.h — exo_fault_info handler (SCRUM-283).
 *
 * Binds exo_fault_info (#41). Call from kernel_main after syscall_init(),
 * ahead of the TESTING branch like every other syscall *_init().
 */
void syscall_fault_init(void);
