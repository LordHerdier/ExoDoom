/*
 * test_sse_k.c -- SSE is enabled, works in both rings, and survives kernel
 * entry (SCRUM-177).
 *
 * Doom cannot run without SSE and cannot be compiled without it: the x86_64
 * SysV ABI returns `float` in %xmm0, so src/doom/m_config.c's
 * M_GetFloatVariable() has no SSE-free encoding, and docker/scripts/build-doom.sh
 * drops -mno-sse for the engine accordingly. Until this ticket, nothing set
 * the control-register bits that make those instructions executable, so the
 * first float Doom touched would have raised #UD. src/boot.s's _start64 now
 * sets them; this suite is what says so.
 *
 * ── Why the cases are ordered the way they are ─────────────────────────────
 *
 * Executing an SSE instruction when the bits are NOT set raises #UD on vector
 * 6, where idt_init() (src/idt.c) leaves default_stub -- a bare `iretq`
 * straight back to the faulting instruction, i.e. an infinite loop. That
 * surfaces as a CI timeout with no failing assertion at all, which is the
 * worst signal this tree can produce and exactly the silent-failure mode
 * KUNIT_MAX_SUITES' own comment (src/kunit.h) warns about.
 *
 * So nothing here executes an SSE instruction on faith. Suite init reads
 * CR0/CR4 once -- two instructions, no SSE involved -- the control-register
 * cases run first and assert on what it found, and every case that goes on to
 * execute a real instruction opens with REQUIRE_SSE_ENABLED(). Delete the
 * enable block from boot.s and this suite reports nine ordinary failures and
 * lets the rest of the run finish, instead of hanging the machine.
 *
 * ── Why the SSE instructions live in assembly ──────────────────────────────
 *
 * Every C file in this tree, kernel and test alike, compiles -mno-sse
 * -mno-sse2 -mno-mmx, and that is load-bearing rather than incidental -- see
 * docs/syscall_spec.md §3.4a. C here therefore cannot name an
 * XMM register or write a float literal, so the instructions under test sit
 * in tests/kernel/sse_probe.s and floats cross the boundary as their IEEE-754
 * bit patterns, spelled out below as integer constants.
 *
 * ── What the ring-3 cases add ──────────────────────────────────────────────
 *
 * Doom runs at CPL 3, so ring 0 working proves the wrong thing on its own.
 * tests/kernel/sse_ring3_probe.s executes SSE at CPL 3 and checks all sixteen
 * XMM registers across a real syscall; tests/kernel/sse_irq_probe.s checks
 * them across a real hardware interrupt taken at CPL 3. Together they are the
 * executable form of this ticket's deferral decision -- that neither
 * src/syscall_entry.s nor src/isr.s needs to save FPU/XMM state, because the
 * kernel is incapable of touching it. docs/syscall_spec.md §3.4a is the
 * written form, including what would invalidate it.
 */

#include "kunit.h"
#include "libos_launch.h"
#include "libos_test_common.h"
#include "sse_probe_result.h"
#include "vmm.h"
#include "fault.h"
#include "syscall.h"
#include "pit.h"

#include <stdint.h>

#define SSE_TEST_OWNER TEST_OWNER_SSE

/* ── Control-register bits boot.s programs (Intel SDM 3A §13.1.4) ───────── */
#define CR0_MP          (1ULL << 1)
#define CR0_EM          (1ULL << 2)
#define CR0_TS          (1ULL << 3)
#define CR0_NE          (1ULL << 5)
#define CR4_OSFXSR      (1ULL << 9)
#define CR4_OSXMMEXCPT  (1ULL << 10)

/* MXCSR's architectural default: round-to-nearest, FTZ/DAZ off, and all six
 * SIMD FP exceptions masked. Must match the word boot.s's ldmxcsr loads. */
#define MXCSR_DEFAULT   0x00001F80u
#define MXCSR_ALL_MASKS 0x00001F80u  /* bits 7-12, the six exception masks */

/*
 * IEEE-754 bit patterns, since -mno-sse C cannot write the literals.
 * Single: sign | 8-bit biased exponent | 23-bit fraction.
 */
