#ifndef EXOFS_PATH_H
#define EXOFS_PATH_H

#include <stdint.h>

#include "exofs_internal.h"
#include "exofs_dirent.h"
#include "exofs_layout.h"

/*
 * exofs_path — path parsing and resolution (SCRUM-189).
 *
 * The cfat equivalents are extract_path/extract_filename/findParentFromPath/
 * findEntryFromPath in direntry.c. Three things change.
 *
 * NO strtok(). cfat tokenised paths with strtok(), which keeps its state in
 * a static. Any function that called strtok() while another was mid-walk
 * silently destroyed the outer walk — and cfat's findEntryFromPath() and
 * findParentFromPath() both use it, so a nested resolution was a latent bug
 * waiting for a caller that did one. The iterator here carries its own
 * cursor.
 *
 * NO FIXED-SIZE PATH COPY. cfat copied the path into `char path[MAXPATH]`
 * with strcpy() before tokenising — an unchecked copy into a 255-byte stack
 * buffer, from a caller-supplied string. Nothing here copies the path; the
 * iterator walks it in place and copies only one component at a time, into a
 * buffer whose size it is told.
 *
 * ABSOLUTE PATHS ONLY. A path must start with '/'. There is no working
 * directory in this library and there should not be: a shell that wants one
 * keeps it itself and passes absolute paths down, which is also the only
 * arrangement that works when two callers share a mounted volume. Empty
 * components ("//") are skipped; a trailing '/' is ignored.
 *
 * "." and ".." are resolved through the directory's own entries, not
 * special-cased here, so they mean whatever the directory says they mean —
 * which is what makes ".." work at all once exofs_dir.c writes them.
 */

/*
 * Component iterator.
 *
 * Initialise with exofs_path_iter_init() on a path, then call
 * exofs_path_iter_next() until it returns 0. `out` receives one
 * NUL-terminated component and must hold EXOFS_MAX_NAME + 1 bytes.
 *
 * Returns 1 with a component, 0 at the end of the path, or -EXO_EINVAL for a
 * component longer than EXOFS_MAX_NAME — reported rather than truncated,
 * since a truncated component would resolve to the wrong file.
 */
typedef struct exofs_path_iter {
    const char *cur;
} exofs_path_iter_t;

void exofs_path_iter_init(exofs_path_iter_t *it, const char *path);
int  exofs_path_iter_next(exofs_path_iter_t *it, char *out);

/* Whether `path` is a syntactically acceptable absolute path. Returns 0 or
 * -EXO_EINVAL; does not touch the volume. */
int exofs_path_validate(const char *path);

/*
 * Resolve `path` to the entry it names.
 *
 * "/" resolves to the synthetic root entry (exofs_root_dirent()), with
 * *ref set to block EXOFS_NO_BLOCK to mark that it is not stored in any
 * parent — a caller that intends to modify the entry it resolved must check
 * for that, because there is no slot to write back to.
 *
 * Returns 0, -EXO_ENOENT if any component is missing, -EXO_ENOTDIR if a
 * non-final component is not a directory, -EXO_EINVAL for a malformed path,
 * or a device error.
 */
int exofs_path_resolve(exofs_volume_t *v, const char *path,
                       exofs_entry_ref_t *ref, exofs_dirent_t *out);

/*
 * Resolve everything but the last component of `path`, which is what create
 * and remove need: the directory to act in, plus the name to act on.
 *
 * `leaf` receives the final component and must hold EXOFS_MAX_NAME + 1
 * bytes. `parent_out` receives the parent directory's entry, whose
 * first_block is the directory chain to pass to exofs_dir_add()/lookup.
 *
 * Returns -EXO_EINVAL for "/" — the root has no parent, so there is no
 * meaningful answer and silently returning the root itself would let a
 * caller try to create an entry named "" or delete the volume's root.
 */
int exofs_path_resolve_parent(exofs_volume_t *v, const char *path,
                              exofs_dirent_t *parent_out, char *leaf);

#endif /* EXOFS_PATH_H */
