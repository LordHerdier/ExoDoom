/*
 * fault_probe.s — deliberate faulting accesses with a known resume point
 * (SCRUM-17).
 *
 * The page fault test needs two things the C compiler will not reliably give
 * it: an access guaranteed to reach memory, and an address to resume at once
 * the handler has seen the fault.
 *
 * The obvious C spelling -- a volatile access followed by a `&&label` fixup --
 * does not work at -O2.  GCC only anchors a label whose address is taken if a
 * computed `goto` in the same function jumps to it; with the jump living in
 * the fault handler instead, GCC folded the label onto the function prologue,
 * so "resuming" restarted the test function and faulted again forever.  Here
 * the instruction and its resume point are both fixed at assembly time.
 *
 * Each probe is a leaf with no prologue: the return address is on top of the
 * stack throughout, so resuming at the `ret` returns to the caller with the
 * stack exactly as the call left it.
 */

.code64

/* void fault_probe_read(uint64_t va) — reads 8 bytes from va. */
.global fault_probe_read
.global fault_probe_read_resume
fault_probe_read:
    mov (%rdi), %rax
fault_probe_read_resume:
    ret

/* void fault_probe_write(uint64_t va) — writes 8 bytes to va. */
.global fault_probe_write
.global fault_probe_write_resume
fault_probe_write:
    movq $0x5A5A5A5A, (%rdi)
fault_probe_write_resume:
    ret
