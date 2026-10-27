#pragma once

#ifndef __ASSEMBLER__
#include <stdint.h>
#endif

/*
 * exo_kbd_src.h -- keyboard source numbers, the second argument of
 * exo_kbd_bind (#44, SCRUM-297).
 *
 * Split out of exo_syscall.h so src/kbd_source.h can share the public numbers
 * without pulling in the whole syscall ABI. exo_syscall.h includes it.
 */

/* One namespace for the PS/2 keyboard and every USB keyboard; `usb:N` is the
 * keyboard's enumeration index, which can change between boots or ports.
 * EXO_KBD_SRC_NONE unbinds. */
#define EXO_KBD_SRC_NONE      ((uint64_t)-1)
#define EXO_KBD_SRC_PS2       0u
#define EXO_KBD_SRC_USB(n)    (1u + (uint32_t)(n))
