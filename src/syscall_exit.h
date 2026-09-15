#pragma once

/*
 * Register the LibOS lifecycle syscall.
 *
 * Must run after syscall_init(), page_alloc_init(), and framebuffer binding
 * initialization so exo_exit can reclaim resources owned by its caller.
 */
void syscall_exit_init(void);
