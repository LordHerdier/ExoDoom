#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * Freestanding <sys/types.h> (SCRUM-64).
 *
 * src/doom/m_misc.c includes this next to <sys/stat.h> on non-Windows builds,
 * for the types `mkdir`'s prototype is written in.  That is its only consumer
 * under src/doom/.
 *
 * These are the LP64 definitions, matching the x86_64-elf target the kernel is
 * built for -- `long` is 64-bit, `int` 32-bit.  They are spelled out rather
 * than aliased to the exact-width types from <stdint.h> where POSIX leaves the
 * width implementation-defined, so nothing here claims a guarantee POSIX does
 * not make.
 */

#ifndef _EXO_SSIZE_T_DEFINED
#define _EXO_SSIZE_T_DEFINED
typedef long ssize_t;
#endif

typedef unsigned int  mode_t;
typedef long          off_t;
typedef unsigned long ino_t;
typedef unsigned long dev_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
typedef unsigned long nlink_t;
typedef long          time_t;
typedef int           pid_t;
typedef long          blksize_t;
typedef long          blkcnt_t;
