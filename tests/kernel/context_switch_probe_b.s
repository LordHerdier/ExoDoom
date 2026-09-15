/*
 * context_switch_probe_b.s — SCRUM-108 context switch test probe, side B.
 *
 * Never launched via libos_enter() -- context_prime() seeds its
 * context_regs_t directly with this entry point and its own stack, and it
 * is reached for the first time via context_switch_tail's iretq when side A
 * calls SYS_SWITCH naming this context's id. Writes a marker to its own
 * data page, then calls SYS_SWITCH itself, naming side A's id, to switch
 * back -- proving both directions of the round trip in one test.
 *
 * Data layout at LIBOS_LAUNCH_DATA_VADDR, same technique as side A:
 *   +0x00  peer_id  (in: side A's context id, set by the test before launch)
 *   +0x08  mark     (out: MARK, written before switching back)
 */

#include "exo_syscall.h"
#include "libos_launch.h"

.code64

.set SYS_SWITCH, EXO_SYS_YIELD
.set MARK, 0xB000000000000001

.global context_switch_probe_b
.global context_switch_probe_b_end
context_switch_probe_b:
    movabsq $LIBOS_LAUNCH_DATA_VADDR, %rax
    movabsq $MARK, %rdx
    movq %rdx, 0x08(%rax)

    movq 0x00(%rax), %rdi      /* peer_id -> SYS_SWITCH's argument */
    movq $SYS_SWITCH, %rax
    syscall
context_switch_probe_b_end:
    ud2                          /* unreachable: side B is never resumed
                                  * again in this test. */
