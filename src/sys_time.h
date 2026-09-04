#pragma once

/*
 * sys_time.h — timer syscalls (SCRUM-33).
 *
 * The first syscall group to exist.  Deliberately the first: exo_get_ticks
 * takes no arguments and returns a value the kernel already computes
 * correctly, so if it misbehaves the fault is in the ABI rather than in the
 * handler — which is what makes it a useful end-to-end test of SCRUM-32's
 * entry path.
 */

/*
 * Bind the timer syscalls into the dispatch table.  Call once at boot, after
 * syscall_init().  Safe to call before pit_init(): the handler reads the tick
 * counter on demand rather than caching anything, so a syscall issued before
 * the PIT is running simply reports 0 ms.
 */
void sys_time_init(void);