#define F_1_0    0x3F800000u
#define F_2_0    0x40000000u
#define F_3_0    0x40400000u
#define F_1_5    0x3FC00000u
#define F_2_5    0x40200000u
#define F_3_75   0x40700000u   /* 1.5f * 2.5f */
#define F_42_0   0x42280000u   /* (float)42   */
#define F_NEG7_0 0xC0E00000u   /* (float)-7   */

/* Double: sign | 11-bit biased exponent | 52-bit fraction. */
#define D_1_0 0x3FF0000000000000ULL
#define D_2_0 0x4000000000000000ULL
#define D_3_0 0x4008000000000000ULL

/* tests/kernel/sse_probe.s */
extern uint64_t sse_probe_read_cr0(void);
extern uint64_t sse_probe_read_cr4(void);
extern uint32_t sse_probe_read_mxcsr(void);
extern uint32_t sse_probe_addss(uint32_t a_bits, uint32_t b_bits);
extern uint32_t sse_probe_mulss(uint32_t a_bits, uint32_t b_bits);
extern uint64_t sse_probe_addsd(uint64_t a_bits, uint64_t b_bits);
extern uint32_t sse_probe_cvtsi2ss(int32_t v);
extern int32_t  sse_probe_cvttss2si(uint32_t bits);
extern void     sse_probe_movaps_copy16(const void *src, void *dst);
extern void     sse_probe_pxor_zero16(void *dst);

/* tests/kernel/sse_ring3_probe.s, tests/kernel/sse_irq_probe.s */
extern void sse_ring3_probe(void);
extern void sse_ring3_probe_end(void);
extern void sse_irq_probe(void);
extern void sse_irq_probe_end(void);

extern uint64_t libos_enter_irq(uint64_t entry_vaddr, uint64_t stack_top_vaddr);

/*
 * Read once in suite init, before any case has had the chance to execute an
 * SSE instruction -- see this file's header comment for why that ordering is
 * not optional.  CR0.EM/TS and CR4.OSFXSR are the three that decide whether
 * an instruction executes at all; CR0.MP/NE and CR4.OSXMMEXCPT matter for
 * correctness but cannot turn an `addss` into a #UD, so they are asserted
 * below rather than gated on here.
 */
static int sse_bits_enabled;

int sse_suite_init(void) {
    uint64_t cr0 = sse_probe_read_cr0();
    uint64_t cr4 = sse_probe_read_cr4();

    sse_bits_enabled = (cr0 & CR0_EM) == 0 &&
                       (cr0 & CR0_TS) == 0 &&
                       (cr4 & CR4_OSFXSR) != 0;
    return 0;
}

/* Fail this case and return, rather than executing an instruction that would
 * #UD into an infinite loop.  Opens every case below that runs one. */
#define REQUIRE_SSE_ENABLED()             \
    do {                                  \
        CU_ASSERT_TRUE(sse_bits_enabled); \
        if (!sse_bits_enabled) return;    \
    } while (0)

/* ── Ring 0: the bits themselves ────────────────────────────────────────── */

static void test_cr0_permits_sse(void) {
    uint64_t cr0 = sse_probe_read_cr0();

    /* EM=1 is the one that raises #UD on every SSE instruction -- the exact
     * state this ticket exists to leave behind. */
    CU_ASSERT_EQUAL(cr0 & CR0_EM, 0);

    /* TS=1 would raise #NM instead. Nothing here runs a lazy-FPU scheme, so
     * this asserts an invariant rather than guarding a mechanism. */
    CU_ASSERT_EQUAL(cr0 & CR0_TS, 0);

    CU_ASSERT_NOT_EQUAL(cr0 & CR0_MP, 0);
    CU_ASSERT_NOT_EQUAL(cr0 & CR0_NE, 0);
}

static void test_cr4_enables_sse(void) {
    uint64_t cr4 = sse_probe_read_cr4();

    CU_ASSERT_NOT_EQUAL(cr4 & CR4_OSFXSR, 0);
    CU_ASSERT_NOT_EQUAL(cr4 & CR4_OSXMMEXCPT, 0);
}

