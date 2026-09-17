#pragma once
#include <stddef.h>
#include <stdint.h>

size_t strlen(const char *s);

int strcmp(const char *s1, const char *s2);

int strcasecmp(const char *s1, const char *s2);

int strncasecmp(const char *s1, const char *s2, size_t n);

void *memset(void *s, int c, size_t n);

void *memcpy(void *dest, const void *src, size_t n);

void *memmove(void *dest, const void *src, size_t n);

int memcmp(const void *s1, const void *s2, size_t n);

char *strncpy(char *dest, const char *src, size_t n);

char *strcat(char *dest, const char *src);

char *strchr(const char *s, int c);

/*
 * ---------------------------------------------------------------------------
 * SCRUM-65: the four <string.h> functions Doom reaches that had no
 * implementation -- and, worse, no declaration.
 * ---------------------------------------------------------------------------
 *
 * docs/libc_audit.md sec3.2 is emphatic that the DECLARATIONS mattered more
 * than the bodies here: strncmp, strrchr, strstr and strdup were all called
 * with nothing in scope, so gnu99's implicit rule gave each one a return type
 * of `int`.  For the three that return a pointer that truncates the result to
 * 32 bits at every call site -- the compiler says so, in 10 "makes pointer
 * from integer without a cast" warnings across d_iwad, m_argv, m_config,
 * m_misc and w_wad -- and the damage would have looked like heap corruption,
 * not like a missing prototype.
 */
int   strncmp(const char *s1, const char *s2, size_t n);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

/*
 * strdup is malloc'd, so it is the one function in this header that can fail.
 * Returns NULL when the allocation does, which every Doom caller other than
 * M_StringDuplicate ignores -- M_StringDuplicate (m_misc.c) checks and
 * I_Errors, and is what most of the 9 call sites actually go through.
 */
char *strdup(const char *s);
