.global idt_load
idt_load:
    mov 4(%esp), %eax
    lidt (%eax)
    ret

/* Default handler for unregistered vectors — just return silently.
   No EOI is sent here; this stub covers CPU exception vectors (0-31)
   where EOI is not appropriate. Hardware IRQs should have dedicated stubs. */
.global default_stub
default_stub:
    iret

.global irq0_stub
.extern irq0_handler

irq0_stub:
    pusha
    call irq0_handler
    popa
    iret