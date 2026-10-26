#ifndef EXO_FILE_STDIO_H
#define EXO_FILE_STDIO_H

#include "libos_fs/exofs.h"

/*
 * exo_file_stdio — the FILE* shim over ExoFS (SCRUM-42, SCRUM-75).
 *
 * src/stdio.c's fopen/fread/fseek/ftell/fclose are a pluggable reader
 * (exo_stdio_set_backend, src/stdio.h). This installs the backend that routes
 * them through exo_file_* (src/libos_fs/exo_file.h), so a program that has
 * mounted a volume with exofs_mount() can fopen a file on it with no other
 * change -- which is how Doom's unmodified w_file_stdc.c reads the IWAD.
 *
 * Writing (SCRUM-42): fopen accepts r/w/a and their "+" forms through this
 * backend, fwrite/fprintf/fflush write through exo_file_write, and mkdir
 * creates a directory. remove and rename (SCRUM-106, wired in SCRUM-267) go
 * through exo_file_remove/exo_file_rename. A stream's writes are buffered a
 * block at a time in src/stdio.c; **fclose (or fflush) every stream before
 * exofs_unmount()**, or its unwritten tail is lost along with its length
 * (exo_file.h, DURABILITY).
 *
 * Paths: fopen's argument is looked up on the mounted volume. ExoFS wants an
 * absolute path and Doom builds "./name" or a bare name, so a relative path is
 * resolved against the volume root and a leading "./" is dropped. Directories
 * are not searched: Doom's own search list names the file it wants.
 *
 * Mount and unmount stay the caller's decision (exo_file.h, NO MOUNT POLICY).
 * Close every FILE before exofs_unmount().
 */
void exo_file_stdio_install(void);

/* Remove the backend (and any fallback volume); fopen goes back to the blob
 * table. For tests. */
void exo_file_stdio_remove(void);

/*
 * A second volume to read from (SCRUM-267). A program with a private volume
 * of its own (the default, exo_file_set_volume) and a shared read-only one --
 * Doom: saves and config on the first, the IWAD on the second -- sets the
 * shared one here. A read-only fopen of a name the default volume does not
 * have is then tried on `vol` before it fails ENOENT. Anything that could
 * write (w, a, +) never falls through, so the default volume is the only one
 * the program can change, and a file present on both is read from the
 * default. NULL clears it. Like the default volume it reads as unset once
 * unmounted; unmount nothing it names before closing every FILE read from it.
 */
void exo_file_stdio_set_fallback(exofs_vol_t *vol);

#endif /* EXO_FILE_STDIO_H */
