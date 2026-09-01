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
