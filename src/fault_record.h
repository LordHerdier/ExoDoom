#ifndef FAULT_RECORD_H
#define FAULT_RECORD_H

#include <stdint.h>

#include "exo_syscall.h"

/*
 * fault_record — the log of programs the kernel ended for a CPU fault
 * (SCRUM-283), read through exo_fault_info (#41).
 *
 * A fixed ring of the last EXO_FAULT_LOG_MAX records, kept apart from the
 * context table on purpose: the row of a program that crashed as the last one
 * running is reaped on the spot (src/fault.c), and the relaunched shell must
 * still be able to say what happened.  Sequence numbers start at 1 and never
 * repeat, so a reader asks for "newer than the last I saw".
 */

/* Append a record, assigning the next seq. `name` may be NULL. */
void fault_record_add(uint64_t ctx_id, const char *name, uint32_t vector,
                      uint32_t reason, uint64_t rip, uint64_t addr);

/* Copy the oldest record with seq > after_seq into *out. Returns 0, or -1 if
 * there is none. */
int fault_record_next(uint64_t after_seq, exo_fault_info_t *out);

/* Highest seq assigned so far (0 = nothing yet). */
uint64_t fault_record_last_seq(void);

#ifdef TESTING
/* Forget everything, including the seq counter. */
void fault_record_reset(void);
#endif

#endif /* FAULT_RECORD_H */
