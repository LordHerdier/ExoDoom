/*
 * sse_probe.s — C-callable SSE instructions for the ring-0 half of the SSE
 * suite (SCRUM-177).
 *
 * These exist as hand-written assembly rather than C for one reason: every
 * src/*.c and tests/kernel/*.c in this tree is compiled -mno-sse -mno-sse2
 * -mno-mmx (docker/scripts/build.sh), and that is not going to change --
 * it is the whole basis of the "the kernel cannot clobber a LibOS's XMM
 * state" invariant documented in docs/syscall_spec.md §3.4a.  A test written
 * in C could not name an XMM register, and one that dropped the flag for its
 * own translation unit would be testing a kernel nobody ships.
 *
 * So the instruction under test lives here and the *arguments and results
 * cross the boundary as integers*: a float is passed and returned as its
 * IEEE-754 bit pattern in a GP register, which is representable in -mno-sse
 * C and needs no xmm0 return slot.  tests/kernel/test_sse_k.c holds the
 * expected bit patterns.
 *
 * SysV AMD64 ABI: integer arguments in %rdi/%rsi, integer result in %rax.
 * None of these touch memory beyond what the caller passes in, and all of
 * them are leaf functions, so no stack frame beyond the one explicit
 * sub/add in sse_probe_read_mxcsr (stmxcsr needs an address, not a
 * register).
 */

.code64
.section .text

/* uint64_t sse_probe_read_cr0(void);
 * uint64_t sse_probe_read_cr4(void);
 *
 * Read back what boot.s's _start64 actually programmed.  test_sse_k.c
 * asserts on these *before* executing any SSE instruction: if a future edit
 * drops the enable block, the CR0/CR4 assertions fail cleanly, where an
 * `addss` would instead raise #UD onto idt_init()'s default_stub -- a bare
 * `iretq` back to the faulting instruction, i.e. a silent hang and a CI
 * timeout with no failing assertion.  Order matters in that file. */
.global sse_probe_read_cr0
sse_probe_read_cr0:
    mov %cr0, %rax
    ret

.global sse_probe_read_cr4
sse_probe_read_cr4:
    mov %cr4, %rax
    ret

/* uint32_t sse_probe_read_mxcsr(void);
 *
 * The control word boot.s loaded with ldmxcsr.  Checked because the six
 * exception-mask bits being set is what keeps vector 19 (#XM) unreachable --
 * see boot.s's own comment on why that vector has no real handler. */
.global sse_probe_read_mxcsr
sse_probe_read_mxcsr:
    sub     $8, %rsp
    stmxcsr (%rsp)
    mov     (%rsp), %eax
    add     $8, %rsp
    ret

/* uint32_t sse_probe_addss(uint32_t a_bits, uint32_t b_bits);
 * uint32_t sse_probe_mulss(uint32_t a_bits, uint32_t b_bits);
 *
 * Scalar single-precision arithmetic -- the class src/doom/m_config.c,
 * g_game.c, p_setup.c and v_video.c actually contain (18 instructions
 * between them, per docker/scripts/build-doom.sh).  In and out as bit
 * patterns; movd moves the low 32 bits of an XMM register to/from a GP
 * register without reinterpreting them. */
.global sse_probe_addss
sse_probe_addss:
    movd  %edi, %xmm0
    movd  %esi, %xmm1
    addss %xmm1, %xmm0
    movd  %xmm0, %eax
    ret

.global sse_probe_mulss
sse_probe_mulss:
    movd  %edi, %xmm0
    movd  %esi, %xmm1
    mulss %xmm1, %xmm0
    movd  %xmm0, %eax
    ret

/* uint64_t sse_probe_addsd(uint64_t a_bits, uint64_t b_bits);
 *
 * The SSE2 double-precision counterpart.  Worth its own case because
 * CR4.OSFXSR gates SSE and SSE2 together but the two use different opcode
 * prefixes -- and doomgeneric's DG_* layer takes doubles even where Doom
 * itself is fixed-point. */
.global sse_probe_addsd
sse_probe_addsd:
    movq  %rdi, %xmm0
    movq  %rsi, %xmm1
    addsd %xmm1, %xmm0
    movq  %xmm0, %rax
    ret

/* uint32_t sse_probe_cvtsi2ss(int32_t v);
 * int32_t  sse_probe_cvttss2si(uint32_t bits);
 *
 * Integer <-> float conversion in both directions.  This is the pair Doom
 * reaches through M_GetFloatVariable()/M_SetVariable() in m_config.c, and
 * cvttss2si is also what the compiler emits for any (int) cast of a float. */
.global sse_probe_cvtsi2ss
sse_probe_cvtsi2ss:
    cvtsi2ss %edi, %xmm0
    movd     %xmm0, %eax
    ret

.global sse_probe_cvttss2si
sse_probe_cvttss2si:
    movd      %edi, %xmm0
    cvttss2si %xmm0, %eax
    ret

/* void sse_probe_movaps_copy16(const void *src, void *dst);
 * void sse_probe_pxor_zero16(void *dst);
 *
 * The *other* class, and the larger one: allowing SSE lets GCC inline struct
 * copies and zeroing as movaps/movdqa/pxor across the whole engine (~143
 * instructions, against 18 of real float arithmetic).  Both classes need the
 * same CR0/CR4 bits, so both are covered here rather than assuming one
 * implies the other.
 *
 * movaps/movdqa fault #GP on a memory operand that is not 16-byte aligned;
 * the caller in test_sse_k.c aligns its buffers accordingly. */
.global sse_probe_movaps_copy16
sse_probe_movaps_copy16:
    movaps (%rdi), %xmm0
    movaps %xmm0, (%rsi)
    ret

.global sse_probe_pxor_zero16
sse_probe_pxor_zero16:
    pxor   %xmm0, %xmm0
    movdqa %xmm0, (%rdi)
    ret
