/*
 * boot.s — Multiboot 2 entry, 32→64 trampoline, long mode setup.
 *
 * GRUB loads us in 32-bit protected mode.  We build identity-mapped page
 * tables covering the first 4 GB (2 MB pages), flip on long mode, load a
 * 64-bit GDT, and far-jump to _start64 which calls kernel_main(mb2_info).
 */

/* ── Multiboot 2 header ─────────────────────────────────────────────────── */

.set MB2_MAGIC,     0xE85250D6
.set MB2_ARCH_X86,  0
.set MB2_BOOT_MAGIC, 0x36D76289   /* magic GRUB passes back in EAX */
.set MB2_HLEN,      multiboot2_header_end - multiboot2_header
.set MB2_CHECKSUM,  -(MB2_MAGIC + MB2_ARCH_X86 + MB2_HLEN)

.section .multiboot2, "a"
.align 8
multiboot2_header:
    .long MB2_MAGIC
    .long MB2_ARCH_X86
    .long MB2_HLEN
    .long MB2_CHECKSUM

    /* ── Framebuffer request tag (type 5) ─────────────────────────────── */
    .align 8
    .short 5                    /* type  */
    .short 0                    /* flags (optional — not required) */
    .long  20                   /* size  */
    .long  1024                 /* width */
    .long  768                  /* height */
    .long  32                   /* depth */

    /* ── End tag ──────────────────────────────────────────────────────── */
    .align 8
    .short 0                    /* type  */
    .short 0                    /* flags */
    .long  8                    /* size  */
multiboot2_header_end:


/* ── BSS: stack + page tables ────────────────────────────────────────── */

.section .bss
.align 16
stack_bottom:
    .skip 16384
stack_top:

/* Page tables — each must be 4 KB-aligned, 4 KB in size */
.align 4096
pml4:   .skip 4096
pdpt:   .skip 4096
pd0:    .skip 4096              /* 0 GB – 1 GB  */
pd1:    .skip 4096              /* 1 GB – 2 GB  */
pd2:    .skip 4096              /* 2 GB – 3 GB  */
pd3:    .skip 4096              /* 3 GB – 4 GB  */


/* ── 64-bit GDT ──────────────────────────────────────────────────────── */

.section .rodata
.align 16
.global gdt64
gdt64:
    .quad 0                         /* 0x00: null descriptor              */

    /* 0x08: 64-bit kernel code — L=1, D=0, P=1, DPL=0, type=exec/read  */
    .quad 0x00AF9A000000FFFF

    /* 0x10: kernel data — P=1, DPL=0, type=read/write                   */
    .quad 0x00CF92000000FFFF

    /*
     * ── Ring 3 descriptors (SCRUM-32, nominally SCRUM-45) ─────────────
     *
     * The order below is not a style choice — `sysretq` computes its
     * selectors from IA32_STAR[63:48] rather than loading them from a
     * register:
     *
     *     SS = STAR[63:48] + 8      CS = STAR[63:48] + 16
     *
     * With STAR[63:48] = 0x18 that fixes user data at 0x20 and user
     * code64 at 0x28, and it means the 32-bit user code descriptor at
     * 0x18 must exist as a placeholder even though long mode never
     * loads it.  Reordering these three, or closing the 0x18 gap,
     * silently breaks every syscall return.  See docs/syscall_spec.md
     * §3.4.
     *
     * `syscall` uses the other half the same way — CS = STAR[47:32] and
     * SS = STAR[47:32] + 8 — which the existing 0x08/0x10 pair already
     * satisfies.
     */

    /* 0x18: user code32 — DPL=3.  Placeholder: required only so the
     * arithmetic above lands on the two descriptors that follow. */
    .quad 0x00CFFA000000FFFF

    /* 0x20: user data — P=1, DPL=3, type=read/write                     */
    .quad 0x00CFF2000000FFFF

    /* 0x28: 64-bit user code — L=1, D=0, P=1, DPL=3, type=exec/read     */
    .quad 0x00AFFA000000FFFF

    /*
     * 0x30: TSS descriptor (SCRUM-46).  A 64-bit TSS descriptor is a system
     * descriptor, twice the width of the code/data descriptors above — it
     * occupies this slot *and* the one at 0x38, base+limit spread across
     * both quads (Intel SDM 3A §7.2.3).  The two are left zeroed here
     * because the descriptor encodes the TSS structure's address, which
     * this file has no way to know — src/tss.c's tss_init() patches both
     * quads at runtime (see gdt64 being .global above) and then `ltr`s the
     * selector.  Until tss_init() runs, TR is null and any exception taken
     * at CPL 3 triple-faults for want of an RSP0 to switch to.
     */
    .quad 0x0000000000000000   /* 0x30: low half   */
    .quad 0x0000000000000000   /* 0x38: high half  */
