#include "context.h"

#include <string.h>

/*
 * context.c — see context.h for the design. A flat table, same shape as
 * vmm.c's own address-space registry (and the same reason: CONTEXT_MAX is
 * small enough that a linear scan is not worth optimizing away).
 */

static context_t contexts[CONTEXT_MAX];

static context_t *find_slot(page_owner_t id) {
    for (int i = 0; i < CONTEXT_MAX; i++) {
        if (contexts[i].state != CONTEXT_STATE_UNUSED && contexts[i].id == id) {
            return &contexts[i];
        }
    }
    return NULL;
}

/* Whether `id` is safe for context_create() to hand out: not already live in
 * this table, and not already bound in vmm.c's own address-space registry
 * either -- src/kernel.c binds PAGE_OWNER_LIBOS there directly (ahead of the
 * TESTING branch, so on every boot), entirely outside this table, and
 * find_slot() alone can't see that binding. Without this second check,
 * next_id starting at PAGE_OWNER_LIBOS would silently hand that id back out
 * on the first call, and vmm_bind_address_space() rebinds a matching owner
 * in place rather than refusing it -- corrupting whatever PAGE_OWNER_LIBOS
 * already pointed at with no error anywhere. */
static int id_is_free(page_owner_t id) {
    return find_slot(id) == NULL && vmm_address_space_for(id) == 0;
}

static context_t *find_free_slot(void) {
    for (int i = 0; i < CONTEXT_MAX; i++) {
        if (contexts[i].state == CONTEXT_STATE_UNUSED) {
            return &contexts[i];
        }
    }
    return NULL;
}

/* Next id to try, starting at the first id page_alloc.h reserves for a real
 * LibOS. Only ever incremented, skipping ids already live in the table --
 * see context_create()'s comment for why a destroyed id can still be
 * reused by a later create despite this counter never going backwards. */
static page_owner_t next_id = PAGE_OWNER_LIBOS;

int context_create(uint64_t pml4_phys, page_owner_t *id_out) {
    /* A context with no real page directory is not a context -- reject the
     * sentinel vmm_address_space_for()/context_pml4() both use for "no
     * binding" up front, rather than silently creating a live, READY
     * context that resolves to nothing. */
    if (pml4_phys == 0) {
        return CONTEXT_EINVAL;
    }

    context_t *slot = find_free_slot();
    if (slot == NULL) {
        return CONTEXT_ENOMEM;
    }

    /* Find an id not already live in this table AND not already bound in
     * vmm.c's own registry (id_is_free() checks both -- see its comment:
     * src/kernel.c binds PAGE_OWNER_LIBOS there directly, outside this
     * table entirely, and next_id starts at that same id). next_id only
     * ever increments, so two contexts never collide while both are live,
     * but a destroyed context's old id is fair game again the moment it's
     * asked for here -- the tables themselves, not the counter, are the
     * source of truth for "is this id in use". PAGE_OWNER_ID_MASK bounds
     * the search so a pathological caller that never destroys anything
     * fails cleanly (CONTEXT_ENOMEM, from find_free_slot() above, catches
     * the ordinary case; this loop only protects the id space itself from
     * wrapping past page_alloc.h's 15-bit id field). */
    page_owner_t id = next_id;
    for (int tries = 0; tries <= PAGE_OWNER_ID_MASK; tries++) {
        if (id_is_free(id) && id != PAGE_OWNER_FREE &&
           id != PAGE_OWNER_KERNEL) {
            break;
        }
        id = (page_owner_t)((id + 1) & PAGE_OWNER_ID_MASK);
        if (tries == PAGE_OWNER_ID_MASK) {
            return CONTEXT_EINVAL;   /* id space exhausted */
        }
    }
    next_id = (page_owner_t)((id + 1) & PAGE_OWNER_ID_MASK);

    int vmm_status = vmm_bind_address_space(id, pml4_phys);
    if (vmm_status != VMM_OK) {
        /* VMM_ENOMEM means vmm.c's own address_spaces[] registry is full --
         * a resource-exhaustion condition distinct from a bad argument, and
         * worth telling apart from VMM_EINVAL (which id_is_free() should
         * already have prevented, but vmm_bind_address_space() is the one
         * source of truth for what it actually refuses). */
        return vmm_status == VMM_ENOMEM ? CONTEXT_ENOMEM : CONTEXT_EINVAL;
    }

    memset(slot, 0, sizeof(*slot));
    slot->id = id;
    slot->state = CONTEXT_STATE_READY;

    *id_out = id;
    return CONTEXT_OK;
}

int context_destroy(page_owner_t id) {
    context_t *slot = find_slot(id);
    if (slot == NULL) {
        return CONTEXT_ENOENT;
    }

    /* A live context is always supposed to have a vmm-side binding
     * (context_create() rejects pml4_phys == 0 and only marks the slot
     * READY after vmm_bind_address_space() succeeds), so VMM_ENOENT here
     * means the two tables have already desynced -- surface that instead
     * of reporting a clean teardown that didn't actually happen. The local
     * slot is still cleared either way: whatever vmm-side state exists (or
     * doesn't), this id is no longer a live context in this table. */
    int vmm_status = vmm_destroy_address_space(id);
    slot->state = CONTEXT_STATE_UNUSED;
    slot->id = PAGE_OWNER_FREE;
    return vmm_status == VMM_OK ? CONTEXT_OK : CONTEXT_ENOENT;
}

context_t *context_lookup(page_owner_t id) {
    return find_slot(id);
}

uint32_t context_count(void) {
    uint32_t count = 0;
    for (int i = 0; i < CONTEXT_MAX; i++) {
        if (contexts[i].state != CONTEXT_STATE_UNUSED) {
            count++;
        }
    }
    return count;
}

int context_set_state(page_owner_t id, context_state_t state) {
    context_t *slot = find_slot(id);
    if (slot == NULL) {
        return CONTEXT_ENOENT;
    }
    slot->state = state;
    return CONTEXT_OK;
}

uint64_t context_pml4(page_owner_t id) {
    if (find_slot(id) == NULL) {
        return 0;
    }
    return vmm_address_space_for(id);
}
