/*
 * exofs_dirent.c — directory entry storage and iteration (SCRUM-189).
 * See exofs_dirent.h for the design and for what this does differently from
 * cfat's direntry.c.
 */

#include "exofs_dirent.h"
#include "exofs_internal.h"
#include "exofs_layout.h"
#include "exofs_blockdev.h"
#include "exofs_name.h"
#include "exofs_fat.h"
#include "exofs.h"

#include "exo_errno.h"
#include "libos_heap.h"
#include "string.h"

#include <stdint.h>
#include <stddef.h>

int exofs_dirent_is_live(const exofs_dirent_t *e)
{
    if (e == NULL) return 0;
    if (e->flags & EXOFS_ENT_FREE) return 0;
    return e->attributes != 0;
}

/* ---- Slot access --------------------------------------------------------
 *
 * Each of these is a whole-block transfer for one 32-byte entry. That is the
 * cost of not caching data blocks (docs/filesystem.md §4) and it is paid
 * knowingly: a directory scan reads each block once per 16 entries anyway
 * once the iterator below is doing the reading, and the alternative is a
 * second dirty-state invariant.
 */

static int ref_valid(exofs_volume_t *v, const exofs_entry_ref_t *ref)
{
    if (v == NULL || ref == NULL)               return 0;
    if (ref->block >= v->total_blocks)          return 0;
    if (ref->index >= EXOFS_ENTS_PER_BLOCK)     return 0;
    return 1;
}

int exofs_dirent_read(exofs_volume_t *v, const exofs_entry_ref_t *ref,
                      exofs_dirent_t *out)
{
    if (!ref_valid(v, ref) || out == NULL) return -EXO_EINVAL;

    uint8_t *buf = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (buf == NULL) return -EXO_ENOMEM;

    int rc = exofs_read_block(ref->block, buf);
    if (rc == 0) {
        memcpy(out, buf + (size_t)ref->index * sizeof(exofs_dirent_t),
               sizeof(exofs_dirent_t));
    }

    libos_heap_free(buf);
    return rc;
}

int exofs_dirent_write(exofs_volume_t *v, const exofs_entry_ref_t *ref,
                       const exofs_dirent_t *in)
{
    if (!ref_valid(v, ref) || in == NULL) return -EXO_EINVAL;

    uint8_t *buf = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (buf == NULL) return -EXO_ENOMEM;

    /* Read-modify-write: 15 other entries share this block. */
    int rc = exofs_read_block(ref->block, buf);
    if (rc == 0) {
        memcpy(buf + (size_t)ref->index * sizeof(exofs_dirent_t), in,
               sizeof(exofs_dirent_t));
        rc = exofs_write_block(ref->block, buf);
    }

    libos_heap_free(buf);
    return rc;
}

int exofs_root_dirent(exofs_volume_t *v, exofs_dirent_t *out)
{
    if (v == NULL || out == NULL) return -EXO_EINVAL;

    memset(out, 0, sizeof(*out));
    out->name_block  = EXOFS_NO_BLOCK;   /* the root's name is "/" */
    out->name_off    = 0;
    out->name_len    = 0;
    out->first_block = v->root_block;
    out->size        = 0;
    out->attributes  = EXOFS_ATTR_DIRECTORY;
    return 0;
}

/* ---- The directory walker (SCRUM-224) -----------------------------------
 *
 * dir_cursor_next() is the ONE loop that follows a directory's chain, and so
 * the only place a directory walk's cycle bound and corrupt-link checks
 * live. It yields every slot in chain order, live or not; what a caller
 * wants from a directory is then a filter over that, not a loop of its own:
 *
 *   exofs_dir_iter_next()   the live slots
 *   find_free_slot()        the first slot that is not live
 *
 * Those two used to be separate hand-written walks. They agreed about what a
 * directory was, but only because someone had been careful: one checked its
 * bound before reading a block (`steps > total_blocks`), the other wrapped
 * the whole per-block body in it (`steps <= total_blocks`), and each had its
 * own copy of "a link into a free block is corruption". A fix to how a
 * damaged directory is detected would have had two places to be applied and
 * nothing to notice if it reached only one -- which is the same hazard
 * SCRUM-226 removed from exofs_fat.c, one layer up.
 *
 * ONE READ PER BLOCK, for as long as the caller keeps `buf`. The cursor
 * records whether `buf` already holds its block, so a scan that owns the
 * buffer across calls -- find_free_slot() does -- reads each directory block
 * exactly once however many slots it yields.
 *
 * THE BOUND. A directory cannot span more blocks than the volume has, so a
 * walk that has followed that many links is in a cycle: -EXO_EIO, the same
 * rule and the same code as every walk in exofs_fat.c.
 */
typedef struct dir_cursor {
    uint32_t block;    /* block the cursor is in                   */
    uint16_t index;    /* next slot to yield from that block       */
    uint32_t steps;    /* links followed so far, for the bound     */
    int      loaded;   /* whether the caller's buf holds `block`   */
} dir_cursor_t;

