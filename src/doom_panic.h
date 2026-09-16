#pragma once

#include <stdarg.h>
#include <stddef.h>

/*
 * doom_panic — the back end for Doom's I_Error and I_Quit (SCRUM-83).
 *
 * I_Error is how Doom reports why it failed to start.  On a desktop build it
 * writes to stderr and then opens a zenity dialog; on bare metal there is no
 * stderr, no zenity, and no process to exit from, so the vendored
 * src/doom/i_system.c calls in here instead.
 *
 * ── Why this lives in src/ and not in the vendored tree ────────────────
 *
 * SCRUM-83 says the stubs live in src/doom/, and they do -- I_Error and
 * I_Quit are still defined there, because they must be: a second definition
 * anywhere else would be a duplicate symbol.  What is *not* in there is the
 * machinery they now call, and that split is deliberate:
 *
 *   - src/doom/ is vendored verbatim (SCRUM-63) so a re-vendor stays a clean
 *     drop-in.  Every line of ExoDoom logic added there is a line of merge
 *     conflict later, so the patch to i_system.c is kept to the smallest
 *     thing that redirects control flow and nothing more.
 *   - Nothing under src/doom/ links into build/exodoom yet
 *     (docs/libc_audit.md), so a unit test cannot reach a function defined
 *     there.  Everything below IS linked into the kernel by build.sh's
 *     C-source glob, which is what lets tests/kernel/test_doom_panic_k.c
 *     drive the formatting, the chunking and the recursion guard from ring 0
 *     -- the parts where the bugs actually are.
 *
 * ── Which serial path ──────────────────────────────────────────────────
 *
 * Same #ifdef EXO_KERNEL split as src/stdio.c and src/doomgeneric_exo.c, for
 * the same reason: a ring-3 LibOS reaches COM1 only through
 * exo_serial_write() (#8), while a kernel build must call src/serial.c
 * directly, because a `syscall` executed from ring 0 would sysretq the
 * caller down to CPL 3.  Both paths go through vprintf(), so there is one
 * formatting engine and one sink rather than a second copy here.
 */

/*
 * Print "<prefix><formatted message>" followed by a newline and mark a panic
 * as in progress.  `prefix` may be NULL.
 *
 * This RETURNS, and the split from doom_panic_halt() below is the point: it
 * lets I_Error get the message out BEFORE it walks Doom's atexit list.  Those
 * handlers are shutdown code running on a machine that has just declared
 * itself broken, so one of them faulting is a real possibility -- and if the
 * message had not been flushed first, the fault would take with it the one
 * line explaining why any of this happened.
 *
 * Re-entry is handled rather than trusted: if a panic is already in progress
 * (an I_Error raised from inside one of those atexit handlers, say) this
 * prints a short fixed notice and halts immediately instead of running the
 * formatter again -- on the ring-3 side that recursion would land on a stack
 * that is exactly one 4 KiB page (LIBOS_LAUNCH_STACK_VADDR,
 * src/libos_launch.h), so the second failure would present as a stack
 * overflow and bury the first.
 *
 * Doom has its own `already_quitting` flag for the same purpose, but in the
 * vendored source it does not actually stop anything: the `exit(-1)` it
 * guards sits inside `#if ORIGCODE`, so the second call used to fall straight
 * through and re-run the whole function.
 */
void doom_panic_begin(const char *prefix, const char *fmt, va_list ap);

/*
 * Stop, permanently.  Never returns.
 *
 * Ring 3 asks the kernel to tear the context down (exo_exit, #20) so its
 * pages and framebuffer binding are released; ring 0 flushes COM1 and halts
 * with interrupts masked.
 */
void doom_panic_halt(void);

/*
 * Print `msg` (may be NULL) and halt forever.  Never returns.
 *
 * This is I_Quit's back end: an orderly shutdown rather than an error, but
 * on bare metal it ends the same way, because there is no process to return
 * to and nothing else scheduled to run.
 */
void doom_halt(const char *msg);

/*
 * Write `len` bytes to the serial console, splitting the write so no single
 * exo_serial_write exceeds the kernel's own SERIAL_WRITE_MAX_LEN cap.
 *
 * Exposed for tests: the cap is a syscall-side rule (src/syscall_serial.h)
 * that a ring-3 caller can only discover by being refused -EXO_EINVAL, so
 * getting the chunking wrong would silently drop the tail of exactly the
 * message someone is trying to read.
 */
void doom_panic_write(const char *buf, size_t len);

/*
 * Non-zero once doom_panic_begin() has been entered.  Exposed so a test can
 * observe the recursion guard without having to survive a halt.
 */
int doom_panic_in_progress(void);

/*
 * Reset the recursion guard.  TEST ONLY -- there is no legitimate reason for
 * a running system to un-panic, but a unit test that checks the guard needs
 * to put it back so later tests still see a clean flag.
 */
void doom_panic_reset(void);
