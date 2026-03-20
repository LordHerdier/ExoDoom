.global idt_load
idt_load:
    mov 4(%esp), %eax
    lidt (%eax)
    ret

.global irq0_stub
.extern irq0_handler

irq0_stub:
    pusha
    call irq0_handler
    popa
    iret