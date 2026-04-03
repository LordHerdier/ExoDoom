.set ALIGN,        1<<0
.set MEMINFO,      1<<1
.set VIDEO,        1<<2
.set FLAGS,        ALIGN | MEMINFO | VIDEO
.set MAGIC,        0x1BADB002
.set CHECKSUM,     -(MAGIC + FLAGS)

.section .multiboot, "a"
.align 4
.global multiboot_header
multiboot_header:
.long MAGIC
.long FLAGS
.long CHECKSUM

/* reserved (offsets 12-28 unused without AOUT_KLUDGE, kept to preserve VIDEO field offsets) */
.long 0
.long 0
.long 0
.long 0
.long 0

/* graphics fields */
.long 0                  /* mode_type: 0=linear graphics */
.long 1024               /* width */
.long 768                /* height */
.long 32                 /* depth */


.section .bss
.align 16
stack_bottom:
.skip 16384
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    mov $stack_top, %esp
    push %ebx
    call kernel_main

    cli
1:  hlt
    jmp 1b

.size _start, . - _start
