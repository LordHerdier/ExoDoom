#pragma once
#include <stdint.h>

#include "page_alloc.h"   /* page_owner_t */
#include "ps2.h"          /* kbd_event_t */
#include "usb_kbd.h"      /* USB_KBD_MAX */
#include "exo_syscall.h"  /* EXO_KBD_SRC_* */

/*
 * kbd_source.h -- which keyboard feeds which context (SCRUM-199, epic
 * SCRUM-195).
 *
 * Every keyboard is a *source*: the PS/2 ring (src/ps2.c) or one of the USB
 * rings (src/usb_kbd.c, one per keyboard). Both ring kinds are strictly
 * single-consumer (src/kbd_ring.h), so a source is bound to at most one
 * context, and a context reads from at most one source.
 *
 * A context with no binding keeps the original behaviour -- it reads the
 * PS/2 ring -- so the shell, Snake and a lone Doom are untouched. The one
 * exception is a PS/2 ring some other context has bound: an unbound context
 * then reads nothing, since popping it would steal that context's keys.
 *
 * Input is routed by the context making the poll, not by which context is
 * in the foreground: a bound context reads its own keyboard regardless of
 * who is on screen.
 */

/* The numbers are the public ones (EXO_KBD_SRC_*, src/exo_syscall.h). */
#define KBD_SRC_NONE      (-1)
#define KBD_SRC_PS2       ((int)EXO_KBD_SRC_PS2)
#define KBD_SRC_USB(n)    ((int)EXO_KBD_SRC_USB(n))
#define KBD_SRC_MAX       (1 + USB_KBD_MAX)   /* PS/2 + every USB slot */

/* 1 if `src` names a keyboard that exists now (PS/2, or a USB index below
 * usb_kbd_count()). KBD_SRC_NONE is not a source. */
int kbd_source_valid(int src);

/* Bind `src` to `ctx`. 0 on success (also if `ctx` already holds `src`; a
 * context that held a different source gives it up). -EXO_ENOENT for an
 * unknown source, -EXO_EINVAL for PAGE_OWNER_FREE/KERNEL as `ctx`,
 * -EXO_EBUSY if another context holds it, in which case nothing changes.
 * A new binding starts with the source's ring empty: events typed before it
 * (or left by the previous owner) are discarded, so a key-down without its
 * key-up cannot arrive as a stuck key. */
int kbd_source_claim(int src, page_owner_t ctx);

/* Drop whatever `ctx` holds. Safe on a context with nothing bound. */
void kbd_source_release_ctx(page_owner_t ctx);

/* The source `ctx` holds, or KBD_SRC_NONE. */
int kbd_source_of(page_owner_t ctx);

/* The context holding `src`, or PAGE_OWNER_FREE. */
page_owner_t kbd_source_owner(int src);

/* Oldest event `ctx` may read: 1 and *out filled, or 0. This is exo_kbd_poll's
 * routing rule (header comment). */
int kbd_source_poll(page_owner_t ctx, kbd_event_t *out);

/* Forget every binding. TESTING suites call this to start clean. */
void kbd_source_reset(void);
