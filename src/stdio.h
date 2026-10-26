#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Freestanding stdio stub (SCRUM-20).
 *
 * printf/putchar/puts route formatted output to COM1 serial (src/serial.c).
 * Supported conversions: %d %i %u %x %c %s %p and %% literal, with field
 * width and the '-' (left-justify) and '0' (zero-pad) flags — e.g. %5d,
 * %-10s, %08x. Precision, sign flags, length modifiers and %X/%o/%f are not
 * yet implemented (see the plan / SCRUM-21).
 */

int printf(const char *fmt, ...);
int putchar(int c);
int puts(const char *s);

/*
 * vprintf — printf's va_list sibling, sharing its sink (SCRUM-83).
 *
 * printf is implemented in terms of this rather than the other way round, so
 * there is exactly one formatting engine and one serial sink per build.
 * Added for doom_panic() (src/doom_panic.c), which receives I_Error's
 * varargs as a va_list and must not stand up a second formatter to print
 * them; SCRUM-65's vfprintf(stdout/stderr, ...) is now this same function,
 * which is the second caller that split pays for.
 */
int vprintf(const char *fmt, va_list ap);

/*
 * Internal formatting core — NOT a standard stdio function.
 *
 * Emits each output character via emit(c, ctx) and returns the number of
 * characters produced. This is the shared engine behind printf; SCRUM-21
 * reuses it for snprintf by supplying a bounded-buffer sink. Exposed here so
 * the kernel test suite can capture output into memory.
 */
int kvprintf(void (*emit)(int c, void *ctx), void *ctx,
             const char *fmt, va_list ap);

/*
 * ---------------------------------------------------------------------------
 * File I/O surface -- declared by SCRUM-64, IMPLEMENTED by SCRUM-65.
 * ---------------------------------------------------------------------------
 *
 * This block used to be declared-and-undefined on purpose: SCRUM-64's scope
 * was "every file under src/doom/ compiles", and a call that is declared but
 * has no definition compiles cleanly and then fails loudly at link. That was
 * the honest state, and the alternative it warned against -- stubbing each
 * one to return a plausible value -- would have let Doom link and then
 * misbehave deep inside W_Init with no sign the filesystem under it was
 * imaginary.
 *
 * SCRUM-65 implements them, and not by inventing a filesystem. The design
 * docs/architecture.md sec7 calls for is a MEMORY-MAPPED reader:
 *
 *   - fopen() looks the requested name up in a table of registered blobs
 *     (see exo_file_register_blob below) and returns a read-only stream over
 *     memory that is already mapped. For the IWAD that is the multiboot
 *     module the kernel mapped at LIBOS_WAD_VADDR -- 28 MB that is never
 *     copied, because there is nowhere to copy it to.
 *   - fread/fseek/ftell/feof are offset arithmetic over that window.
 *
 * (SCRUM-75: that is the default. A program that has mounted ExoFS installs
 * a backend, exo_stdio_set_backend below, and the same calls then read real
 * files; ring-3 Doom reads its IWAD that way and registers no blob.)
 *   - fopen() for WRITING needs a backend with a write op (SCRUM-42). With
 *     none installed it FAILS (EROFS), and that is deliberate: Doom writes a
 *     config file and savegames; a write that silently went nowhere would
 *     surface much later as a config that never persists or a savegame that
 *     reloads as garbage. Returning NULL puts the failure at the open, where
 *     M_SaveDefaults and P_SaveGame already handle it. Same reasoning for
 *     remove/rename, which report EROFS rather than claiming success, and
 *     for mkdir when the backend has no mkdir op.
 *   - fwrite/fprintf/vfprintf to stdout or stderr go to COM1. Those are the
 *     65 fprintf(stderr, ...) diagnostics the audit counted, and they need
 *     no file at all.
 *
 * `FILE` is still an incomplete type to everything outside src/stdio.c.
 * Nothing under src/doom/ reaches into it -- every use is through a
 * `FILE *` -- so the layout stays free to change without touching a caller.
 *
 * fscanf supports the scanset ("%99[^\n]", m_config.c's config read); ranges
 * such as "a-z" are not interpreted, which Doom never needs.
 */
typedef struct _exo_file FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *stream);

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

int   fseek(FILE *stream, long offset, int whence);
long  ftell(FILE *stream);
int   feof(FILE *stream);
int   fflush(FILE *stream);

int fprintf(FILE *stream, const char *fmt, ...);
int vfprintf(FILE *stream, const char *fmt, va_list ap);
int fscanf(FILE *stream, const char *fmt, ...);
int sscanf(const char *str, const char *fmt, ...);

int remove(const char *path);
int rename(const char *oldpath, const char *newpath);

/*
 * snprintf/vsnprintf (SCRUM-21's subject, finished under SCRUM-65).
 *
 * kvprintf already took an arbitrary sink, so these are that core plus a
 * bounded-buffer emit function. The part that mattered was NOT the sink: it
 * was precision, without which the WAD lump names these build are wrong.
 * See docs/libc_audit.md sec3.1 and the tests in
 * tests/kernel/test_libc_gaps_k.c.
 *
 * Both return what they WOULD have written, not what they did -- m_misc.c's
 * M_StringJoin sizes its allocation from that number.
 *
 * Consumers under src/doom/: m_misc.c's M_snprintf/M_vsnprintf wrappers, and
 * 5 vsnprintf call sites reached through them.
 */
