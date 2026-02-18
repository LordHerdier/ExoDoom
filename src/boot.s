.set ALIGN,        1<<0
.set MEMINFO,      1<<1
.set VIDEO,        1<<2
.set AOUT_KLUDGE,  1<<16
.set FLAGS,        ALIGN | MEMINFO | VIDEO | AOUT_KLUDGE
.set MAGIC,        0x1BADB002
.set CHECKSUM,     -(MAGIC + FLAGS)

.section .multiboot, "a"
.align 4
.global multiboot_header
multiboot_header:
.long MAGIC
.long FLAGS
.long CHECKSUM

/* a.out kludge fields */
.long multiboot_header   /* header_addr */
.long _load_start        /* load_addr */
.long _load_end          /* load_end_addr */
.long _bss_end           /* bss_end_addr */
.long _start             /* entry_addr */

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
