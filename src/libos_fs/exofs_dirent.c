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
 *   dir_scan_for_name()     every slot: the live ones for their names, and
 *                           the first that is not, for a create to use
 *
 * Iteration and the search for a free slot used to be separate hand-written
 * walks. They agreed about what a directory was, but only because someone
 * had been careful: one checked its bound before reading a block
 * (`steps > total_blocks`), the other wrapped the whole per-block body in it
 * (`steps <= total_blocks`), and each had its own copy of "a link into a
 * free block is corruption". A fix to how a damaged directory is detected
 * would have had two places to be applied and nothing to notice if it
 * reached only one -- which is the same hazard SCRUM-226 removed from
 * exofs_fat.c, one layer up.
 *
 * ONE READ PER BLOCK, for as long as the caller keeps `buf`. The cursor
 * records whether `buf` already holds its block, so a scan that owns the
 * buffer across calls -- dir_scan_for_name() does -- reads each directory
 * block exactly once however many slots it yields.
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

/* ---- The name scan (SCRUM-225) ------------------------------------------
 *
 * One pass over a directory that answers both questions anything ever asks
 * of one:
 *
 *   is `name` in here?            -> returns 1, with *ref / *out filled
 *   if not, where could it go?    -> *free_out, the first slot that is not
 *                                    live, with *have_free set
 *
 * exofs_dir_lookup() asks the first. exofs_dir_add() asks both, and used to
 * ask them separately: a whole lookup to rule out a duplicate, then a second
 * walk from the head to find a slot. The lookup alone has to read every
 * block of the directory when the name is not there -- which for a create is
 * the normal case -- so the second walk was re-reading blocks the first had
 * just had in its hand. Noting the first free slot on the way past costs a
 * comparison per slot and no I/O at all.
 *
 * This holds the walker and its buffer itself rather than going through
 * exofs_dir_iter_next(), which re-reads its block on every entry it returns.
 * So each directory block is read exactly ONCE per scan, and the only other
 * reads are name blocks, one for each entry whose length makes it a
 * candidate.
 *
 * IT DOES NOT STOP AT THE FIRST FREE SLOT, and that is the one thing a
 * merged scan can get wrong that two separate ones could not. A hole that
 * comes before the name in chain order is met first; taking it there would
 * put the same name in the directory twice. The slot is remembered and the
 * scan carries on to the end. tests/kernel/test_exofs_k.c has that case.
 *
 * "Unused" (a zeroed slot) and "free" (an EXOFS_ENT_FREE tombstone) are not
 * distinguished: the difference matters to iteration, which skips both, and
 * not to allocation, which overwrites either.
 *
 * `free_out` and `have_free` are both NULL for a caller that only wants the
 * lookup. Returns 1 found, 0 not found having scanned the whole directory,
 * or a negative error -- including -EXO_EIO from the walker for a cyclic
 * chain, before anything has been allocated.
 */
static int dir_scan_for_name(exofs_volume_t *v, uint32_t dir_head,
                             const char *name, size_t want,
                             exofs_entry_ref_t *ref, exofs_dirent_t *out,
                             exofs_entry_ref_t *free_out, int *have_free)
{
    uint8_t *buf = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (buf == NULL) return -EXO_ENOMEM;

    dir_cursor_t c;
    dir_cursor_init(&c, dir_head);

    if (have_free != NULL) *have_free = 0;

    int rc;
    for (;;) {
        exofs_entry_ref_t r;
        exofs_dirent_t e;

        rc = dir_cursor_next(v, &c, buf, &r, &e);
        if (rc <= 0) break;                 /* end of the directory, or error */

        if (!exofs_dirent_is_live(&e)) {
            if (have_free != NULL && !*have_free) {
                *free_out  = r;
                *have_free = 1;
            }
            continue;
        }

        /* Cheap rejection first: a different length cannot be the same
         * name, and name_len is right here in the entry. Only a
         * length-matching candidate costs a name-block read. */
        if (e.name_len != want) continue;

        /* Its own buffer, not `buf`: exofs_name.c's top comment is about
         * exactly this caller, mid-scan on a directory block. */
        int eq = exofs_name_equals(v, e.name_block, e.name_off, e.name_len,
                                   name);
        if (eq < 0) { rc = eq; break; }
        if (!eq) continue;

        if (ref != NULL) *ref = r;
        if (out != NULL) *out = e;
        rc = 1;
        break;
    }

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

    int rc = dir_scan_for_name(v, dir_head, name, want, ref, out, NULL, NULL);
    if (rc < 0) return rc;

    return rc == 1 ? 0 : -EXO_ENOENT;
}

/* ---- Add ---------------------------------------------------------------- */

int exofs_dir_add(exofs_volume_t *v, uint32_t dir_head, const char *name,
                  uint16_t attributes, uint32_t first_block, uint32_t size,
                  exofs_entry_ref_t *ref_out)
{
    if (v == NULL || name == NULL) return -EXO_EINVAL;

    size_t len = strlen(name);
    if (len == 0 || len > EXOFS_MAX_NAME) return -EXO_EINVAL;

    /* One pass: is the name already here, and if not, where is the first
     * free slot? Before anything is allocated, so a rejected create has cost
     * nothing and left nothing behind. */
    exofs_entry_ref_t ref;
    int have_free;
    int rc = dir_scan_for_name(v, dir_head, name, len, NULL, NULL,
                               &ref, &have_free);
    if (rc < 0) return rc;
    if (rc == 1) return -EXO_EEXIST;

    /* Slot before name: settling the slot can extend the directory chain,
     * and doing that after the name record exists would mean unwinding the
     * name on a full volume. This way a failure here has changed nothing
     * observable.
     *
     * No cached tail to jump to instead of scanning (SCRUM-224 considered
     * one): the scan above was owed for the duplicate check regardless, and
     * it has already said whether there is a hole. Only a directory with
     * every slot taken gets here needing more, and growing it is a FAT
     * operation, not a search. The new block is zeroed by exofs_fat_alloc(),
     * so every slot in it reads as unused with no further initialisation. */
    if (!have_free) {
        uint32_t fresh;
        rc = exofs_chain_extend(v, dir_head, &fresh);
        if (rc < 0) return rc;

        ref.block = fresh;
        ref.index = 0;
    }

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
