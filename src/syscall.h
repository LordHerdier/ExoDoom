#pragma once
#include <stdint.h>

/*
 * syscall.h — kernel side of the exokernel syscall ABI (SCRUM-32).
 *
 * src/exo_syscall.h is the ABI: numbers, argument structs, error codes and
 * the LibOS-side stubs.  This header is the kernel's half — how a `syscall`
 * that arrives from ring 3 finds its way to a C function.
 *
 * The path is:
 *
 *   ring 3 `syscall`
 *     -> syscall_entry            (src/syscall_entry.s: stack swap, register
 *                                  save, argument marshalling)
 *     -> exo_syscall_dispatch     (below: range check, table lookup)
 *     -> the registered handler
 *     -> back out through syscall_entry, `sysretq`
 *
 * See docs/syscall_spec.md §3.1 for the calling convention and §3.4 for the
 * entry path itself.
 */

/*
 * A syscall handler.  Every handler takes the full six arguments regardless
 * of how many the ABI defines for it; the dispatcher cannot know an
 * individual syscall's arity, and unused parameters cost nothing because
 * they are already in the right registers.  Return values follow §3.1: >= 0
 * on success, -EXO_E* on failure.
 */
typedef int64_t (*exo_handler_t)(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6);

/*
 * Program IA32_EFER.SCE, STAR, LSTAR and FMASK so the `syscall` instruction
 * has somewhere to land.  Must run before any ring-3 code exists; called
 * from kernel_main ahead of the TESTING branch.  Idempotent.
 */
void syscall_init(void);

/*
 * Install a handler.  Replaces any previous handler for that number; passing
 * NULL removes it, which is what makes the number report as unimplemented
 * again.  Out-of-range numbers are ignored.
 *
 * Handlers are registered rather than listed in a const table so that a test
 * can bind a scratch number without adding a non-ABI syscall to
 * exo_syscall.h.
 */
void exo_syscall_register(uint64_t num, exo_handler_t fn);

/* Read back a registered handler; NULL if the number is unbound or out of
 * range.  Exists for tests and for handlers that chain. */
exo_handler_t exo_syscall_handler(uint64_t num);

/*
 * The C half of the entry path, called by syscall_entry with the syscall
 * number from RAX and the six arguments in SysV order.  Returns -EXO_ENOSYS
 * for numbers >= EXO_SYS_COUNT or with no handler bound; otherwise returns
 * whatever the handler returns, which lands back in the caller's RAX.
 */
int64_t exo_syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5,
                             uint64_t a6);

/* Defined in src/syscall_entry.s — the LSTAR target.  Declared here so
 * syscall_init and the MSR readback test can both name it. */
extern void syscall_entry(void);
