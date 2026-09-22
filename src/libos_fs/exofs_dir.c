/*
 * exofs_dir.c — directory operations (SCRUM-189).
 * See exofs_dir.h for the internal design and exofs.h for the public API.
 * The cfat equivalent is directory.c, minus everything that printed.
 */

#include "exofs_dir.h"
#include "exofs_dirent.h"
#include "exofs_path.h"
#include "exofs_name.h"
#include "exofs_fat.h"
#include "exofs_internal.h"
#include "exofs_layout.h"
#include "exofs.h"

#include "exo_errno.h"
#include "string.h"

#include <stdint.h>
#include <stddef.h>

/* ---- "." and ".." ------------------------------------------------------- */

/*
 * Write a directory's own two entries.
 *
 * `self` is the directory's first block; `parent` is its parent's. For the
 * root they are the same block: there is nowhere above it, and a self-loop
 * makes "/.." resolve to "/" the way every other filesystem behaves, with no
 * special case in exofs_path.c.
 */
static int write_dot_entries(exofs_volume_t *v, uint32_t self, uint32_t parent)
{
    int rc = exofs_dir_add(v, self, ".", EXOFS_ATTR_DIRECTORY, self, 0, NULL);
    if (rc < 0) return rc;

    return exofs_dir_add(v, self, "..", EXOFS_ATTR_DIRECTORY, parent, 0, NULL);
}

int exofs_dir_is_dot(exofs_volume_t *v, const exofs_dirent_t *e)
{
    if (v == NULL || e == NULL) return 0;

    /* Length first: only entries of length 1 or 2 can be "." or "..", so
     * everything else is rejected without a name-block read. */
    if (e->name_len != 1u && e->name_len != 2u) return 0;

    int rc = exofs_name_equals(v, e->name_block, e->name_off, e->name_len,
                               (e->name_len == 1u) ? "." : "..");
    return (rc == 1);
}

int exofs_dir_init_root(exofs_volume_t *v)
{
    if (v == NULL) return -EXO_EINVAL;
    return write_dot_entries(v, v->root_block, v->root_block);
}

/* ---- Create ------------------------------------------------------------- */

int exofs_dir_create(exofs_volume_t *v, uint32_t parent_head,
                     const char *name, uint32_t *new_head_out)
{
    if (v == NULL || name == NULL) return -EXO_EINVAL;

    /* Reject a duplicate before allocating anything, so a rejected mkdir
     * costs nothing and leaves nothing behind. */
    int rc = exofs_dir_lookup(v, parent_head, name, NULL, NULL);
    if (rc == 0) return -EXO_EEXIST;
    if (rc != -EXO_ENOENT) return rc;

    uint32_t head;
    rc = exofs_fat_alloc(v, &head);
    if (rc < 0) return rc;

    /*
     * Contents before the link, and the ordering is docs/filesystem.md §1
     * again. A failure after the block is allocated but before the parent
     * names it leaks one block chain — invisible, harmless, reclaimed by a
     * reformat. Linking first would make the parent name a block that is
     * not yet a directory, which every reader above would then walk.
     */
    rc = write_dot_entries(v, head, parent_head);
    if (rc < 0) goto fail;

    rc = exofs_dir_add(v, parent_head, name, EXOFS_ATTR_DIRECTORY, head, 0,
                       NULL);
    if (rc < 0) goto fail;

    if (new_head_out != NULL) *new_head_out = head;
    return 0;

fail:
    /* Give the block chain back rather than stranding it. Best effort: if
     * this fails too there is nothing further to try, and a leaked chain is
     * strictly better than a corrupt one. */
    (void)exofs_dir_destroy(v, head);
    return rc;
}

/* ---- Emptiness and teardown --------------------------------------------- */

int exofs_dir_is_empty(exofs_volume_t *v, uint32_t head, int *out)
{
    if (v == NULL || out == NULL) return -EXO_EINVAL;

    exofs_dir_iter_t it;
    exofs_dir_iter_init(&it, head);

    for (;;) {
        exofs_dirent_t e;
        int rc = exofs_dir_iter_next(v, &it, NULL, &e);
        if (rc < 0) return rc;
        if (rc == 0) break;

        if (!exofs_dir_is_dot(v, &e)) { *out = 0; return 0; }
    }

    *out = 1;
    return 0;
}

