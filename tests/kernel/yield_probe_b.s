/*
 * yield_probe_b.s — SCRUM-109 real exo_yield syscall test probe, side B.
 *
 * Never launched via libos_enter() -- context_prime() seeds its
 * context_regs_t directly with this entry point and its own stack, and it
 * is reached for the first time via context_switch_tail's iretq when side
 * A's exo_yield() round-robins to it (src/context.c's
 * context_next_ready()). Writes a marker to its own data page, then calls
 * the real exo_yield() itself, with no argument, to yield back -- proving
 * both directions of the round trip in one test, the same shape
 * context_switch_probe_b.s (SCRUM-108) proves for the raw primitive.
 *
 * Data layout at LIBOS_LAUNCH_DATA_VADDR, same technique as side A:
 *   +0x00  mark  (out: MARK, written before yielding back)
 */

#include "exo_syscall.h"
#include "libos_launch.h"

.code64

.set MARK, 0xB000000000000001

.global yield_probe_b
.global yield_probe_b_end
yield_probe_b:
    movabsq $LIBOS_LAUNCH_DATA_VADDR, %rax
    movabsq $MARK, %rdx
    movq %rdx, 0x00(%rax)

    movq $EXO_SYS_YIELD, %rax
    syscall
yield_probe_b_end:
    ud2                          /* unreachable: side B is never resumed
                                  * again in this test. */
