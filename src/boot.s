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
gdt64:
    .quad 0                         /* 0x00: null descriptor              */

    /* 0x08: 64-bit kernel code — L=1, D=0, P=1, DPL=0, type=exec/read  */
    .quad 0x00AF9A000000FFFF

    /* 0x10: kernel data — P=1, DPL=0, type=read/write                   */
    .quad 0x00CF92000000FFFF
gdt64_end:

gdt64_ptr:
    .short gdt64_end - gdt64 - 1   /* limit                              */
    .quad  gdt64                    /* base (64-bit)                      */


/* ── 32-bit entry point ──────────────────────────────────────────────── */

.section .text
.code32
.global _start
.type _start, @function
_start:
    /* EAX = multiboot2 magic, EBX = info struct physical address */
    mov %ebx, %edi              /* save MB2 info pointer in EDI         */

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
    or  $0x03, %eax             /* present | writable                   */
    movl %eax, pml4

    /* ── PDPT[0..3] → PD0..PD3 ───────────────────────────────────── */
    mov $pd0, %eax
    or  $0x03, %eax
    movl %eax, pdpt + 0

    mov $pd1, %eax
    or  $0x03, %eax
    movl %eax, pdpt + 8

    mov $pd2, %eax
    or  $0x03, %eax
    movl %eax, pdpt + 16

    mov $pd3, %eax
    or  $0x03, %eax
    movl %eax, pdpt + 24

    /* ── Fill PD0–PD3: 512 entries each, 2 MB pages ──────────────── */
    /* Flags: present(0) | writable(1) | page-size(7) = 0x83         */
    mov $pd0, %ebx
    xor %ecx, %ecx             /* physical page counter (0..2047)      */
.Lfill_pd:
    mov %ecx, %eax
    shl $21, %eax              /* EAX = page number × 2 MB             */
    or  $0x83, %eax            /* present | writable | PS (2 MB page)  */
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

    /* EDI already holds the MB2 info pointer from the 32-bit stub.
       Zero-extend it to RDI (upper 32 bits already 0 from mov in 32-bit). */
    mov %edi, %edi              /* zero-extend EDI → RDI                */

    call kernel_main

    cli
1:  hlt
    jmp 1b

.size _start, . - _start
