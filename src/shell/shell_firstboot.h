#ifndef SHELL_FIRSTBOOT_H
#define SHELL_FIRSTBOOT_H

/*
 * shell_firstboot -- the decisions behind the first-boot flow (SCRUM-234).
 *
 * On startup the shell mounts the system volume (the program volume at
 * LIBOS_WAD_DISK_BASE_LBA). A machine with a blank disk has no ExoFS
 * superblock there, and the shell offers to format it. Both decisions are
 * made here, in a header with no syscalls and no globals, so the KUnit suite
 * can drive them (the shell itself never returns and cannot be launched
 * under test -- see tests/kernel/test_shell_libos_k.c). Same arrangement as
 * src/exo_admit.h.
 *
 * Only -EXO_EINVAL means "unformatted": exofs_mount() returns it for a blank
 * or foreign disk (src/libos_fs/exofs.h). Anything else -- the disk is busy,
 * out of memory, an I/O error -- says nothing about what is on the disk, and
 * offering to format on the strength of a transient fault would erase a good
 * volume. Those are reported and no format is offered.
 */

#include <stdint.h>

#include "exo_errno.h"

typedef enum {
    SHELL_FIRSTBOOT_PRESENT     = 0,  /* mounted: a valid volume is there   */
    SHELL_FIRSTBOOT_UNFORMATTED = 1,  /* no superblock: offer to format     */
    SHELL_FIRSTBOOT_ERROR       = 2   /* could not tell: report, never format */
} shell_firstboot_t;

static inline shell_firstboot_t shell_firstboot_classify(int mount_rc)
{
    if (mount_rc == 0) {
        return SHELL_FIRSTBOOT_PRESENT;
    }
    if (mount_rc == -EXO_EINVAL) {
        return SHELL_FIRSTBOOT_UNFORMATTED;
    }
    return SHELL_FIRSTBOOT_ERROR;
}

/* The "[y/N]" answer: only y or Y formats. Enter, n, and every other key are
 * "no" -- the capital in [y/N] is the default. */
static inline int shell_firstboot_answer_is_yes(char c)
{
    return c == 'y' || c == 'Y';
}

#endif /* SHELL_FIRSTBOOT_H */
