/*
 * fault.c — page fault (vector 14) diagnostic handler (SCRUM-17).
 *
 * See fault.h for the frame layout and why this exists.  The handler is
 * deliberately dependency-light: serial and vmm only, no framebuffer console.
 * It has to work at any point after serial_init(), including during
 * page_alloc_init()/vmm_init(), long before the console exists.
 */

#include "fault.h"
#include "context.h"
#include "fault_record.h"
#include "serial.h"
#include "shell_launch.h"
#include "syscall_exit.h"
#include "vmm.h"

/* Append as much of `s` as fits.  *pos is the write cursor; on return it is
 * the length written so far (never >= n, so there is always room for NUL). */
static void append(char *buf, size_t n, size_t *pos, const char *s)
{
    while (*s && *pos + 1 < n)
        buf[(*pos)++] = *s++;
}

const char *fault_describe_err(uint64_t err, char *buf, size_t n)
{
    size_t pos = 0;

    if (n == 0)
        return buf;

    append(buf, n, &pos, (err & PF_ERR_PRESENT) ? "protection" : "not-present");
    append(buf, n, &pos, (err & PF_ERR_WRITE)   ? " write"     : " read");
    append(buf, n, &pos, (err & PF_ERR_USER)    ? " user"      : " supervisor");

    if (err & PF_ERR_RESERVED)
        append(buf, n, &pos, " reserved-bit");
    if (err & PF_ERR_IFETCH)
        append(buf, n, &pos, " ifetch");

    buf[pos] = '\0';
    return buf;
}

