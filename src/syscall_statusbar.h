#pragma once

/*
 * syscall_statusbar.h -- exo_statusbar (#45, SCRUM-278): turn the compositor's
 * status bar on or off. The bar itself is src/status_bar.h.
 *
 * Call from kernel_main next to syscall_kbd_bind_init(), ahead of the TESTING
 * branch.
 */
void syscall_statusbar_init(void);