static void test_mxcsr_masks_every_simd_exception(void) {
    /* stmxcsr is itself an SSE instruction, so this case needs the guard
     * too, even though it only reads a control word. */
    REQUIRE_SSE_ENABLED();

    uint32_t mxcsr = sse_probe_read_mxcsr();

    CU_ASSERT_EQUAL(mxcsr, MXCSR_DEFAULT);

    /* Stated separately from the exact-value check above because this is the
     * part that is load-bearing: OSXMMEXCPT is set, so an *unmasked* SIMD
     * exception would be delivered on vector 19, which idt_init() leaves on
     * default_stub -- a bare iretq back to the faulting instruction, i.e. a
     * hang. Whoever first unmasks one of these owes vector 19 a handler. */
    CU_ASSERT_EQUAL(mxcsr & MXCSR_ALL_MASKS, MXCSR_ALL_MASKS);
}

/* ── Ring 0: instructions actually execute, and are right ───────────────── */

static void test_scalar_single_arithmetic(void) {
    REQUIRE_SSE_ENABLED();

    CU_ASSERT_EQUAL(sse_probe_addss(F_1_0, F_2_0), F_3_0);
    CU_ASSERT_EQUAL(sse_probe_mulss(F_1_5, F_2_5), F_3_75);
}

static void test_scalar_double_arithmetic(void) {
    REQUIRE_SSE_ENABLED();

    /* SSE2, not SSE. The same CR4.OSFXSR gates both, but they are distinct
     * opcode spaces, and doomgeneric's own interface takes doubles even
     * where Doom proper is fixed-point throughout. */
    CU_ASSERT_EQUAL(sse_probe_addsd(D_1_0, D_2_0), D_3_0);
}

static void test_int_float_conversion_round_trips(void) {
    REQUIRE_SSE_ENABLED();

    CU_ASSERT_EQUAL(sse_probe_cvtsi2ss(42), F_42_0);
    CU_ASSERT_EQUAL(sse_probe_cvtsi2ss(-7), F_NEG7_0);

    /* cvttss2si truncates toward zero rather than rounding -- 3.75f is the
     * case that tells the two apart. */
    CU_ASSERT_EQUAL(sse_probe_cvttss2si(F_3_75), 3);
    CU_ASSERT_EQUAL(sse_probe_cvttss2si(F_NEG7_0), -7);
}

static void test_packed_move_and_zero(void) {
    REQUIRE_SSE_ENABLED();

    /* The larger of the two instruction classes SSE unlocks for the engine:
     * ~143 movaps/movdqa/pxor from inlined struct copies and zeroing, against
     * 18 instructions of real float arithmetic (docker/scripts/build-doom.sh).
     * Both need the same bits, so both are checked rather than one assumed
     * to imply the other.
     *
     * 16-byte aligned because movaps/movdqa raise #GP otherwise. */
    static const uint8_t src[16] __attribute__((aligned(16))) = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
    };
    static uint8_t dst[16] __attribute__((aligned(16)));

    for (int i = 0; i < 16; i++) {
        dst[i] = 0x5A;
    }

    sse_probe_movaps_copy16(src, dst);
    for (int i = 0; i < 16; i++) {
        CU_ASSERT_EQUAL(dst[i], src[i]);
    }

    sse_probe_pxor_zero16(dst);
    for (int i = 0; i < 16; i++) {
        CU_ASSERT_EQUAL(dst[i], 0);
    }
}

/* ── Ring 3 ─────────────────────────────────────────────────────────────── */

static volatile int fault_count;

static int recording_hook(exception_frame_t *f, uint64_t cr2) {
    (void)f; (void)cr2;
    fault_count++;
    return 0;
}

static void test_sse_executes_in_ring3_and_survives_a_syscall(void) {
    REQUIRE_SSE_ENABLED();

    size_t code_len = (uintptr_t)&sse_ring3_probe_end -
                      (uintptr_t)&sse_ring3_probe;

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(SSE_TEST_OWNER,
                                      (const void *)&sse_ring3_probe,
                                      code_len, 0, 0, 0, &img),
                    VMM_OK);

    libos_test_launch_result_t run = libos_test_launch(&img);
    CU_ASSERT_EQUAL(run.switch_in_status, VMM_OK);
    CU_ASSERT_EQUAL(run.switch_out_status, VMM_OK);
    CU_ASSERT_EQUAL(run.fault_count, 0);

    /* Anything other than MARKER + R_OK names the check that failed; see
     * tests/kernel/sse_probe_result.h. A result of 0 would mean the probe
     * never reached libos_return() at all, which is why the marker is not
     * zero. */
    CU_ASSERT_EQUAL(run.result, SSE_PROBE_MARKER + SSE_PROBE_R_OK);

    libos_destroy_image(SSE_TEST_OWNER, &img);
}

