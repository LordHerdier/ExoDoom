#pragma once

#include "page_alloc.h"

/*
 * Register the LibOS lifecycle syscall.
 *
 * Must run after syscall_init(), page_alloc_init(), and framebuffer binding
 * initialization so exo_exit can reclaim resources owned by its caller.
 */
void syscall_exit_init(void);

/*
 * Release every resource `owner` holds (fb binding + shadow, sound, disk
 * binding, owned pages). Shared by exo_exit and the ring-3 fault path
 * (SCRUM-283). Leaves the context row in place. Returns 0, or -EXO_EPERM if
 * `owner` is PAGE_OWNER_FREE or PAGE_OWNER_KERNEL.
 */
int context_teardown(page_owner_t owner);