gdt64_end:

gdt64_ptr:
    .short gdt64_end - gdt64 - 1   /* limit                              */
    .quad  gdt64                    /* base (64-bit)                      */

/*
 * MXCSR's architectural power-on value (Intel SDM 1 §10.2.3): round-to-
 * nearest, flush-to-zero and denormals-are-zero off, and all six SIMD
 * floating-point exceptions *masked*.  Loaded by _start64 -- see the
 * ldmxcsr there for why inheriting whatever firmware left is not good
 * enough once CR4.OSXMMEXCPT is set.
 */
.align 4
mxcsr_init:
    .long 0x00001F80


/* ── 32-bit entry point ──────────────────────────────────────────────── */

.section .text
.code32
.global _start
.type _start, @function
_start:
    /* EAX = multiboot2 magic, EBX = info struct physical address */

    /* ── Verify GRUB passed the Multiboot2 magic ─────────────────── */
    /* No serial/console yet this early — an untrusted EAX means EBX  */
    /* may be garbage, so on mismatch just hang safely instead of     */
    /* dereferencing it later in kernel_main.                         */
    cmp $MB2_BOOT_MAGIC, %eax
    jne .Lhang

    mov %ebx, %edi              /* save MB2 info pointer in EDI         */

    /*
     * ── Page-table flag selection (SCRUM-32) ──────────────────────
     *
     * PT_LINK flags the PML4/PDPT links, PT_LEAF the 2 MB PDEs.
     *
     * The U/S bit must be set at *every* level for ring-3 code to
     * execute at all, so the ring-3 syscall test in
     * tests/kernel/test_syscall_k.c cannot run without it.  Setting it
     * makes the whole identity map ring-3 readable and writable, which
     * is precisely the hole SCRUM-48 (per-LibOS page directories) and
     * SCRUM-55/-56 (isolation tests) exist to close — so it is gated on
     * RING3_PROBE, which docker/scripts/build.sh defines only for
     * TESTING=1 builds.  A normal boot keeps supervisor-only pages.
     *
     * SCRUM-47 removes the gate when it launches a real LibOS in ring 3
     * against page tables that are not the kernel's own.
     */
.ifdef RING3_PROBE
    .set PT_LINK, 0x07              /* present | writable | user       */
    .set PT_LEAF, 0x87              /* present | writable | user | PS  */
.else
    .set PT_LINK, 0x03              /* present | writable              */
    .set PT_LEAF, 0x83              /* present | writable | PS         */
.endif

    /*
     * Export both flag words as absolute symbols so build.sh can assert
     * which variant was assembled (see its "[1b/6] Verify page-table
     * protection" step).  Without that check the gate is enforced only by
     * convention: nothing would fail if a future edit passed TESTING through
     * to a shipped build, and a kernel with ring-3-accessible kernel memory
     * would ship quietly.
     *
     * `.set` on a .global makes these ABS entries in the symbol table — they
     * occupy no space in the image, and their value is the constant actually
     * used above rather than a restatement of it that could drift.
     */
    .global boot_pt_link_flags
    .global boot_pt_leaf_flags
    .set boot_pt_link_flags, PT_LINK
    .set boot_pt_leaf_flags, PT_LEAF

    /* ── Zero page-table memory (6 pages × 4096 bytes = 24 KiB) ───── */
    mov $pml4, %eax
    mov $6144, %ecx             /* 6 pages × 4096 / 4 = 6144 dwords    */