static void dir_cursor_init(dir_cursor_t *c, uint32_t dir_head)
{
    c->block  = dir_head;
    c->index  = 0;
    c->steps  = 0;
    c->loaded = 0;
}

/*
 * Yield the next slot. Returns 1 with *ref and *e filled, 0 at the end of
 * the chain, or a negative error. `buf` is a block-sized staging buffer in
 * the LibOS window; it must be the same one on every call that passes a
 * cursor with `loaded` set.
 */
static int dir_cursor_next(exofs_volume_t *v, dir_cursor_t *c, uint8_t *buf,
                           exofs_entry_ref_t *ref, exofs_dirent_t *e)
{
    for (;;) {
        if (c->index < EXOFS_ENTS_PER_BLOCK) {
            if (!c->loaded) {
                int rc = exofs_read_block(c->block, buf);
                if (rc < 0) return rc;
                c->loaded = 1;
            }

            memcpy(e, buf + (size_t)c->index * sizeof(exofs_dirent_t),
                   sizeof(*e));
            ref->block = c->block;
            ref->index = c->index;
            c->index++;
            return 1;
        }

        /* Block exhausted: move to the next one in the chain. */
        uint32_t next;
        int rc = exofs_fat_get(v, c->block, &next);
        if (rc < 0) return rc;

        if (next == EXOFS_BLOCK_EOC)  return 0;
        if (next == EXOFS_BLOCK_FREE) return -EXO_EIO;

        if (c->steps >= v->total_blocks) return -EXO_EIO;   /* a cycle */

        c->block  = next;
        c->index  = 0;
        c->steps++;
        c->loaded = 0;
    }
}

/* ---- Iteration ----------------------------------------------------------
 *
 * The public iterator: the walker above, filtered to live slots.
 *
 * It re-reads its current block on every call. exofs_dir_iter_t is a plain
 * struct a caller keeps on its own stack, with nowhere to hold 512 bytes
 * between calls -- and nowhere it legally could, since a disk buffer has to
 * come from the LibOS window (exofs_blockdev.h). So a caller stepping
 * through a directory one entry at a time pays one read per entry, not one
 * per 16. Scans that live entirely inside this file do not go through here
 * for exactly that reason; they hold the walker and its buffer themselves.
 */

void exofs_dir_iter_init(exofs_dir_iter_t *it, uint32_t dir_head)
{
    if (it == NULL) return;
    it->block = dir_head;
    it->index = 0;
    it->steps = 0;
    it->done  = 0;
}

int exofs_dir_iter_next(exofs_volume_t *v, exofs_dir_iter_t *it,
                        exofs_entry_ref_t *ref, exofs_dirent_t *out)
{
    if (v == NULL || it == NULL) return -EXO_EINVAL;
    if (it->done) return 0;

    uint8_t *buf = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (buf == NULL) return -EXO_ENOMEM;

    /* Pick up where the previous call stopped. */
    dir_cursor_t c;
    c.block  = it->block;
    c.index  = it->index;
    c.steps  = it->steps;
    c.loaded = 0;

    int rc;
    for (;;) {
        exofs_entry_ref_t r;
        exofs_dirent_t e;

        rc = dir_cursor_next(v, &c, buf, &r, &e);
        if (rc <= 0) break;

        if (!exofs_dirent_is_live(&e)) continue;

        if (ref != NULL) *ref = r;
        if (out != NULL) *out = e;
        break;
    }

    it->block = c.block;
    it->index = c.index;
    it->steps = c.steps;
    if (rc == 0) it->done = 1;

    libos_heap_free(buf);
    return rc;
}

/* ---- Lookup ------------------------------------------------------------- */

int exofs_dir_lookup(exofs_volume_t *v, uint32_t dir_head, const char *name,
                     exofs_entry_ref_t *ref, exofs_dirent_t *out)
{
    if (v == NULL || name == NULL) return -EXO_EINVAL;

    size_t want = strlen(name);
    if (want == 0 || want > EXOFS_MAX_NAME) return -EXO_EINVAL;

    exofs_dir_iter_t it;
    exofs_dir_iter_init(&it, dir_head);

    for (;;) {
        exofs_entry_ref_t r;
        exofs_dirent_t e;

        int rc = exofs_dir_iter_next(v, &it, &r, &e);
        if (rc < 0) return rc;
        if (rc == 0) return -EXO_ENOENT;

        /* Cheap rejection first: a different length cannot be the same
         * name, and name_len is right here in the entry. Only a
         * length-matching candidate costs a name-block read. */
        if (e.name_len != want) continue;

        int eq = exofs_name_equals(v, e.name_block, e.name_off, e.name_len,
                                   name);
        if (eq < 0) return eq;
        if (!eq) continue;

        if (ref != NULL) *ref = r;
        if (out != NULL) *out = e;
        return 0;
    }
}

/* ---- Add ---------------------------------------------------------------- */

