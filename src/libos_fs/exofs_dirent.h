#ifndef EXOFS_DIRENT_H
#define EXOFS_DIRENT_H

#include <stdint.h>

#include "exofs_internal.h"
#include "exofs_layout.h"

/*
 * exofs_dirent — directory entry storage and iteration (SCRUM-189).
 *
 * The cfat equivalent is the entry half of direntry.c. The storage layout
 * carries over — a directory is a FAT chain of blocks, each holding a packed
 * array of fixed-size entries — but the way entries are *found* does not,
 * and that is the substantive change in this file.
 *
 * ITERATION IS POSITIONAL. cfat's getNextEntry() took an entry pointer and
 * found "the next one" by re-scanning the block comparing every entry's name
 * against the current entry's name with strcmp(), then returning the entry
 * after the match. That is O(n) per step, so a full directory walk is
 * O(n^2); it is wrong outright if two entries share a name; and it cannot
 * represent "position 3" at all, only "after the entry called X". Here an
 * exofs_dir_iter_t carries (block, index) and steps by arithmetic.
 *
 * LIVENESS, NOT A TERMINATOR. A directory's extent is its FAT chain. A slot
 * is live iff attributes != 0 and EXOFS_ENT_FREE is clear; everything else
 * is skipped. exofs_layout.h has the argument for why there is no
 * "last entry" flag.
 *
 * SLOTS ARE REUSED. A removed entry becomes EXOFS_ENT_FREE and is handed to
 * the next create before the directory is extended, so create/delete churn
 * does not grow a directory without bound the way cfat's did.
 *
 * NAMES. Adding an entry allocates a name record (exofs_name.h) and removing
 * one releases it. Nothing above this file has to remember that pairing,
 * which is where a leak would otherwise come from.
 */

/* Where an entry lives: which block of the directory's chain, and which slot
 * within it. This is what "the next entry" means here, and what cfat could
 * not express. */
typedef struct exofs_entry_ref {
    uint32_t block;
    uint16_t index;
} exofs_entry_ref_t;

/* Iteration state. Opaque to callers beyond initialising it. */
typedef struct exofs_dir_iter {
    uint32_t block;    /* block currently being scanned          */
    uint16_t index;    /* next slot to look at within that block */
    uint32_t steps;    /* blocks visited, bounding a corrupt chain */
    int      done;
} exofs_dir_iter_t;

/* Read/write the entry at `ref`. Both bounds-check `ref` against the volume
 * and against EXOFS_ENTS_PER_BLOCK. */
int exofs_dirent_read(exofs_volume_t *v, const exofs_entry_ref_t *ref,
                      exofs_dirent_t *out);
int exofs_dirent_write(exofs_volume_t *v, const exofs_entry_ref_t *ref,
                       const exofs_dirent_t *in);

/* Whether a slot holds a live entry. Exported because directory emptiness
 * checks and readdir both need exactly this test and must not each invent
 * their own. */
int exofs_dirent_is_live(const exofs_dirent_t *e);

/*
 * Iterate the live entries of the directory whose chain starts at
 * `dir_head`.
 *
 * exofs_dir_iter_next() returns 1 having filled `ref` and `out`, 0 at the
 * end of the directory, or a negative error — including -EXO_EIO for a
 * cyclic chain, which is bounded here the same way exofs_fat.c bounds its
 * walks.
 *
 * `ref` or `out` may be NULL if the caller wants only the other.
 */
void exofs_dir_iter_init(exofs_dir_iter_t *it, uint32_t dir_head);
int  exofs_dir_iter_next(exofs_volume_t *v, exofs_dir_iter_t *it,
                         exofs_entry_ref_t *ref, exofs_dirent_t *out);

/*
 * Find `name` (NUL-terminated) directly inside the directory at `dir_head`.
 *
 * Returns 0 with `ref`/`out` filled, -EXO_ENOENT if there is no such entry,
 * or a negative error. `ref` and `out` may each be NULL.
 *
 * Comparison goes through exofs_name_equals(), which rejects on length
 * before touching the disk — so most candidates in a directory cost nothing
 * but the entry read that had to happen anyway.
 */
int exofs_dir_lookup(exofs_volume_t *v, uint32_t dir_head, const char *name,
                     exofs_entry_ref_t *ref, exofs_dirent_t *out);

/*
 * Add an entry to the directory at `dir_head`, allocating its name record.
 *
 * Reuses a free slot if the directory has one, otherwise extends the chain.
 * Returns 0 with `ref_out` filled (may be NULL), -EXO_EEXIST if the name is
 * already there, -EXO_EINVAL for a name that is empty or over
 * EXOFS_MAX_NAME, -EXO_ENOSPC, or a device error.
 *
 * On any failure after the name record is allocated, the record is released
 * before returning — a half-added entry would otherwise leak name bytes that
 * nothing references.
 */
int exofs_dir_add(exofs_volume_t *v, uint32_t dir_head, const char *name,
                  uint16_t attributes, uint32_t first_block, uint32_t size,
                  exofs_entry_ref_t *ref_out);

/*
 * Remove the entry at `ref`: release its name record and mark the slot free.
 *
 * Does NOT touch the entry's own data/directory chain — freeing that is the
 * caller's decision, because unlink and rmdir differ exactly there and
 * because a rename will eventually want to move an entry without disturbing
 * its contents.
 */
int exofs_dir_remove(exofs_volume_t *v, const exofs_entry_ref_t *ref);

/* Fill `out` with the synthetic entry describing the root directory.
 *
 * The root has no entry in any parent — it is where the tree starts — so
 * path resolution needs something to hand back for "/" and this manufactures
 * it from the volume's geometry rather than reading a slot that does not
 * exist. */
int exofs_root_dirent(exofs_volume_t *v, exofs_dirent_t *out);

#endif /* EXOFS_DIRENT_H */