int exofs_dir_destroy(exofs_volume_t *v, uint32_t head)
{
    if (v == NULL) return -EXO_EINVAL;

    /*
     * Release every entry's name record before freeing the blocks those
     * entries live in. "Empty" still means two live entries — "." and ".."
     * — whose name records would otherwise be leaked, which for a
     * mkdir/rmdir loop is two records per cycle forever.
     *
     * The iterator is restarted after each removal rather than carried
     * across it: removing an entry rewrites the block the iterator is
     * positioned in, and an iterator that kept its own stale copy of that
     * block would hand back the entry it just deleted. Restarting is O(n^2)
     * in the entry count, which for a directory that must be empty to get
     * here is two.
     */
    for (;;) {
        exofs_dir_iter_t it;
        exofs_dir_iter_init(&it, head);

        exofs_entry_ref_t ref;
        int rc = exofs_dir_iter_next(v, &it, &ref, NULL);
        if (rc < 0) return rc;
        if (rc == 0) break;        /* no live entries left */

        rc = exofs_dir_remove(v, &ref);
        if (rc < 0) return rc;
    }

    return exofs_chain_free(v, head);
}

/* ---- Public API --------------------------------------------------------- */

int exofs_mkdir(const char *path)
{
    exofs_volume_t *v = exofs_vol();
    if (v == NULL) return -EXO_EINVAL;

    exofs_dirent_t parent;
    char leaf[EXOFS_MAX_NAME + 1];

    int rc = exofs_path_resolve_parent(v, path, &parent, leaf);
    if (rc < 0) return rc;

    rc = exofs_dir_create(v, parent.first_block, leaf, NULL);
    if (rc < 0) return rc;

    return exofs_sync();
}

int exofs_rmdir(const char *path)
{
    exofs_volume_t *v = exofs_vol();
    if (v == NULL) return -EXO_EINVAL;

    exofs_entry_ref_t ref;
    exofs_dirent_t e;

    int rc = exofs_path_resolve(v, path, &ref, &e);
    if (rc < 0) return rc;

    if (!(e.attributes & EXOFS_ATTR_DIRECTORY)) return -EXO_ENOTDIR;

    /* The root resolves to a synthetic entry with no slot behind it
     * (exofs_path.c), so there is nothing to remove it from. */
    if (ref.block == EXOFS_NO_BLOCK) return -EXO_EBUSY;

    int empty = 0;
    rc = exofs_dir_is_empty(v, e.first_block, &empty);
    if (rc < 0) return rc;
    if (!empty) return -EXO_ENOTEMPTY;

    /*
     * Unlink from the parent FIRST, then free the directory's blocks. This
     * is docs/filesystem.md §1's rule at its sharpest:
     *
     *   this order        an interruption leaks a block chain that nothing
     *                     references. Invisible and harmless.
     *   the other order   an interruption leaves the parent naming blocks
     *                     that are back on the free list, so the next
     *                     allocation hands them to another file while a
     *                     live directory entry still points at them.
     *
     * One of those is a wasted block; the other is two files sharing
     * storage.
     */
    rc = exofs_dir_remove(v, &ref);
    if (rc < 0) return rc;

    rc = exofs_dir_destroy(v, e.first_block);
    if (rc < 0) return rc;

    return exofs_sync();
}

int exofs_opendir(const char *path, exofs_dir_t *out)
{
    exofs_volume_t *v = exofs_vol();
    if (v == NULL || out == NULL) return -EXO_EINVAL;

    exofs_dirent_t e;
    int rc = exofs_path_resolve(v, path, NULL, &e);
    if (rc < 0) return rc;

    if (!(e.attributes & EXOFS_ATTR_DIRECTORY)) return -EXO_ENOTDIR;

    memset(out, 0, sizeof(*out));
    out->open  = 1;
    out->head  = e.first_block;
    out->block = e.first_block;
    out->index = 0;
    out->steps = 0;
    out->done  = 0;
    return 0;
}

int exofs_readdir(exofs_dir_t *d, exofs_dirinfo_t *out)
{
    exofs_volume_t *v = exofs_vol();
    if (v == NULL || d == NULL || out == NULL) return -EXO_EINVAL;
    if (!d->open) return -EXO_EBADF;

    /* exofs.h keeps exofs_dir_t's fields flat so the public header need not
     * include the internal ones; this is where the two meet. */
    exofs_dir_iter_t it;
    it.block = d->block;
    it.index = d->index;
    it.steps = d->steps;
    it.done  = d->done;

    exofs_dirent_t e;
    int rc = exofs_dir_iter_next(v, &it, NULL, &e);

    d->block = it.block;
    d->index = it.index;
    d->steps = it.steps;
    d->done  = it.done;

    if (rc <= 0) return rc;

    memset(out, 0, sizeof(*out));
    out->size       = e.size;
    out->attributes = e.attributes;

    rc = exofs_name_read(v, e.name_block, e.name_off, e.name_len, out->name);
    if (rc < 0) return rc;

    return 1;
}

int exofs_closedir(exofs_dir_t *d)
{
    if (d == NULL) return -EXO_EINVAL;
    d->open = 0;
    return 0;
}
