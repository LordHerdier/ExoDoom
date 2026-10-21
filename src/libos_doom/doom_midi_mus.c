/*
 * doom_midi_mus.c — the MUS leg of the music reader (SCRUM-215).
 *
 * The only file that names the vendored mus2mid(); see src/doom_midi.h for why
 * src/doom_midi.c reaches it through a hook instead.  Built into the libos_doom
 * link and into the TESTING kernel, never into a shipped kernel (nothing from
 * src/doom/ links there).
 *
 * mus2mid() writes its SMF into a memio stream that lives on Doom's zone
 * allocator, and mem_fclose() frees the buffer along with the stream, so the
 * stream itself is the handle the song holds until doom_midi_close().
 */

#include <stddef.h>   /* before doom/memio.h, which uses size_t without including it */
#include <stdint.h>

#include "doom_midi.h"

#include "doom/memio.h"
#include "doom/mus2mid.h"

static int mus_convert(const void *mus, uint32_t mus_size,
                       const uint8_t **midi, uint32_t *midi_size, void **handle)
{
    /* mem_fopen_read() takes a non-const buffer but only ever reads it. */
    MEMFILE *in  = mem_fopen_read((void *)(uintptr_t)mus, mus_size);
    MEMFILE *out = mem_fopen_write();

    boolean failed = mus2mid(in, out);

    mem_fclose(in);     /* a read stream does not own the lump */

    if (failed) {
        mem_fclose(out);
        return -1;
    }

    void  *buf = NULL;
    size_t len = 0;
    mem_get_buf(out, &buf, &len);

    *midi      = buf;
    *midi_size = (uint32_t)len;
    *handle    = out;
    return 0;
}

static void mus_release(void *handle)
{
    mem_fclose(handle);
}

void doom_midi_mus_install(void)
{
    doom_midi_set_mus_converter(mus_convert, mus_release);
}
