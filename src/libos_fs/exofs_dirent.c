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

/* ---- Iteration ----------------------------------------------------------
 *
 * One block is read per 16 slots rather than per slot: the iterator holds
 * the block it is working through in a local buffer for the duration of that
 * block. Reading per slot would be 16x the transfers for a full scan, and a
 * full scan is what lookup does.
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

    int rc = 0;

    for (;;) {
        /* Same bound as every walk in exofs_fat.c: a directory cannot span
         * more blocks than the volume has, so exceeding that is a cycle. */
        if (it->steps > v->total_blocks) { rc = -EXO_EIO; goto out; }

        rc = exofs_read_block(it->block, buf);
        if (rc < 0) goto out;

        while (it->index < EXOFS_ENTS_PER_BLOCK) {
            exofs_dirent_t e;
            memcpy(&e, buf + (size_t)it->index * sizeof(exofs_dirent_t),
                   sizeof(e));

            uint16_t slot = it->index;
            it->index++;

            if (!exofs_dirent_is_live(&e)) continue;

            if (ref != NULL) { ref->block = it->block; ref->index = slot; }
            if (out != NULL) *out = e;

            rc = 1;
            goto out;
        }

        /* Block exhausted: move to the next one in the chain. */
        uint32_t next;
        rc = exofs_fat_get(v, it->block, &next);
        if (rc < 0) goto out;

        if (next == EXOFS_BLOCK_EOC) { it->done = 1; rc = 0; goto out; }
        if (next == EXOFS_BLOCK_FREE) { rc = -EXO_EIO; goto out; }

        it->block = next;
        it->index = 0;
        it->steps++;
    }

out:
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
 */
static int find_free_slot(exofs_volume_t *v, uint32_t dir_head,
                          exofs_entry_ref_t *out)
{
    uint8_t *buf = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (buf == NULL) return -EXO_ENOMEM;

    int rc = 0;
    uint32_t blk = dir_head;

    for (uint32_t steps = 0; steps <= v->total_blocks; steps++) {
        rc = exofs_read_block(blk, buf);
        if (rc < 0) goto out;

        for (uint16_t i = 0; i < EXOFS_ENTS_PER_BLOCK; i++) {
            exofs_dirent_t e;
            memcpy(&e, buf + (size_t)i * sizeof(exofs_dirent_t), sizeof(e));

            if (!exofs_dirent_is_live(&e)) {
                out->block = blk;
                out->index = i;
                rc = 0;
                goto out;
            }
        }

        uint32_t next;
        rc = exofs_fat_get(v, blk, &next);
        if (rc < 0) goto out;

        if (next == EXOFS_BLOCK_EOC) {
            /* Full: grow the directory. The new block is zeroed by
             * exofs_fat_alloc(), so every slot in it reads as unused with
             * no further initialisation. */
            uint32_t fresh;
            rc = exofs_chain_extend(v, dir_head, &fresh);
            if (rc < 0) goto out;

            out->block = fresh;
            out->index = 0;
            rc = 0;
            goto out;
        }
        if (next == EXOFS_BLOCK_FREE) { rc = -EXO_EIO; goto out; }

        blk = next;
    }

    rc = -EXO_EIO;   /* cycle in the directory chain */

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
