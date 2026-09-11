/*
 * fault.c — page fault (vector 14) diagnostic handler (SCRUM-17).
 *
 * See fault.h for the frame layout and why this exists.  The handler is
 * deliberately dependency-light: serial and vmm only, no framebuffer console.
 * It has to work at any point after serial_init(), including during
 * page_alloc_init()/vmm_init(), long before the console exists.
 */

#include "fault.h"
#include "serial.h"
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

    /* CPL comes from the low two bits of the saved CS.
     *
     * The ring-3 arm is reachable now that tss_init() loads a TSS with a
     * valid RSP0 (SCRUM-46): idt_set_gate's gates are all IST=0, so a CPL 3
     * exception loads that RSP0 rather than faulting for want of one.
     * tests/kernel/test_tss_k.c drives it deliberately; no LibOS runs in
     * ring 3 on a normal boot yet (SCRUM-47), so in practice this still only
     * fires under test.
     *
     * A real LibOS fault still just halts, because there is no policy yet
     * for what should happen instead -- this is where a faulting LibOS gets
     * terminated and its resources reclaimed via revoke_all(), once that
     * policy exists. */
    serial_print("  context:   ");
    if ((f->cs & 3) != 0) {
        serial_print("ring 3 (LibOS) -- no per-context teardown yet, halting\n");
    } else {
        serial_print("ring 0 (kernel) -- fatal\n");
    }

    serial_print("=== halted ===\n");
    halt_forever();
}
