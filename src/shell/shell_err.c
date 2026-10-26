/*
 * shell_err.c — errno to sentence for the shell (SCRUM-271). See shell_err.h.
 */

#include "shell/shell_err.h"
#include "exo_errno.h"

#include <stddef.h>

const char *shell_err_lead(shell_op_t op)
{
    switch (op) {
    case SHELL_OP_FS:      return "error";
    case SHELL_OP_MOUNT:   return "mount";
    case SHELL_OP_RUN:     return "run";
    case SHELL_OP_INSTALL: return "install";
    case SHELL_OP_PROBE:   return "system volume";
    case SHELL_OP_DISK:
    default:               return NULL;
    }
}

static const char *generic_text(shell_op_t op)
{
    switch (op) {
    case SHELL_OP_MOUNT:   return "could not mount the volume (run mkfs?)";
    case SHELL_OP_RUN:     return "launch failed";
    case SHELL_OP_INSTALL: return "install failed";
    case SHELL_OP_PROBE:   return "cannot check it";
    case SHELL_OP_DISK:    return "cannot use the disk";
    case SHELL_OP_FS:
    default:               return "command failed";
    }
}

const char *shell_err_text(shell_op_t op, int64_t err, int *mapped)
{
    if (err < 0) {
        err = -err;
    }
    if (mapped != NULL) {
        *mapped = 1;
    }

    switch (err) {
    case EXO_EBUSY:
        return "another program is using the disk -- Ctrl+Tab to it, or "
               "quit it first";
    case EXO_ENOENT:
        if (op == SHELL_OP_RUN) {
            return "no such program (try 'programs')";
        }
        if (op == SHELL_OP_INSTALL) {
            return "not on the install disk (type 'install' to list it)";
        }
        return "no such file or directory";
    case EXO_ENODEV:    return "no such disk";
    case EXO_EEXIST:    return "already exists";
    case EXO_ENOTDIR:   return "not a directory";
    case EXO_EISDIR:    return "is a directory";
    case EXO_ENOTEMPTY: return "directory is not empty";
    case EXO_ENOSPC:    return "no space left on the volume";
    case EXO_EROFS:     return "the volume is read-only";
    case EXO_EFBIG:     return "file too large";
    case EXO_ENOMEM:    return "out of memory";
    case EXO_EIO:       return "disk read or write failed";
    case EXO_EINVAL:
        if (op == SHELL_OP_RUN) {
            return "not a valid program package";
        }
        return "invalid argument";
    default:
        break;
    }

    if (mapped != NULL) {
        *mapped = 0;
    }
    return generic_text(op);
}
