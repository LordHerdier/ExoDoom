#pragma once
#include <sys/types.h>

/*
 * Freestanding <sys/stat.h> (SCRUM-64).
 *
 * One consumer under src/doom/: `M_MakeDirectory()` in m_misc.c calls
 * `mkdir(path, 0755)`, reached from m_config.c when it wants a config or
 * savegame directory.
 *
 * There is no filesystem behind this.  `mkdir` is DECLARED here so the call
 * site compiles; it is not defined anywhere, so anything that actually links
 * and calls it fails at link time rather than silently pretending a directory
 * appeared.  That is the intended state for SCRUM-64, whose scope is compiling
 * src/doom/ to .o -- see docs/libc_audit.md (SCRUM-72) for the full list of
 * what is declared-but-absent and what each call site needs before Doom can
 * link.
 *
 * `struct stat` and `stat()`/`fstat()` are omitted: nothing in src/doom/ calls
 * them, and inventing a layout for a struct no code fills in would be a
 * guess that no test could catch being wrong.
 */

int mkdir(const char *path, mode_t mode);

/* File-type and permission bits, POSIX values.  Present because the mode
 * argument above is written with them at the call site (0755); no code under
 * src/doom/ tests them. */
#define S_IRWXU 0000700
#define S_IRUSR 0000400
#define S_IWUSR 0000200
#define S_IXUSR 0000100
#define S_IRWXG 0000070
#define S_IRGRP 0000040
#define S_IWGRP 0000020
#define S_IXGRP 0000010
#define S_IRWXO 0000007
#define S_IROTH 0000004
#define S_IWOTH 0000002
#define S_IXOTH 0000001
