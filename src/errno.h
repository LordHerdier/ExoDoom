#pragma once

/*
 * Freestanding <errno.h> (SCRUM-64).
 *
 * doomgeneric reads `errno` in exactly one place -- `M_FileExists()` in
 * src/doom/m_misc.c tests `errno == EISDIR` to tell "the open failed" apart
 * from "the open failed because it is a directory" -- and src/doom/m_config.c
 * includes the header without using it.  That one live use is the whole
 * requirement.
 *
 * `errno` is a genuine object here (src/errno.c), not a macro over a
 * per-thread lookup: the kernel is single-threaded and the LibOS is one
 * context, so there is nothing to make it thread-local against yet.  It is
 * declared the standard way -- as a macro expanding to an lvalue -- so that
 * code written against a real libc, which may assume `errno` is not a plain
 * identifier, still compiles.
 *
 * The values below are Linux's, so that anything later compared against a
 * host-built tool's output agrees.  Only the ones the shim can actually
 * produce are listed; adding a constant here is not the same as anything ever
 * setting it.  Nothing in the shim sets `errno` today -- see
 * docs/libc_audit.md (SCRUM-72) for what that costs and which call sites care.
 */

extern int exo_errno;

#define errno exo_errno

#define EPERM         1  /* Operation not permitted */
#define ENOENT        2  /* No such file or directory */
#define EIO           5  /* I/O error */
#define EBADF         9  /* Bad file descriptor */
#define ENOMEM       12  /* Cannot allocate memory */
#define EACCES       13  /* Permission denied */
#define EBUSY        16  /* Device or resource busy */
#define EEXIST       17  /* File exists */
#define ENODEV       19  /* No such device */
#define ENOTDIR      20  /* Not a directory */
#define EISDIR       21  /* Is a directory */
#define EINVAL       22  /* Invalid argument */
#define ENFILE       23  /* File table overflow */
#define EMFILE       24  /* Too many open files */
#define EFBIG        27  /* File too large */
#define ENOSPC       28  /* No space left on device */
#define ESPIPE       29  /* Illegal seek */
#define EROFS        30  /* Read-only file system */
#define ERANGE       34  /* Numerical result out of range */
#define ENOSYS       38  /* Function not implemented */
#define ENOTEMPTY    39  /* Directory not empty */
