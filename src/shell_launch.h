#ifndef SHELL_LAUNCH_H
#define SHELL_LAUNCH_H

#include "page_alloc.h"

/*
 * Build a fresh shell LibOS as a READY context (SCRUM-283), without entering
 * it: context_create() + name + libos_build_image() of the embedded shell
 * blobs + context_prime_irq().  The same recipe kernel_main's boot tail runs,
 * minus the boot-console shadow seed (the screen is no longer the boot log)
 * and minus the direct entry -- the caller reaches it with
 * context_switch_request()/context_resume_noreturn().
 *
 * Used by the fault path when the program that crashed was the last ready
 * context (src/fault.c).  Returns 0 and writes the new id to `id_out`, or -1
 * if no context row, address space or pages were available; nothing is left
 * allocated on failure.
 */
int shell_launch_context(page_owner_t *id_out);

#endif
