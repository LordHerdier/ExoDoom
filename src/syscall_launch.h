#pragma once

/*
 * syscall_launch.h — exo_launch_wad_viewer handler (SCRUM-178).
 *
 * Binds exo_launch_wad_viewer (#21) to the dispatcher in src/syscall.c: the
 * shell LibOS's `wadview` command (src/shell/shell_main.c) calls this to
 * launch the WAD/flat/automap viewer (src/libos_wad_viewer/libos_wad_viewer.c)
 * as a second, real LibOS context, coexisting with the shell rather than
 * replacing it. The handler stages the WAD module read-only into a fresh
 * address space (src/libos_wad_map.c/h, kernel-side only -- there is no
 * exo_wad_acquire syscall, see that header's own comment), builds the
 * viewer's image via libos_build_image() (src/libos_launch.c/h), and
 * context_switch_request()s to it (src/context.c/h, SCRUM-108) -- which
 * only succeeds because the calling context (the shell) now has a real
 * context_t row of its own (SCRUM-178's refactor of kernel_main's shell
 * launch, see src/kernel.c). Getting back to the shell needs no code here:
 * the viewer's own exo_yield() call round-robins back via the same
 * mechanism SCRUM-109 already built.
 *
 * Call from kernel_main after syscall_yield_init() (same placement rule
 * every other syscall_*_init() follows) and ahead of the TESTING branch.
 */
void syscall_launch_init(void);