int snprintf(char *str, size_t size, const char *fmt, ...);
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

/*
 * sprintf/vsprintf — the unbounded sibling of snprintf/vsnprintf.
 *
 * Implemented in terms of vsnprintf with an effectively unbounded size,
 * so there is exactly one buffer-writing engine, not two.
 * Prefer snprintf/vsnprintf wherever the destination size is known --
 * these exist only for callers that already guarantee the buffer is large enough.
 */
int sprintf(char *str, const char *fmt, ...);
int vsprintf(char *str, const char *fmt, va_list ap);


/* vsscanf -- sscanf's va_list form. Not called by Doom; sscanf is
 * implemented in terms of it, the same way printf is in terms of vprintf. */
int vsscanf(const char *str, const char *fmt, va_list ap);

/*
 * ---------------------------------------------------------------------------
 * exo_file_register_blob -- NOT a standard stdio function (SCRUM-65).
 * ---------------------------------------------------------------------------
 *
 * How a file comes to exist on a system with no filesystem.
 *
 * The caller hands over a name and a region of memory that is already
 * mapped, and fopen() will thereafter find it under that name. The intended
 * caller is the Doom LibOS, registering the IWAD the kernel mapped for it
 * (src/libos_wad_map.h) as "freedoom2.wad", so that W_AddFile's fopen
 * resolves without anything under src/doom/ being modified.
 *
 * Matching is on the BASENAME and case-insensitive: Doom builds WAD paths by
 * joining a directory it discovered (d_iwad.c) onto a filename, so what
 * reaches fopen is a path while what was registered is a name.
 *
 * `name` and `data` are borrowed, not copied -- both must outlive every
 * stream opened over them. That is free for the WAD, which is a kernel
 * mapping that outlives the LibOS entirely.
 *
 * Returns 0, or -1 if the table is full or an argument is NULL.
 * Registering a name that already exists replaces it.
 */
int exo_file_register_blob(const char *name, const void *data, size_t size);

/* Forget every registered blob. For tests, which register a series of them
 * and must not leave one behind for the next suite. */
void exo_file_reset_blobs(void);

/*
 * ---------------------------------------------------------------------------
 * exo_stdio_set_backend -- NOT a standard stdio function (SCRUM-75/42).
 * ---------------------------------------------------------------------------
 *
 * Where a real file comes from. A program that has mounted a filesystem
 * installs a backend, and fopen("r") then opens through it instead of
 * looking in the blob table; a name the backend does not have fails with
 * the errno its open reported, it does not fall back to a blob. With no
 * backend installed (the default, and every program that does not link a
 * filesystem) fopen behaves exactly as it always has.
 *
 * The backend is a table of functions rather than a direct call into
 * src/libos_fs/exo_file.h so that src/stdio.c, which every ring-3 target and
 * the kernel link, does not drag ExoFS into all of them.
 * src/libos_fs/exo_file_stdio.c supplies the ExoFS one.
 *
 * open, read, seek and close are required; write, mkdir, remove and rename
 * are optional. A backend without write makes every write mode fail EROFS,
 * which is how the table stayed read-only until SCRUM-42; one without remove
 * or rename makes those fail EROFS too (SCRUM-267). Both take paths the same
 * way open does and return 0 or a negative EXO_E* code.
 *
 * All of them return a negative EXO_E* code (numerically the errno value) on
 * failure; open returns a handle >= 0, read/write the byte count (read 0 at
 * end of file), seek the new absolute position, close and mkdir 0.
 * `flags` to open is a mask of EXO_STDIO_*, derived from fopen's mode string.
 * A backend's write is a plain write at the handle's current position (at
 * end of file under EXO_STDIO_APPEND); the position is set with seek first.
 */
#define EXO_STDIO_READ    0x01
#define EXO_STDIO_WRITE   0x02
#define EXO_STDIO_CREATE  0x04   /* create if absent              */
#define EXO_STDIO_TRUNC   0x08   /* discard existing contents     */
#define EXO_STDIO_APPEND  0x10   /* every write goes to the end   */

typedef struct {
    int     (*open)(const char *path, unsigned flags);
    int64_t (*read)(int handle, void *buf, size_t n);
    int64_t (*seek)(int handle, int64_t off, int whence);
    int     (*close)(int handle);
    int64_t (*write)(int handle, const void *buf, size_t n);   /* optional */
    int     (*mkdir)(const char *path);                        /* optional */
    int     (*remove)(const char *path);                       /* optional */
    int     (*rename)(const char *oldpath, const char *newpath); /* optional */
} exo_stdio_backend_t;

/* Install (or, with NULL, remove) the backend. Streams already open keep the
 * backend they were opened with. */
void exo_stdio_set_backend(const exo_stdio_backend_t *backend);