static inline uint64_t read_cr2(void)
{
    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

static void halt_forever(void)
{
    serial_flush();
    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* Set for as long as a fault is being handled, so a fault raised while
 * reporting one is caught rather than recursing until the stack runs out.
 * The hook path clears it again on the way out (see page_fault_handler). */
static int in_fault = 0;

#ifdef TESTING
static fault_hook_t test_hook = 0;

/* The RIP the last hook-driven resume aimed at.  If the next fault arrives
 * *at* that address, resuming made no progress -- the resume target itself
 * faults -- and resuming again would loop forever with nothing on the wire.
 * Recording it lets that case fall through to the full diagnostic instead. */
static uint64_t last_resume_rip = 0;

void fault_set_hook(fault_hook_t h)
{
    test_hook = h;
    last_resume_rip = 0;
}
#endif

/* What the log and the shell call each CPU exception (SCRUM-283): `why` is
 * the plain-language reason a ring-3 program was stopped, `reason` the
 * exo_fault_info reason code the shell turns into wording of its own. */
static const char *exception_name(uint64_t vector, const char **why,
                                  uint32_t *reason)
{
    *reason = EXO_FAULT_REASON_OTHER;
    switch (vector) {
    case 0:  *why = "divided by zero";                *reason = EXO_FAULT_REASON_ARITHMETIC; return "DIVIDE ERROR (#DE)";
    case 1:  *why = "unexpected debug trap";          return "DEBUG (#DB)";
    case 3:  *why = "unexpected breakpoint";          return "BREAKPOINT (#BP)";
    case 4:  *why = "arithmetic overflow trap";       *reason = EXO_FAULT_REASON_ARITHMETIC; return "OVERFLOW (#OF)";
    case 5:  *why = "bound range exceeded";           *reason = EXO_FAULT_REASON_MEMORY;     return "BOUND RANGE (#BR)";
    case 6:  *why = "illegal instruction";            *reason = EXO_FAULT_REASON_ILLEGAL;    return "INVALID OPCODE (#UD)";
    case 7:  *why = "floating-point unit unavailable"; *reason = EXO_FAULT_REASON_ILLEGAL;   return "DEVICE NOT AVAILABLE (#NM)";
    case 8:  *why = "double fault";                   return "DOUBLE FAULT (#DF)";
    case 10: *why = "invalid task state";             return "INVALID TSS (#TS)";
    case 11: *why = "segment not present";            *reason = EXO_FAULT_REASON_MEMORY;     return "SEGMENT NOT PRESENT (#NP)";
    case 12: *why = "stack fault";                    *reason = EXO_FAULT_REASON_MEMORY;     return "STACK FAULT (#SS)";
    case 13: *why = "privileged or illegal operation"; *reason = EXO_FAULT_REASON_PRIVILEGED; return "GENERAL PROTECTION FAULT (#GP)";
    case 14: *why = "illegal memory access";          *reason = EXO_FAULT_REASON_MEMORY;     return "PAGE FAULT (#PF)";
    case 16: *why = "floating-point error";           *reason = EXO_FAULT_REASON_ARITHMETIC; return "x87 FLOATING POINT (#MF)";
    case 17: *why = "misaligned memory access";       *reason = EXO_FAULT_REASON_MEMORY;     return "ALIGNMENT CHECK (#AC)";
    case 18: *why = "machine check";                  return "MACHINE CHECK (#MC)";
    case 19: *why = "floating-point error";           *reason = EXO_FAULT_REASON_ARITHMETIC; return "SIMD FLOATING POINT (#XM)";
    case 20: *why = "virtualization exception";       return "VIRTUALIZATION (#VE)";
    case 21: *why = "control-flow protection fault";  return "CONTROL PROTECTION (#CP)";
    default: *why = "unexpected processor exception"; return "EXCEPTION";
    }
}

/* Nothing left to hand the machine to, and no way to make something. */
static void no_survivor(const char *msg)
{
    serial_print("  ");
    serial_print(msg);
    serial_print(" -- halting\n");
    serial_print("=== halted ===\n");
    halt_forever();
}

/* What to run when the faulting program was the last ready context: build a
 * fresh READY context and return its id, or PAGE_OWNER_FREE if that failed.
 * Production relaunches the shell; tests substitute their own. */
static page_owner_t relaunch_shell(void)
{
    page_owner_t id;
    return shell_launch_context(&id) == 0 ? id : PAGE_OWNER_FREE;
}

static fault_relaunch_fn relaunch = relaunch_shell;

#ifdef TESTING
void fault_set_relaunch(fault_relaunch_fn fn)
{
    relaunch = fn ? fn : relaunch_shell;
}
#endif

void fault_terminate_current(const exception_frame_t *f, uint64_t vector,
                             uint64_t addr)
{
    serial_print("  context:   ");

    /* CPL comes from the low two bits of the saved CS. A fault at CPL 0 is a
     * kernel bug: nothing to terminate, nothing safe to resume. */
    if ((f->cs & 3) != 3) {
        serial_print("ring 0 (kernel) -- fatal\n");
        serial_print("=== halted ===\n");
        halt_forever();
    }

    /* Only a context with a real table row can be torn down and switched
     * away from; the boot-time PAGE_OWNER_LIBOS default (a LibOS entered
     * directly, with no row) has neither a register save area nor a row for
     * context_switch_request() to find, so it keeps the old behaviour. */
    page_owner_t owner = context_current();
    context_t *ctx = context_lookup(owner);
    if (ctx == NULL) {
        serial_print("ring 3 (LibOS) with no context row -- cannot terminate, halting\n");
        serial_print("=== halted ===\n");
        halt_forever();
    }

    const char *why;
    uint32_t reason;
    (void)exception_name(vector, &why, &reason);

    serial_print("ring 3 (LibOS) id ");
    serial_print_hex64(owner);
    serial_print(" \"");
    serial_print(ctx->name);
    serial_print("\" -- terminating: ");
    serial_print(why);
    serial_print("\n");

    /* Recorded before the teardown: the name lives in the row, and the shell
     * reads this after the row may be gone (exo_fault_info, #41). */
    fault_record_add(owner, ctx->name, (uint32_t)vector, reason, f->rip, addr);

    /* The same reclaim exo_exit runs (SCRUM-155), so the two cannot drift. */
    if (context_teardown(owner) != 0) {
        serial_print("  teardown refused -- halting\n");
        serial_print("=== halted ===\n");
        halt_forever();
    }

    /* The outgoing context is dead, so there is nothing to capture: arm the
     * switch only to fill in context_switch_in_*, then resume without the
     * syscall-path capture (context_resume_noreturn, src/context_switch.s).
     * context_switch_request() marks the outgoing row READY; override it as
     * sys_exit does (SCRUM-111) so round-robin never re-enters freed pages. */
    page_owner_t next = context_next_ready(owner);
    int relaunched = 0;
    if (next == PAGE_OWNER_FREE) {
        /* The crashed program was the last one running: bring the shell back
         * rather than leave a machine with nothing to show or type into. */
        serial_print("  no other program is ready -- relaunching the shell\n");
        next = relaunch();
        if (next == PAGE_OWNER_FREE)
            no_survivor("could not relaunch the shell");
        relaunched = 1;
    }
    if (context_switch_request(next) != CONTEXT_OK)
        no_survivor("could not switch to the next context");
    context_set_state(owner, CONTEXT_STATE_BLOCKED);

    if (relaunched) {
        /* Nothing will launch over this row, so reap it here: the switch
         * request is armed and context_resume_noreturn() never reads the
         * outgoing row. Leave the dead address space first -- destroying the
         * page tables CR3 still points at would pull them out from under us. */
        vmm_switch_address_space(vmm_kernel_pml4());
        context_destroy(owner);
    }

    serial_print("  resuming context id ");
    serial_print_hex64(next);
    serial_print("\n");

    /* Handled: a later fault is a new event, not a fault inside this one. */
    in_fault = 0;
    context_resume_noreturn();
}

/* Report what the live tables say about the faulting address.  This is what
 * separates "faulted at X" from "faulted at X, which is unmapped" versus
 * "...which is mapped but supervisor-only" -- the distinction that will matter
 * once a LibOS runs in ring 3 (SCRUM-48/55/56). */
static void print_mapping(uint64_t cr2)
{
    uint64_t paddr = 0, flags = 0;

    serial_print("  mapping:   ");

    if (!vmm_is_active()) {
        serial_print("unknown (kernel map not loaded; still on the boot map)\n");
        return;
    }

    if (vmm_translate(cr2, &paddr, &flags) != VMM_OK) {
        serial_print("none (no present entry along the walk)\n");
        return;
    }

    serial_print("phys 0x");
    serial_print_hex64(paddr);
    serial_print(" flags 0x");
    serial_print_hex64(flags);
    serial_print(flags & VMM_WRITE ? " write" : " read-only");
    serial_print(flags & VMM_USER  ? " user"  : " supervisor");
    serial_print("\n");
}

void page_fault_handler(exception_frame_t *f)
{
    uint64_t cr2 = read_cr2();

    /* Guard first, so it covers the hook below as well as the reporting
     * path.  Say so once, with no further table walks, and stop. */
    if (in_fault) {
        serial_print("\n#PF while handling #PF -- halting\n");
        halt_forever();
    }
    in_fault = 1;

#ifdef TESTING
    if (test_hook && f->rip != last_resume_rip && test_hook(f, cr2)) {
        /* The hook has rewritten f->rip; remember where it is sending us so
         * a resume target that faults in turn is reported rather than
         * resumed again. */
        last_resume_rip = f->rip;
        in_fault = 0;
        return;
    }
#endif

    char desc[64];

    serial_print("\n=== PAGE FAULT (#PF, vector 14) ===\n");

    serial_print("  cr2:       0x");
    serial_print_hex64(cr2);
    serial_print("\n");

    serial_print("  error:     0x");
    serial_print_hex64(f->error_code);
    serial_print("  (");
    serial_print(fault_describe_err(f->error_code, desc, sizeof desc));
    serial_print(")\n");

    serial_print("  rip:       0x");
    serial_print_hex64(f->rip);
    serial_print("\n");

    serial_print("  cs:rsp:    0x");
    serial_print_hex64(f->cs);
    serial_print(":0x");
    serial_print_hex64(f->rsp);
    serial_print("\n");

    serial_print("  rflags:    0x");
    serial_print_hex64(f->rflags);
    serial_print("\n");

    print_mapping(cr2);

    /* A ring-3 fault ends that program and carries on (SCRUM-283); a CPL 0
     * fault is a kernel bug and halts. Either way the diagnostic above has
     * already been printed. */
    fault_terminate_current(f, 14, cr2);

    /* Not reached: fault_terminate_current() halts or switches away. */
    halt_forever();
}

void gp_fault_handler(exception_frame_t *f)
{
    /* Guard first, so it covers the hook below as well as the reporting
     * path -- same reasoning as page_fault_handler's guard. */
    if (in_fault) {
        serial_print("\n#GP while handling a fault -- halting\n");
        halt_forever();
    }
    in_fault = 1;

#ifdef TESTING
    if (test_hook && f->rip != last_resume_rip && test_hook(f, 0)) {
        last_resume_rip = f->rip;
        in_fault = 0;
        return;
    }
#endif

    serial_print("\n=== GENERAL PROTECTION FAULT (#GP, vector 13) ===\n");

    serial_print("  error:     0x");
    serial_print_hex64(f->error_code);
    serial_print("  (0 unless tied to a specific selector)\n");

    serial_print("  rip:       0x");
    serial_print_hex64(f->rip);
    serial_print("\n");

    serial_print("  cs:rsp:    0x");
    serial_print_hex64(f->cs);
    serial_print(":0x");
    serial_print_hex64(f->rsp);
    serial_print("\n");

    serial_print("  rflags:    0x");
    serial_print_hex64(f->rflags);
    serial_print("\n");

    /* See page_fault_handler: ring 3 is terminated, ring 0 halts. */
    fault_terminate_current(f, 13, 0);

    /* Not reached: fault_terminate_current() halts or switches away. */
    halt_forever();
}

void exception_handler(exception_frame_t *f, uint64_t vector)
{
    if (in_fault) {
        serial_print("\nexception while handling a fault -- halting\n");
        halt_forever();
    }
    in_fault = 1;

    const char *why;
    uint32_t reason;
    const char *name = exception_name(vector, &why, &reason);

    serial_print("\n=== ");
    serial_print(name);
    serial_print(", vector ");
    serial_print_hex64(vector);
    serial_print(" ===\n");

    serial_print("  error:     0x");
    serial_print_hex64(f->error_code);
    serial_print("\n  rip:       0x");
    serial_print_hex64(f->rip);
    serial_print("\n  cs:rsp:    0x");
    serial_print_hex64(f->cs);
    serial_print(":0x");
    serial_print_hex64(f->rsp);
    serial_print("\n  rflags:    0x");
    serial_print_hex64(f->rflags);
    serial_print("\n");

    /* #DF and #MC are not something to resume from whichever ring raised
     * them: the CPU state is no longer trustworthy. */
    if (vector == 8 || vector == 18) {
        serial_print("  context:   abort-class exception -- fatal\n");
        serial_print("=== halted ===\n");
        halt_forever();
    }

    fault_terminate_current(f, vector, 0);
}
