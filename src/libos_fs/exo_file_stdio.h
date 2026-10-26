#ifndef EXO_FILE_STDIO_H
#define EXO_FILE_STDIO_H

/*
 * exo_file_stdio — the FILE* shim over ExoFS (SCRUM-42, SCRUM-75).
 *
 * src/stdio.c's fopen/fread/fseek/ftell/fclose are a pluggable reader
 * (exo_stdio_set_backend, src/stdio.h). This installs the backend that routes
 * them through exo_file_* (src/libos_fs/exo_file.h), so a program that has
 * mounted a volume with exofs_mount() can fopen a file on it with no other
 * change -- which is how Doom's unmodified w_file_stdc.c reads the IWAD.
 *
 * Read-only: fopen refuses write modes whichever backend is installed
 * (EROFS, docs/architecture.md §7), so the backend only ever opens for read.
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

/* Remove the backend; fopen goes back to the blob table. For tests. */
void exo_file_stdio_remove(void);

#endif /* EXO_FILE_STDIO_H */