static void test_xmm_survives_a_hardware_interrupt_at_cpl3(void) {
    REQUIRE_SSE_ENABLED();

    size_t code_len = (uintptr_t)&sse_irq_probe_end -
                      (uintptr_t)&sse_irq_probe;

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(SSE_TEST_OWNER,
                                      (const void *)&sse_irq_probe,
                                      code_len, 0, 0, 0, &img),
                    VMM_OK);

    /* libos_enter_irq() rather than libos_test_launch()'s libos_enter():
     * this probe needs RFLAGS.IF set on the way in, because an interrupt
     * landing at CPL 3 is the entire subject. Everything else here is
     * libos_test_launch()'s body, done by hand for that one difference --
     * the same thing test_irq_entry_k.c does, and for the same reason. */
    pit_irq0_reset_last_rsp();
    fault_count = 0;
    fault_set_hook(recording_hook);
    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, libos_return);

    CU_ASSERT_EQUAL(vmm_switch_address_space(img.pml4_phys), VMM_OK);
    uint64_t result = libos_enter_irq(img.entry_vaddr, img.stack_top_vaddr);
    CU_ASSERT_EQUAL(vmm_switch_address_space(vmm_kernel_pml4()), VMM_OK);

    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, 0);
    fault_set_hook(0);

    CU_ASSERT_EQUAL(result, SSE_PROBE_MARKER + SSE_PROBE_R_OK);
    CU_ASSERT_EQUAL(fault_count, 0);

    /* The probe only escapes after observing real tick advancement, which
     * only IRQ0 produces -- so interrupts did fire while it held its
     * patterns. irq0_handler's recorded entry RSP is the kernel-side
     * confirmation of the same thing (test_irq_entry_k.c checks that it
     * lands inside the TSS stack; here it only needs to be non-zero, i.e.
     * the handler ran at all). */
    CU_ASSERT_NOT_EQUAL(pit_irq0_last_rsp(), 0);

    libos_destroy_image(SSE_TEST_OWNER, &img);
}

/* Leaves nothing behind even if an assertion above failed mid-test and
 * skipped its own cleanup -- see libos_test_common.h. */
int sse_suite_cleanup(void) {
    pit_irq0_reset_last_rsp();
    libos_test_teardown_owner(SSE_TEST_OWNER);
    return 0;
}

void suite_sse_tests(CU_pSuite s) {
    /* Control registers first -- see this file's header comment. */
    CU_add_test(s, "CR0 permits SSE (EM/TS clear, MP/NE set)",
                test_cr0_permits_sse);
    CU_add_test(s, "CR4 enables SSE (OSFXSR/OSXMMEXCPT set)",
                test_cr4_enables_sse);
    CU_add_test(s, "MXCSR masks every SIMD FP exception",
                test_mxcsr_masks_every_simd_exception);

    CU_add_test(s, "scalar single-precision arithmetic (addss/mulss)",
                test_scalar_single_arithmetic);
    CU_add_test(s, "scalar double-precision arithmetic (addsd)",
                test_scalar_double_arithmetic);
    CU_add_test(s, "int/float conversion round-trips (cvtsi2ss/cvttss2si)",
                test_int_float_conversion_round_trips);
    CU_add_test(s, "packed move and zero (movaps/movdqa/pxor)",
                test_packed_move_and_zero);

    CU_add_test(s, "SSE executes at CPL 3 and XMM survives a syscall",
                test_sse_executes_in_ring3_and_survives_a_syscall);
    CU_add_test(s, "XMM survives a hardware interrupt taken at CPL 3",
                test_xmm_survives_a_hardware_interrupt_at_cpl3);
}
