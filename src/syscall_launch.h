#pragma once

/*
 * syscall_launch.h — the EXO_SYS_LAUNCH handler (SCRUM-178, SCRUM-184).
 *
 * Binds EXO_SYS_LAUNCH (#21) to the dispatcher in src/syscall.c. One syscall
 * launches every ring-3 LibOS app, selected by an EXO_LAUNCH_APP_* id
 * argument (src/exo_syscall.h); SCRUM-184 collapsed the four per-app numbers
 * that came before it (#21..#24) into this one. The shell's `wadview`,
 * `clock`, `snake` and `doom` commands (src/shell/shell_main.c) each call it
 * with their own id.
 *
 * The handler builds the app's image via libos_build_image()
 * (src/libos_launch.c/h) into a fresh address space, stages the WAD module
 * read-only for the apps whose table row asks for it (src/libos_wad_map.c/h,
 * kernel-side only -- there is no exo_wad_acquire syscall, see that header's
 * own comment), and context_switch_request()s to it (src/context.c/h,
 * SCRUM-108) -- which only succeeds because the calling context (the shell)
 * has a real context_t row of its own (SCRUM-178's refactor of kernel_main's
 * shell launch, see src/kernel.c). Each app coexists with the shell rather
 * than replacing it. Getting back needs no code here: the app's own
 * exo_yield() round-robins back via the mechanism SCRUM-109 built, and
 * Ctrl+Tab (SCRUM-111) covers an app that never yields.
 *
 * Call from kernel_main after syscall_yield_init() (same placement rule
 * every other syscall_*_init() follows) and ahead of the TESTING branch.
 */
void syscall_launch_init(void);
