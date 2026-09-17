#pragma once
#include <stdarg.h>
#include <stddef.h>

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
 *   - fopen() for WRITING FAILS, and that is deliberate. Doom writes a
 *     config file and savegames; a write that silently went nowhere would
 *     surface much later as a config that never persists or a savegame that
 *     reloads as garbage. Returning NULL puts the failure at the open, where
 *     M_SaveDefaults and P_SaveGame already handle it. Same reasoning for
 *     remove/rename/mkdir, which report EROFS rather than claiming success.
 *   - fwrite/fprintf/vfprintf to stdout or stderr go to COM1. Those are the
 *     65 fprintf(stderr, ...) diagnostics the audit counted, and they need
 *     no file at all.
 *
 * `FILE` is still an incomplete type to everything outside src/stdio.c.
 * Nothing under src/doom/ reaches into it -- every use is through a
 * `FILE *` -- so the layout stays free to change without touching a caller.
 *
 * The one thing still missing is fscanf's scanset ("%99[^\n]"), used once,
 * to read the config file. fscanf returns EOF, which is what C99 specifies
 * for input failure before any conversion and what M_LoadDefaults' loop
 * already treats as end-of-file; it is also unreachable, since fopen cannot
 * produce a config file to read.
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
