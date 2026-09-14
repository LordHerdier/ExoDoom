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
 * File I/O surface (SCRUM-64) -- DECLARED, NOT IMPLEMENTED.
 * ---------------------------------------------------------------------------
 *
 * Everything below this line exists so that src/doom/ compiles.  None of it
 * has a definition anywhere in the tree, on purpose: SCRUM-64's scope is
 * "every doomgeneric .c file compiles to .o with zero errors", and a call that
 * is declared but undefined compiles cleanly and then fails loudly at link
 * time.  That is the honest state to be in -- the alternative, stubbing each
 * one to return a plausible value, would let Doom link and then misbehave
 * somewhere deep inside W_Init with no indication that the filesystem under it
 * was imaginary.
 *
 * So: do not read this block as "stdio works now".  `make docker-build-doom`
 * compiles these files; nothing links them into build/exodoom, and the
 * undefined references are exactly the remaining work.
 * docs/libc_audit.md (SCRUM-72) lists every one of them with its call sites and
 * what implementing it actually requires -- most of them want the memory-mapped
 * WAD reader described in docs/architecture.md sec7, not real file I/O.
 *
 * `FILE` is an incomplete type here.  Nothing under src/doom/ reaches into it
 * -- every use is through a `FILE *` -- so leaving the struct undefined keeps
 * the eventual definition (a pointer and an offset into the mapped WAD module,
 * most likely) free to be whatever the reader needs, and makes any code that
 * tries to peek inside fail at compile time rather than against a layout
 * invented here.
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
 * snprintf/vsnprintf are undefined like the rest of this block, but they are
 * the cheapest of it to finish and the only ones that need no filesystem:
 * src/stdio.c's kvprintf already takes an arbitrary sink, so both are that
 * core plus a bounded-buffer emit function.  SCRUM-21 owns that work -- it is
 * deliberately not done here, because SCRUM-64 is the compile pass and
 * implementing one function from the block while declaring the other fourteen
 * would blur which of the two tickets left the tree where it is.
 *
 * Consumers under src/doom/: m_misc.c's M_snprintf/M_vsnprintf wrappers, and
 * 5 vsnprintf call sites reached through them.
 */
int snprintf(char *str, size_t size, const char *fmt, ...);
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
