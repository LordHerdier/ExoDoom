#include "string.h"
#include "ctype.h"
#include "stdlib.h"   /* strdup: malloc -- the one allocating function here (SCRUM-65) */

size_t strlen(const char *s) {
  const char *p = s;
  while (*p)
    p++;
  return (size_t)(p - s);
}

int strcmp(const char *s1, const char *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return (unsigned char)*s1 - (unsigned char)*s2;
}

/* strcasecmp/strncasecmp are POSIX, not C standard, but doomgeneric leans on
 * them for WAD/config/argv parsing.  Cast through unsigned char before tolower
 * to avoid negative-char UB, matching strcmp/memcmp's unsigned-char diffs. */
int strcasecmp(const char *s1, const char *s2) {
  while (*s1 && (tolower((unsigned char)*s1) == tolower((unsigned char)*s2))) {
    s1++;
    s2++;
  }
  return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
  while (n && *s1 && (tolower((unsigned char)*s1) == tolower((unsigned char)*s2))) {
    s1++;
    s2++;
    n--;
  }
  if (n == 0)
    return 0;
  return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

void *memset(void *s, int c, size_t n) {
  unsigned char *p = s;
  while (n--)
    *p++ = (unsigned char)c;
  return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
  unsigned char *d = dest;
  const unsigned char *s = src;
  while (n--)
    *d++ = *s++;
  return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
  unsigned char *d = dest;
  const unsigned char *s = src;
  if (d < s || d >= s + n) {
    while (n--)
      *d++ = *s++;
  } else {
    d += n;
    s += n;
    while (n--)
      *--d = *--s;
  }
  return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
  const unsigned char *a = s1;
  const unsigned char *b = s2;
  while (n--) {
    if (*a != *b)
      return *a - *b;
    a++;
    b++;
  }
  return 0;
}

char *strchr(const char *s, int c) {
  unsigned char ch = (unsigned char)c;
  while (*s) {
    if ((unsigned char)*s == ch)
      return (char *)s;
    s++;
  }
  return ch == '\0' ? (char *)s : NULL;
}

/* NOTE: strcat has no bounds checking! Caller is responsible for ensuring dest
 * has sufficient space for dest + src + NUL terminator. */
char *strcat(char *dest, const char *src) {
  char *d = dest;
  while (*d)
    d++;
  while ((*d++ = *src++))
    ;
  return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
  size_t i;
  for (i = 0; i < n && src[i]; i++)
    dest[i] = src[i];
  for (; i < n; i++)
    dest[i] = '\0';
  return dest;
}

/*
 * ---------------------------------------------------------------------------
 * SCRUM-65.  See src/string.h for why the declarations mattered more than
 * these bodies.
 * ---------------------------------------------------------------------------
 */

int strncmp(const char *s1, const char *s2, size_t n)
{
    /* Compares as unsigned char, which is what the standard requires and what
     * strcmp above already does: with plain (signed) char, a byte over 0x7F
     * compares as negative and the ordering inverts for exactly the high-bit
     * characters. */
    while (n--) {
        unsigned char a = (unsigned char)*s1++;
        unsigned char b = (unsigned char)*s2++;

        if (a != b) {
            return (int)a - (int)b;
        }
        if (a == '\0') {
            return 0;
        }
    }

    return 0;
}

char *strrchr(const char *s, int c)
{
    char        ch   = (char)c;
    const char *last = (const char *)0;

    /* The terminator is part of the string for strrchr's purposes, so
     * strrchr(s, '\0') must return a pointer to it -- hence the post-loop
     * check rather than a `while (*s)`.  d_iwad.c relies on ordinary
     * behaviour (finding the last '/' or '.'), but getting this edge wrong is
     * the classic way to have strrchr disagree with every other libc. */
    for (;; s++) {
        if (*s == ch) {
            last = s;
        }
        if (*s == '\0') {
            break;
        }
    }

    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    /* The empty needle matches at position 0 -- standard, and worth writing
     * down because the loop below would otherwise return haystack only by
     * accident of the inner loop not running. */
    if (*needle == '\0') {
        return (char *)haystack;
    }

    for (; *haystack != '\0'; haystack++) {
        const char *h = haystack;
        const char *n = needle;

        while (*n != '\0' && *h == *n) {
            h++;
            n++;
        }

        if (*n == '\0') {
            return (char *)haystack;
        }
    }

    return (char *)0;
}

char *strdup(const char *s)
{
    size_t len;
    char  *copy;

    /* Not in C89/C99 -- it is POSIX -- but Doom calls it in 9 places, and
     * m_misc.c's M_StringDuplicate wraps it with an I_Error on failure. */
    len  = strlen(s) + 1;
    copy = malloc(len);

    if (copy == (char *)0) {
        return (char *)0;
    }

    memcpy(copy, s, len);

    return copy;
}
