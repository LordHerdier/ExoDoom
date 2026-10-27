#pragma once

/*
 * syscall_kbd_bind.h — exo_kbd_bind (#44, SCRUM-297): a launcher chooses
 * which keyboard the context it holds reads.
 *
 * The routing is SCRUM-199's (src/kbd_source.h); this is only the way to
 * assign it. The authority is the hold's, as for exo_pane_set
 * (src/syscall_pane.h): only the context that built the hold may bind it, and
 * only until it starts, so no running program can take another's keyboard.
 *
 * Call from kernel_main next to syscall_pane_init(), ahead of the TESTING
 * branch.
 */
void syscall_kbd_bind_init(void);
