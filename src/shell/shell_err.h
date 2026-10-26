/*
 * shell_err.h — plain-language wording for the errors the shell reports
 * (SCRUM-271).
 *
 * Every failing handler used to print a raw `err=-16`. They now all go
 * through shell_err_text(), so one errno reads the same wherever it surfaces.
 * Nothing here touches the console or makes a syscall: the same TU links into
 * the ring-3 shell blob and, under TESTING=1, into the kernel for
 * tests/kernel/test_shell_err_k.c (docker/scripts/build.sh).
 */
#ifndef EXODOOM_SHELL_ERR_H
#define EXODOOM_SHELL_ERR_H

#include <stdint.h>

/* What the shell was doing when it failed. Picks the lead word and the
 * wording of the few errors whose advice depends on the command. */
typedef enum {
    SHELL_OP_FS,      /* a filesystem command: "error: ..." */
    SHELL_OP_DISK,    /* acquiring a disk extent: no lead word */
    SHELL_OP_MOUNT,   /* mounting a volume */
    SHELL_OP_RUN,     /* `run`: "run: ..." */
    SHELL_OP_INSTALL, /* `install`: "install: ..." */
    SHELL_OP_PROBE    /* the first-boot system-volume check */
} shell_op_t;

/* Lead word for `op` ("run", "error", ...), or NULL for none. */
const char *shell_err_lead(shell_op_t op);

/* One-line sentence for `err` (either sign) while doing `op`: what happened
 * and, where there is one, what to do. Never NULL and no newline. *mapped is
 * set to 1 when the sentence is specific to the code, 0 when it is only the
 * op's generic fallback -- the caller then appends the numeric code so it
 * stays greppable. */
const char *shell_err_text(shell_op_t op, int64_t err, int *mapped);

#endif /* EXODOOM_SHELL_ERR_H */