/*
 * Find a slot for a new entry: the first free-or-unused one in the
 * directory's chain, extending the chain if every slot is taken.
 *
 * "Unused" (a zeroed slot) and "free" (an EXOFS_ENT_FREE tombstone) are both
 * acceptable and are not distinguished — the difference matters to iteration,
 * which skips both, and not to allocation, which overwrites either.
 *
 * The same walk as iteration, with the opposite filter (SCRUM-224): the
 * walker yields every slot and this takes the first one that is not live.
 * It owns the buffer for the whole search, so each block is read once.
 *
 * NO CACHED TAIL. A "resume here" hint was considered -- it is what the FAT
 * allocator and the name area both have -- and left out, because it could
 * not make a create cheaper. exofs_dir_add() has to check the new name
 * against every entry already in the directory before it gets here, which
 * means reading every block of the chain whatever this function does. The
 * second pass is the avoidable part, and the fix for that is not to make it
 * shorter but to not make it: the duplicate check can note the first free
 * slot it passes (SCRUM-225).
 */
static int find_free_slot(exofs_volume_t *v, uint32_t dir_head,
                          exofs_entry_ref_t *out)
{
    uint8_t *buf = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (buf == NULL) return -EXO_ENOMEM;

    dir_cursor_t c;
    dir_cursor_init(&c, dir_head);

    int rc;
    for (;;) {
        exofs_entry_ref_t r;
        exofs_dirent_t e;

        rc = dir_cursor_next(v, &c, buf, &r, &e);
        if (rc < 0) goto out;
        if (rc == 0) break;                 /* every slot is taken */

        if (!exofs_dirent_is_live(&e)) {
            *out = r;
            rc = 0;
            goto out;
        }
    }

    /* Full: grow the directory. The new block is zeroed by
     * exofs_fat_alloc(), so every slot in it reads as unused with no
     * further initialisation. */
    uint32_t fresh;
    rc = exofs_chain_extend(v, dir_head, &fresh);
    if (rc < 0) goto out;

    out->block = fresh;
    out->index = 0;
    rc = 0;

out:
    libos_heap_free(buf);
    return rc;
}

int exofs_dir_add(exofs_volume_t *v, uint32_t dir_head, const char *name,
                  uint16_t attributes, uint32_t first_block, uint32_t size,
                  exofs_entry_ref_t *ref_out)
{
    if (v == NULL || name == NULL) return -EXO_EINVAL;

    size_t len = strlen(name);
    if (len == 0 || len > EXOFS_MAX_NAME) return -EXO_EINVAL;

    /* Duplicate check before anything is allocated, so a rejected create
     * has cost nothing and left nothing behind. */
    int rc = exofs_dir_lookup(v, dir_head, name, NULL, NULL);
    if (rc == 0) return -EXO_EEXIST;
    if (rc != -EXO_ENOENT) return rc;

    /* Slot before name: finding a slot can extend the directory chain, and
     * doing that after the name record exists would mean unwinding the name
     * on a full volume. This way the only thing to unwind is the slot
     * search, which changes nothing observable. */
    exofs_entry_ref_t ref;
    rc = find_free_slot(v, dir_head, &ref);
    if (rc < 0) return rc;

    uint32_t nblk; uint16_t noff;
    rc = exofs_name_alloc(v, name, (uint32_t)len, &nblk, &noff);
    if (rc < 0) return rc;

    exofs_dirent_t e;
    memset(&e, 0, sizeof(e));
    e.name_block  = nblk;
    e.name_off    = noff;
    e.name_len    = (uint16_t)len;
    e.first_block = first_block;
    e.size        = size;
    e.attributes  = attributes;
    e.flags       = 0;
    e.ctime       = exofs_bdev_ticks();
    e.mtime       = e.ctime;

    rc = exofs_dirent_write(v, &ref, &e);
    if (rc < 0) {
        /* Release the name record rather than leaking bytes nothing
         * references — see exofs_dirent.h. */
        (void)exofs_name_free(v, nblk, noff);
        return rc;
    }

    if (ref_out != NULL) *ref_out = ref;
    return 0;
}

/* ---- Remove ------------------------------------------------------------- */

int exofs_dir_remove(exofs_volume_t *v, const exofs_entry_ref_t *ref)
{
    if (!ref_valid(v, ref)) return -EXO_EINVAL;

    exofs_dirent_t e;
    int rc = exofs_dirent_read(v, ref, &e);
    if (rc < 0) return rc;

    if (!exofs_dirent_is_live(&e)) return -EXO_ENOENT;

    /*
     * Name record first, then the slot.
     *
     * The same ordering rule as docs/filesystem.md §1: if this is
     * interrupted between the two, the entry is still live and still points
     * at a released name record — which exofs_name_read() reports as
     * -EXO_EINVAL, a visibly broken entry. The other order leaves a freed
     * slot still holding the only reference to a live name record, which is
     * a silent leak nothing can find again.
     */
    if (e.name_len != 0) {
        rc = exofs_name_free(v, e.name_block, e.name_off);
        if (rc < 0) return rc;
    }

    /* Zero the slot rather than only setting the flag: it costs nothing in
     * the same write, and it means a stale name_block/first_block cannot be
     * followed by anything that mistakes a tombstone for a live entry. */
    memset(&e, 0, sizeof(e));
    e.flags = EXOFS_ENT_FREE;

    return exofs_dirent_write(v, ref, &e);
}
