#pragma once

#include "fb_binding.h"
#include "multiboot2.h"

/*
 * syscall_fb.h — framebuffer syscall handler (SCRUM-154).
 *
 * Binds exo_fb_acquire (#4) to the dispatcher in src/syscall.c and translates
 * the ABI-agnostic FB_BIND_* codes from src/fb_binding.c into the -EXO_E*
 * convention, the same split syscall_mem.c has against page_alloc.c.
 *
 * Call from kernel_main after syscall_init() (the handler table lives there)
 * and ahead of the TESTING branch, so the handler is bound for both a normal
 * boot and the in-kernel test run.  `fb_tag` is the multiboot2 framebuffer tag
 * or NULL; NULL means the bootloader gave us no framebuffer and every acquire
 * answers -EXO_ENODEV.
 */
void syscall_fb_init(const struct mb2_tag_framebuffer *fb_tag);
