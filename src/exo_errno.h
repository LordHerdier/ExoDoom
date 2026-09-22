#pragma once

/*
 * exo_errno.h — ExoDoom exokernel syscall error codes (SCRUM-57).
 *
 * Split out of exo_syscall.h so the syscall ABI's three separable pieces —
 * numbers, shared structs, error codes — are three files' worth of contract
 * instead of one file wearing all three hats. exo_syscall.h includes this
 * unconditionally; both the kernel view (-DEXO_KERNEL) and the LibOS view see
 * the same codes, since a value returned by the kernel has to mean the same
 * thing on both sides of the syscall boundary.
 *
 * Returned negated in RAX: a syscall that fails with EXO_ENOMEM returns
 * -EXO_ENOMEM (docs/syscall_spec.md §3.1). Values match the Linux errno
 * numbers of the same names — and, concretely, src/errno.h's own values —
 * so a future libc shim can hand an EXO_E* code to `errno` unmodified rather
 * than translating one numbering into another. Prefixed because the libc
 * shim defines the unprefixed names for Doom itself (SCRUM-64); the two
 * headers are independent today (nothing sets `errno` from an EXO_E* code
 * yet, docs/libc_audit.md) but are kept numerically identical so that wiring
 * them together later is a pass-through, not a mapping table.
 *
 * Not every code below is emitted by a bound handler yet — EEXIST, ENOTDIR,
 * EISDIR, ENFILE, EFBIG, ESPIPE and EROFS have no caller until the
 * exo_file_* syscalls (#9-16, Sprint 4-5) land, but are defined now so that
 * work doesn't have to reopen this file to add them, and so the numbering
 * agrees with src/errno.h from the start rather than by later coincidence.
 */

#define EXO_EPERM     1   /* operation not permitted for this LibOS        */
#define EXO_ENOENT    2   /* no such file                                  */
#define EXO_EIO       5   /* disk I/O fault (exo_disk_read/exo_disk_write) */
#define EXO_EBADF     9   /* bad file descriptor                           */
#define EXO_ENOMEM   12   /* out of physical pages / heap                  */
#define EXO_EACCES   13   /* permission denied                             */
#define EXO_EFAULT   14   /* pointer argument outside caller address space */
#define EXO_EBUSY    16   /* resource held by another LibOS (framebuffer)  */
#define EXO_EEXIST   17   /* file already exists                           */
#define EXO_ENODEV   19   /* the hardware resource does not exist here     */
#define EXO_ENOTDIR  20   /* not a directory                               */
#define EXO_EISDIR   21   /* is a directory                                */
#define EXO_EINVAL   22   /* malformed or out-of-range argument            */
#define EXO_ENFILE   23   /* system-wide open file table full              */
#define EXO_EMFILE   24   /* file descriptor table full                    */
#define EXO_EFBIG    27   /* file too large                                */
#define EXO_ENOSPC   28   /* ramdisk full                                  */
#define EXO_ESPIPE   29   /* seek on a non-seekable descriptor             */
#define EXO_EROFS    30   /* write attempted on a read-only filesystem     */
#define EXO_ENOSYS   38   /* syscall number not implemented                */