.Lzero_pt:
    movl $0, (%eax)
    add  $4, %eax
    dec  %ecx
    jnz  .Lzero_pt

    /* ── PML4[0] → PDPT ──────────────────────────────────────────── */
    mov $pdpt, %eax
    or  $PT_LINK, %eax
    movl %eax, pml4

    /* ── PDPT[0..3] → PD0..PD3 ───────────────────────────────────── */
    mov $pd0, %eax
    or  $PT_LINK, %eax
    movl %eax, pdpt + 0

    mov $pd1, %eax
    or  $PT_LINK, %eax
    movl %eax, pdpt + 8

    mov $pd2, %eax
    or  $PT_LINK, %eax
    movl %eax, pdpt + 16

    mov $pd3, %eax
    or  $PT_LINK, %eax
    movl %eax, pdpt + 24

    /* ── Fill PD0–PD3: 512 entries each, 2 MB pages ──────────────── */
    mov $pd0, %ebx
    xor %ecx, %ecx             /* physical page counter (0..2047)      */
.Lfill_pd:
    mov %ecx, %eax
    shl $21, %eax              /* EAX = page number × 2 MB             */
    or  $PT_LEAF, %eax         /* flags selected above                 */
    movl %eax, (%ebx)
    movl $0, 4(%ebx)           /* high 32 bits = 0 (< 4 GB)           */
    add  $8, %ebx
    inc  %ecx
    cmp  $2048, %ecx
    jb   .Lfill_pd

    /* ── Load PML4 into CR3 ──────────────────────────────────────── */
    mov $pml4, %eax
    mov %eax, %cr3

    /* ── Enable PAE (CR4 bit 5) ──────────────────────────────────── */
    mov %cr4, %eax
    or  $0x20, %eax             /* bit 5 = PAE                         */
    mov %eax, %cr4

    /* ── Set EFER.LME (MSR 0xC0000080, bit 8) ───────────────────── */
    mov $0xC0000080, %ecx
    rdmsr
    or  $0x100, %eax            /* bit 8 = LME                         */
    wrmsr

    /* ── Enable paging (CR0.PG bit 31) — activates long mode ─────── */
    mov %cr0, %eax
    or  $0x80000000, %eax       /* bit 31 = PG                         */
    mov %eax, %cr0

    /* ── Load 64-bit GDT and far-jump to 64-bit code ────────────── */
    lgdt gdt64_ptr
    ljmp $0x08, $_start64

.Lhang:
    cli
1:  hlt
    jmp 1b

