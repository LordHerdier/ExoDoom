/*
 * exofs_path.c — path parsing and resolution (SCRUM-189).
 * See exofs_path.h for the design and for the two cfat defects this avoids
 * (strtok's global state, and an unchecked strcpy into a fixed stack path
 * buffer).
 */

#include "exofs_path.h"
#include "exofs_dirent.h"
#include "exofs_internal.h"
#include "exofs_layout.h"
#include "exofs.h"

#include "exo_errno.h"
#include "string.h"

#include <stdint.h>
#include <stddef.h>

/* ---- Component iteration ------------------------------------------------ */

void exofs_path_iter_init(exofs_path_iter_t *it, const char *path)
{
    if (it == NULL) return;
    it->cur = path;
}

int exofs_path_iter_next(exofs_path_iter_t *it, char *out)
{
    if (it == NULL || out == NULL || it->cur == NULL) return -EXO_EINVAL;

    /* Skip separators, which also handles "//" and a trailing '/'. */
    while (*it->cur == '/') it->cur++;

    if (*it->cur == '\0') return 0;

    const char *start = it->cur;
    while (*it->cur != '\0' && *it->cur != '/') it->cur++;

    size_t len = (size_t)(it->cur - start);

    /* Reported, not truncated: a truncated component resolves to a
     * different file, which is worse than failing. */
    if (len > EXOFS_MAX_NAME) return -EXO_EINVAL;

    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

int exofs_path_validate(const char *path)
{
    if (path == NULL)  return -EXO_EINVAL;
    if (path[0] != '/') return -EXO_EINVAL;   /* absolute only */

    /* Walk it once so a caller learns about an over-long component up
     * front rather than partway through a resolution that has already
     * touched the disk. */
    exofs_path_iter_t it;
    char comp[EXOFS_MAX_NAME + 1];
    exofs_path_iter_init(&it, path);

    for (;;) {
        int rc = exofs_path_iter_next(&it, comp);
        if (rc < 0) return rc;
        if (rc == 0) return 0;
    }
}

/* ---- Resolution ---------------------------------------------------------
 *
 * Both entry points below walk the same way: start at the root's synthetic
 * entry and look each component up in the directory the previous one named.
 * The only difference is where they stop.
 */

int exofs_path_resolve(exofs_volume_t *v, const char *path,
                       exofs_entry_ref_t *ref, exofs_dirent_t *out)
{
    if (v == NULL) return -EXO_EINVAL;

    int rc = exofs_path_validate(path);
    if (rc < 0) return rc;

    exofs_dirent_t cur;
    rc = exofs_root_dirent(v, &cur);
    if (rc < 0) return rc;

    /* The root is not stored in any parent, so there is no slot to name.
     * A caller that means to modify what it resolved has to notice this;
     * exofs_path.h says so. */
    exofs_entry_ref_t cur_ref = { EXOFS_NO_BLOCK, 0 };

    exofs_path_iter_t it;
    char comp[EXOFS_MAX_NAME + 1];
    exofs_path_iter_init(&it, path);

    for (;;) {
        rc = exofs_path_iter_next(&it, comp);
        if (rc < 0) return rc;
        if (rc == 0) break;

        /* Every component before the last has to be a directory to look
         * inside. Checking here rather than at the lookup means a path like
         * "/file.txt/x" reports -EXO_ENOTDIR instead of -EXO_ENOENT, which
         * is the difference between "you have the wrong path" and "the
         * thing you named is not there". */
        if (!(cur.attributes & EXOFS_ATTR_DIRECTORY)) return -EXO_ENOTDIR;

        exofs_entry_ref_t r;
        exofs_dirent_t e;
        rc = exofs_dir_lookup(v, cur.first_block, comp, &r, &e);
        if (rc < 0) return rc;

        cur     = e;
        cur_ref = r;
    }

    if (ref != NULL) *ref = cur_ref;
    if (out != NULL) *out = cur;
    return 0;
}

int exofs_path_resolve_parent(exofs_volume_t *v, const char *path,
                              exofs_dirent_t *parent_out, char *leaf)
{
    if (v == NULL || parent_out == NULL || leaf == NULL) return -EXO_EINVAL;

    int rc = exofs_path_validate(path);
    if (rc < 0) return rc;

    exofs_dirent_t cur;
    rc = exofs_root_dirent(v, &cur);
    if (rc < 0) return rc;

    exofs_path_iter_t it;
    char comp[EXOFS_MAX_NAME + 1];
    exofs_path_iter_init(&it, path);

    /* Read one component ahead: the loop descends into a component only
     * once it knows another follows, so the last one is left in `leaf`
     * rather than being resolved. */
    rc = exofs_path_iter_next(&it, leaf);
    if (rc < 0) return rc;
    if (rc == 0) return -EXO_EINVAL;   /* "/" — the root has no parent */

    for (;;) {
        rc = exofs_path_iter_next(&it, comp);
        if (rc < 0) return rc;
        if (rc == 0) break;            /* `leaf` holds the final component */

        if (!(cur.attributes & EXOFS_ATTR_DIRECTORY)) return -EXO_ENOTDIR;

        exofs_dirent_t e;
        rc = exofs_dir_lookup(v, cur.first_block, leaf, NULL, &e);
        if (rc < 0) return rc;

        cur = e;
        memcpy(leaf, comp, strlen(comp) + 1u);
    }

    if (!(cur.attributes & EXOFS_ATTR_DIRECTORY)) return -EXO_ENOTDIR;

    *parent_out = cur;
    return 0;
}
