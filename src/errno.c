#include "errno.h"

/*
 * The one definition behind <errno.h>'s `errno` macro (SCRUM-64).
 *
 * `errno.h` #defines `errno` to this object, so the definition has to be
 * written under the real name -- `int errno = 0;` here would expand to
 * `int exo_errno = 0;` and still work, but reads as though the macro were
 * absent.  Spelling it out avoids that.
 *
 * C requires errno to be non-zero-initialised by nothing and set by library
 * functions on failure; it is never cleared on success.  Nothing in the shim
 * sets it yet (docs/libc_audit.md), so it holds 0 for the life of the boot and
 * m_misc.c's `errno == EISDIR` test therefore always answers false -- which is
 * the correct answer for a shim with no directories, but for the wrong reason.
 */
int exo_errno = 0;
