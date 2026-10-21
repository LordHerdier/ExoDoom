/*
 * z_zone_shim.c — Z_Malloc/Z_Free for the TESTING kernel (SCRUM-215).
 *
 * src/doom/memio.c, which the vendored mus2mid() writes its MIDI through,
 * allocates from Doom's zone allocator (z_zone.c).  That allocator cannot link
 * into the kernel (nothing under src/doom/ does except the few data-only files
 * build.sh step 3b3 names), so the MUS-conversion test would have no way to
 * run the real converter.  These two symbols back it with the kernel heap.
 *
 * Test-only by location: every C file in tests/kernel/ is compiled only under
 * TESTING, so a shipped kernel carries neither this nor memio.c.  The real
 * libos_doom link
 * gets the real z_zone.c from the vendored src/doom/ sources and must never see this file.
 *
 * The signatures are restated rather than included from z_zone.h, which lives
 * under src/doom/ and is not on the include path the tests/kernel/ files compile
 * with.  The tag and user pointer are ignored: nothing here purges by tag.
 *
 * Z_Malloc keeps the real allocator's contract: it never returns NULL (z_zone.c
 * calls I_Error instead).  memio.c relies on that -- it writes through every
 * pointer it gets, including the buffer it regrows mid-conversion -- so a NULL
 * from kmalloc would surface as a fault far from the cause.  Out of heap here
 * means the test kernel is broken, so say so on serial and halt.
 */

#include "memory.h"
#include "serial.h"

void *Z_Malloc(int size, int tag, void *user);
void  Z_Free(void *ptr);

void *Z_Malloc(int size, int tag, void *user)
{
    (void)tag;
    (void)user;

    void *p = kmalloc((size_t)size);
    if (p == NULL) {
        serial_print("Z_Malloc shim: kernel heap exhausted allocating ");
        serial_print_dec((uint32_t)size);
        serial_print(" bytes\n");
        serial_flush();
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }
    return p;
}

void Z_Free(void *ptr)
{
    kfree(ptr);
}