/* ── 64-bit entry ──────────────────────────────────────────────────── */
.code64
_start64:
    /* Set data segment registers */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Set up 64-bit stack */
    mov $stack_top, %rsp

    /*
     * ── Enable SSE (SCRUM-177) ────────────────────────────────────────
     *
     * Doom needs SSE and cannot be talked out of it: the x86_64 SysV ABI
     * returns `float` in %xmm0, so src/doom/m_config.c's
     * M_GetFloatVariable() has no SSE-free encoding at all (-msoft-float
     * and -mfpmath=387 both still hit the ABI).  docker/scripts/build-doom.sh
     * therefore drops -mno-sse for the engine, which additionally lets GCC
     * inline struct copies and zeroing as movaps/movdqa/pxor across the whole
     * of it -- roughly 143 instructions on top of the 18 that are actual
     * float arithmetic.  Both classes #UD identically without the bits below.
     *
     * What the CPU wants before an SSE instruction will execute at all
     * (Intel SDM 3A §13.1.4, "Initialization of the SSE Extensions"):
     *
     *   CR0.EM = 0   Emulation off.  EM=1 means "no FPU, trap it all to
     *                software" and raises #UD on every SSE instruction --
     *                this is the bit that would fault Doom on its first
     *                float.
     *   CR0.MP = 1   Monitor coprocessor.  Only changes how WAIT/FWAIT reads
     *                TS, but MP=1 with EM=0 is the documented "a real FPU is
     *                present" pair.
     *   CR0.TS = 0   No pending lazy-FPU switch.  Already 0 out of reset and
     *                nothing here ever sets it (this kernel has no lazy
     *                switch scheme), so clearing it states the invariant
     *                rather than fixing anything: leave TS set and the first
     *                SSE instruction takes #NM instead of running.
     *   CR0.NE = 1   Report x87 errors as #MF (vector 16) instead of through
     *                the legacy FERR#/IRQ13 pin, which nothing in this kernel
     *                wires up.
     *   CR4.OSFXSR = 1      The OS is prepared to manage FXSAVE/FXRSTOR-shaped
     *                       state, and SSE instructions are enabled.
     *   CR4.OSXMMEXCPT = 1  An unmasked SIMD FP exception arrives as #XM
     *                       (vector 19) rather than as #UD.
     *
     * This sits in the 64-bit half rather than next to the CR4.PAE write in
     * the 32-bit stub on purpose: long mode is already active here, and
     * AMD64 *requires* SSE2 and FXSAVE/FXRSTOR of any CPU able to enter it.
     * "Does this CPU have SSE?" is therefore already answered by the far jump
     * that got us to this label, which is why there is no CPUID gate below --
     * a check that cannot fail is not a check.
     *
     * None of the kernel's own objects use any of this: build.sh keeps
     * -mno-sse -mno-sse2 -mno-mmx on every src/*.c, which is precisely why
     * neither syscall_entry.s nor isr.s saves any FPU/XMM state.  See
     * docs/syscall_spec.md §3.4a for that decision, the invariant it rests
     * on, and what would invalidate it.
     */
.set CR0_MP,         (1 << 1)
.set CR0_EM,         (1 << 2)
.set CR0_TS,         (1 << 3)
.set CR0_NE,         (1 << 5)
.set CR4_OSFXSR,     (1 << 9)
.set CR4_OSXMMEXCPT, (1 << 10)

    mov  %cr0, %rax
    and  $~(CR0_EM | CR0_TS), %rax
    or   $(CR0_MP | CR0_NE), %rax
    mov  %rax, %cr0

    mov  %cr4, %rax
    or   $(CR4_OSFXSR | CR4_OSXMMEXCPT), %rax
    mov  %rax, %cr4

    /*
     * Put x87 and SSE into a known control state instead of inheriting
     * whatever GRUB left behind.  Both must come *after* the CR0 write
     * above: fninit itself raises #NM while TS is set and #UD while EM is.
     *
     * The ldmxcsr is not housekeeping.  MXCSR's six exception-mask bits are
     * all set out of reset, but with OSXMMEXCPT now on, an *unmasked* SIMD
     * exception would be delivered on vector 19 -- where idt_init()
     * (src/idt.c) leaves default_stub, a bare `iretq` that returns to the
     * faulting instruction and so faults forever.  Loading the architectural
     * default makes that vector unreachable rather than trusting firmware
     * not to have touched MXCSR.  Anything that later unmasks a SIMD
     * exception owes vector 19 a real handler first.
     */
    fninit
    ldmxcsr mxcsr_init(%rip)

    /* EDI already holds the MB2 info pointer from the 32-bit stub.
       Zero-extend it to RDI (upper 32 bits already 0 from mov in 32-bit). */
    mov %edi, %edi              /* zero-extend EDI → RDI                */

    call kernel_main

    cli
1:  hlt
    jmp 1b

.size _start, . - _start
