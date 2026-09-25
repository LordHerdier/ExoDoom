#ifndef EXOFS_DIR_H
#define EXOFS_DIR_H

#include <stdint.h>

#include "exofs_internal.h"
#include "exofs_dirent.h"

/*
 * exofs_dir — directory operations (SCRUM-189).
 *
 * The cfat equivalent is directory.c, minus everything that printed. The
 * public entry points (exofs_mkdir/rmdir/opendir/readdir/closedir) are in
 * exofs.h; this header is the internal half.
 *
 * "." AND ".." ARE REAL ENTRIES, not synthesised during iteration. That is
 * what lets exofs_path.c resolve them with no special case at all — it
 * looks them up in the directory like any other name, so they mean whatever
 * the directory says they mean. The cost is two slots and two name records
 * per directory, and the obligation that anything which ever moves a
 * directory must fix its "..". Nothing moves directories yet; a future
 * rename owns that.
 *
 * The root's ".." points at the root. There is nowhere above it, and a
 * self-loop makes "/.." resolve to "/" the way every other filesystem
 * behaves, without the path code needing to know the root is special.
 */

/*
 * Write the root directory's "." and ".." entries.
 *
 * Called from exofs_format() once the volume is otherwise complete. It has
 * to happen there rather than in the block-laying part of format, because
 * creating an entry allocates a name record, which needs a mounted volume —
 * so format mounts the volume it has just written, bootstraps the root, and
 * unmounts. cfat's createfs() called createRootDirectory() for exactly the
 * same reason.
 */
int exofs_dir_init_root(exofs_volume_t *v);

/*
 * Create a subdirectory called `name` inside the directory at
 * `parent_head`, and return its first block.
 *
 * Allocates the directory's block and writes its "." and ".." before
 * linking it into the parent. If the link then fails the block is freed, so
 * a failure leaves no half-made directory — and the opposite order would
 * leave the parent naming a block that is not yet a directory.
 */
int exofs_dir_create(exofs_volume_t *v, uint32_t parent_head,
                     const char *name, uint32_t *new_head_out);

/* Whether the entry is "." or "..". The name_len check comes first, so only
 * entries of length 1 or 2 ever cost a name-block read. */
int exofs_dir_is_dot(exofs_volume_t *v, const exofs_dirent_t *e);

/*
 * Whether the directory at `head` holds nothing but "." and "..".
 * Returns 0 with *out set, or a negative error.
 */
int exofs_dir_is_empty(exofs_volume_t *v, uint32_t head, int *out);

/*
 * Release every entry in the directory at `head` (freeing each one's name
 * record) and then free the directory's own block chain.
 *
 * Used by rmdir once the directory is known to be empty — "empty" still
 * means two live entries whose name records have to go back.
 */
int exofs_dir_destroy(exofs_volume_t *v, uint32_t head);

#endif /* EXOFS_DIR_H */
