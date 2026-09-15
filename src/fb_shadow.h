#pragma once
#include <stdint.h>

#include "page_alloc.h"   /* page_owner_t */
#include "exo_syscall.h"  /* exo_fb_info_t */

/*
 * fb_shadow.h — per-context virtual framebuffers (SCRUM-112).
 *
 * exo_fb_acquire (src/syscall_fb.c) used to hand out the *real* hardware
 * framebuffer's physical address, exclusively (src/fb_binding.c,
 * SCRUM-154) — a second caller got -EXO_EBUSY, and switching between two
 * LibOS instances that both drew to that one physical buffer left whichever
 * one drew last on screen, not whichever one was actually running (the
 * SCRUM-112 bug). This module replaces that: every context that acquires
 * gets its own private, RAM-backed surface matching the real framebuffer's
 * geometry, and can hold it for as long as it lives — no exclusivity, no
 * -EXO_EBUSY. src/fb_compositor.c is what makes any of these actually
 * visible: it copies whichever context is context_current() onto the real
 * hardware framebuffer on a throttled PIT tick. Neither shell_main.c nor
 * the WAD viewer need to know the difference — they still get a
 * phys_addr + geometry to map with exo_page_map, exactly as before.
 *
 * Sized to VMM_MAX_ADDRESS_SPACES (src/vmm.h) rather than context.c's own
 * CONTEXT_MAX: PAGE_OWNER_LIBOS is bound directly in vmm.c's registry
 * outside the context table (see context.h's CONTEXT_MAX comment) but can
 * still call exo_fb_acquire like any other context, so this table needs
 * room for it too. A page_owner_t cannot hold a shadow buffer without an
 * address space to back the pages it maps them into, so
 * VMM_MAX_ADDRESS_SPACES is a real bound, not an arbitrary one.
 *
 * The pages themselves are ordinary PMM pages tagged owned by `who` (see
 * alloc_pages_contig_owned(), src/page_alloc.h) — freeing them is not this
 * module's job. The existing generic sweep (page_reclaim_all(), what
 * revoke_all() already calls) reclaims them exactly like any other page a
 * context owns. fb_shadow_release() only clears this directory's own entry,
 * so a stale slot cannot outlive its context or be handed to whichever new
 * context reuses that id later.
 */

/* fb_shadow_acquire() outcomes, mirroring the -EXO_E* codes syscall_fb.c
 * reports (ABI-agnostic here for the same reason src/fb_binding.h is). */
#define FB_SHADOW_OK      0    /* *info_out filled                            */
#define FB_SHADOW_ENODEV (-1)  /* no real framebuffer published               */
#define FB_SHADOW_ENOMEM (-2)  /* no contiguous run of that size is free      */

/*
 * Give `who` its own shadow framebuffer, allocating one on first call and
 * returning the same buffer (FB_SHADOW_OK, same phys_addr) on every later
 * call from the same `who` — the same idempotent-reacquire contract
 * exo_fb_acquire already documents. Geometry is read from
 * fb_binding_geometry() (src/fb_binding.h), the one published source of
 * truth for the real framebuffer's dimensions; FB_SHADOW_ENODEV if that is
 * NULL. The buffer is zeroed before being handed back, so the first frame a
 * newly-launched context is composited from is blank rather than whatever
 * garbage the PMM pages last held.
 */
int fb_shadow_acquire(page_owner_t who, exo_fb_info_t *info_out);

/*
 * Drop `who`'s directory entry. A no-op if `who` never acquired one, so exit
 * and revocation paths can call it unconditionally. Does not free the
 * underlying pages — see this header's own top comment.
 */
void fb_shadow_release(page_owner_t who);

/*
 * Look up `who`'s shadow buffer for the compositor. Returns 0 and writes
 * *phys_base_out if `who` holds one, nonzero otherwise (never acquired, or
 * already released).
 */
int fb_shadow_lookup(page_owner_t who, uint64_t *phys_base_out);
