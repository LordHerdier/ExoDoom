/*
 * host_libos_heap.c — malloc-backed stand-in for src/libos_heap.c
 * (SCRUM-190).
 *
 * src/libos_fs/exofs_name.c, exofs_dirent.c, exofs_file.c and
 * exofs_volume.c allocate scratch buffers through libos_heap_alloc()/
 * libos_heap_free() rather than malloc() directly (see libos_heap.h's own
 * comment: real LibOS code gets its heap from libos_page_alloc(), which
 * this host tool has no equivalent of and does not need one for). On the
 * build host there is an ordinary process heap sitting right there, so
 * this just forwards to it.
 *
 * Deliberately does NOT #include "libos_heap.h": that header lives in
 * src/ and pulling it in here would need "-I src", which would shadow this
 * file's own needed <stdlib.h> with the project's freestanding
 * src/stdlib.h (no real malloc). The three signatures below are repeated
 * by hand instead -- they are small, stable, and exofs_* code that DOES
 * see the real header (compiled with "-I src") still links against the
 * same symbol names at the end.
 */

#include <stddef.h>
#include <stdlib.h>

void *libos_heap_alloc(size_t size)
{
    return malloc(size);
}

void libos_heap_free(void *ptr)
{
    free(ptr);
}

void *libos_heap_realloc(void *ptr, size_t size)
{
    return realloc(ptr, size);
}
