#pragma once

/*
 * syscall_pane.h — exo_pane_set (#43, SCRUM-296): a launcher places a context
 * it holds in the split screen.
 *
 * The pane table (src/fb_layout.h, SCRUM-197) has to know a context's rect
 * before that context's first exo_fb_acquire, because the shadow buffer is
 * sized to the pane. A held launch (#36 with EXO_LAUNCH_HOLD) is exactly that
 * window: built, not yet run. So the authority is the hold's: only the context
 * that built the hold may place it, and only until it starts
 * (launch_hold_check(), src/syscall_launch.h). A running program's pane is not
 * movable from ring 3; that is a separate story.
 *
 * Call from kernel_main next to syscall_launch_init(), ahead of the TESTING
 * branch.
 */
void syscall_pane_init(void);
