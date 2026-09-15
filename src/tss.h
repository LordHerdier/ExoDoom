#ifndef TSS_H
#define TSS_H

#include <stdint.h>

/*
 * TSS — Task State Segment (SCRUM-46).
 *
 * Long mode does not use the TSS for hardware task switching; the only field
 * that matters here is RSP0. On any interrupt/exception whose gate has
 * IST=0 (every gate idt_init() installs does), a CPL 3 -> CPL 0 transition
 * makes the CPU load SS:RSP for the handler from TSS.RSP0 rather than
 * trusting whatever RSP the interrupted code happened to have — the same
 * problem src/syscall_entry.s solves by hand for the `syscall` path, which
 * never consults the TSS at all (SYSCALL/SYSRET are not gate-based). Without
 * a TSS loaded, TR is null and that lookup itself faults: a #PF taken at
 * CPL 3 escalates to #GP, then #DF (needing the same stack switch), and the
 * machine triple-faults before any handler runs. See the ring3_probe.s and
 * fault.c comments this ticket resolves.
 */

// Configures the one kernel TSS (RSP0 + a deny-everything I/O bitmap),
// installs its descriptor into gdt64, and loads TR via `ltr`. Idempotent.
void tss_init(void);

// The configured RSP0 (top of the TSS's kernel stack) — exposed for tests
// that need to check a fault frame landed on it rather than for production
// use.
uint64_t tss_rsp0(void);

// Size in bytes of the stack tss_rsp0() is the top of — together they give
// the full [tss_rsp0() - TSS_KERNEL_STACK_SIZE, tss_rsp0()) range a test can
// check an address against.
#define TSS_KERNEL_STACK_SIZE 16384

#endif
