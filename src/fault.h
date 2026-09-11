#ifndef FAULT_H
#define FAULT_H

#include <stdint.h>
#include <stddef.h>

/*
 * fault — CPU exception diagnostics (SCRUM-17).
 *
 * Until this landed, vector 14 was handled by isr.s's error_stub: it discarded
 * the CPU-pushed error code correctly (SCRUM-135) and then iretq'd straight
 * back to the faulting instruction, which faulted again immediately.  A page
 * fault was therefore a silent infinite loop -- not a triple fault, but not a
 * diagnostic either.  pf_stub + page_fault_handler replace that on vector 14
 * with CR2, the decoded error code, the faulting RIP, and what the live page
 * tables actually say about the address, printed to COM1 before the machine
 * halts.
 *
 * The other nine error-code vectors still use error_stub; vector 13 (GPF) is
 * tracked separately (docs/drivers/idt.md §9) and can reuse everything here.
 */

/*
 * The stack as pf_stub leaves it, low address first.  The register block is
 * pushed by PUSH_ALL_REGS in reverse field order; error_code is pushed by the
 * CPU; the last five are the iretq frame.
 *
 * Field order is load-bearing -- change this and change PUSH_ALL_REGS /
 * POP_ALL_REGS in src/isr.s to match.
 */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} exception_frame_t;

/* Page-fault error code bits (Intel SDM 3A §4.7). */
#define PF_ERR_PRESENT  (1ULL << 0)  /* 0 = not present, 1 = protection    */
#define PF_ERR_WRITE    (1ULL << 1)  /* 0 = read, 1 = write                */
#define PF_ERR_USER     (1ULL << 2)  /* 0 = supervisor (CPL 0-2), 1 = user */
#define PF_ERR_RESERVED (1ULL << 3)  /* a reserved bit was set in an entry */
#define PF_ERR_IFETCH   (1ULL << 4)  /* instruction fetch (needs EFER.NXE) */

/*
 * Render `err` as a space-separated description into `buf`, e.g.
 * "not-present write supervisor" or "protection read user ifetch".  Always
 * NUL-terminates when n > 0.  Returns `buf`.
 *
 * Split out from the handler so it can be tested without faulting: the bit
 * decoding is the only part of the handler with logic in it.
 */
const char *fault_describe_err(uint64_t err, char *buf, size_t n);

/* Vector 14 handler.  Called from pf_stub with a pointer to the frame above.
 * Prints the diagnostic and halts; it does not return (except to a TESTING
 * hook that asks it to -- see below). */
void page_fault_handler(exception_frame_t *f);

#ifdef TESTING
/*
 * Test hook.  Called before anything is printed; returning nonzero makes the
 * handler return instead of halting, which lets a test recover from a
 * deliberate fault by pointing f->rip at a fixup label.  Test builds only --
 * a shipped kernel has no way to resume from a page fault, because there is
 * no policy yet for what resuming would mean (SCRUM-47/48).
 */
typedef int (*fault_hook_t)(exception_frame_t *f, uint64_t cr2);
void fault_set_hook(fault_hook_t h);
#endif

#endif /* FAULT_H */
