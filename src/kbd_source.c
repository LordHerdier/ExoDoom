#include "kbd_source.h"
#include "exo_syscall.h"
#include "usb_kbd.h"

static page_owner_t owner[KBD_SRC_MAX];  /* PAGE_OWNER_FREE == unbound */

int kbd_source_valid(int src)
{
    if (src == KBD_SRC_PS2)
        return 1;
    return src > KBD_SRC_PS2 && src < KBD_SRC_MAX &&
           src - 1 < usb_kbd_count();
}

void kbd_source_reset(void)
{
    for (int i = 0; i < KBD_SRC_MAX; i++)
        owner[i] = PAGE_OWNER_FREE;
}

int kbd_source_of(page_owner_t ctx)
{
    if (ctx == PAGE_OWNER_FREE)
        return KBD_SRC_NONE;
    for (int i = 0; i < KBD_SRC_MAX; i++) {
        if (owner[i] == ctx)
            return i;
    }
    return KBD_SRC_NONE;
}

page_owner_t kbd_source_owner(int src)
{
    if (src < 0 || src >= KBD_SRC_MAX)
        return PAGE_OWNER_FREE;
    return owner[src];
}

/* Empty `src`'s ring. The caller is its one consumer (the claim runs in
 * syscall context, and the source has no owner to pop it concurrently). */
static void drain(int src)
{
    kbd_event_t ev;
    if (src == KBD_SRC_PS2) {
        while (exo_kbd_poll(&ev)) {}
    } else {
        while (usb_kbd_pop(src - 1, &ev)) {}
    }
}

int kbd_source_claim(int src, page_owner_t ctx)
{
    /* FREE is also what an unbound owner[] slot holds, so it must be
     * refused before the "already holds it" test below mistakes it for
     * success. */
    if (ctx == PAGE_OWNER_FREE || ctx == PAGE_OWNER_KERNEL)
        return -EXO_EINVAL;
    if (!kbd_source_valid(src))
        return -EXO_ENOENT;
    if (owner[src] == ctx)
        return 0;
    if (owner[src] != PAGE_OWNER_FREE)
        return -EXO_EBUSY;
    kbd_source_release_ctx(ctx);
    drain(src);
    owner[src] = ctx;
    return 0;
}

void kbd_source_release_ctx(page_owner_t ctx)
{
    if (ctx == PAGE_OWNER_FREE)
        return;
    for (int i = 0; i < KBD_SRC_MAX; i++) {
        if (owner[i] == ctx)
            owner[i] = PAGE_OWNER_FREE;
    }
}

int kbd_source_poll(page_owner_t ctx, kbd_event_t *out)
{
    int src = kbd_source_of(ctx);
    if (src == KBD_SRC_NONE) {
        if (owner[KBD_SRC_PS2] != PAGE_OWNER_FREE)
            return 0;
        src = KBD_SRC_PS2;
    }
    if (src == KBD_SRC_PS2)
        return exo_kbd_poll(out);
    return usb_kbd_pop(src - 1, out);
}
