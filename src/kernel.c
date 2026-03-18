#include <stdint.h>
#include "multiboot.h"
#include "serial.h"
#include "fb.h"
#include "fnv1a.h"
#include "wad.h"
#include "flat.h"

#define FLAT_SCALE 2
/* Scale options:
 *   1 → 64×64 px per flat, 16×12 grid (192 flats, fills 1024×768)
 *   2 → 128×128 px per flat,  8×6 grid  (48 flats, fills 1024×768)
 *   4 → 256×256 px per flat,  4×3 grid  (12 flats, fills 1024×768)
 */
#define GRID_COLS (1024 / (64 * FLAT_SCALE))
#define GRID_ROWS  (768 / (64 * FLAT_SCALE))

static inline void qemu_exit(uint32_t code) {
    __asm__ volatile ("outl %0, %1" : : "a"(code), "Nd"(0xF4));
}

static void serial_print_hex(uint32_t v) {
    const char hex[] = "0123456789abcdef";
    serial_print("0x");
    for (int i = 28; i >= 0; i -= 4)
        serial_putc(hex[(v >> i) & 0xf]);
}

static void serial_print_uint(uint32_t v) {
    if (v == 0) { serial_putc('0'); return; }
    char buf[10];
    int i = 0;
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) serial_putc(buf[i]);
}

void kernel_main(uint32_t mb_info_addr) {
    serial_init();

    struct multiboot_info *mb = (struct multiboot_info *)mb_info_addr;

    serial_print("Kernel Booted\n");

    serial_print("mods_count=");
    serial_print_uint(mb->mods_count);
    serial_print(" mods_addr=");
    serial_print_hex(mb->mods_addr);
    serial_putc('\n');

    wad_t wad;
    const uint8_t *palette = (void *)0;

    if (!(mb->flags & MULTIBOOT_INFO_FLAG_MODS) || mb->mods_count == 0) {
        serial_print("ERROR: no multiboot modules loaded (WAD missing)\n");
        serial_flush();
        for (;;);
    }

    struct multiboot_module *mods = (struct multiboot_module *)mb->mods_addr;
    for (uint32_t i = 0; i < mb->mods_count; i++) {
        uint32_t size = mods[i].mod_end - mods[i].mod_start;
        const char *name = (const char *)(uintptr_t)mods[i].cmdline;

        serial_print("  mod[");
        serial_print_uint(i);
        serial_print("] start=");
        serial_print_hex(mods[i].mod_start);
        serial_print(" end=");
        serial_print_hex(mods[i].mod_end);
        serial_print(" size=");
        serial_print_uint(size);
        serial_print(" name=");
        serial_print(name && *name ? name : "(none)");
        serial_putc('\n');

        uint32_t checksum = fnv1a32((const void *)(uintptr_t)mods[i].mod_start, size);
        serial_print("  wad checksum=");
        serial_print_hex(checksum);
        serial_putc('\n');
    }

    /* --- WAD parse --- */
    const uint8_t *wad_data = (const uint8_t *)(uintptr_t)mods[0].mod_start;
    uint32_t wad_size = mods[0].mod_end - mods[0].mod_start;

    if (wad_init(&wad, wad_data, wad_size) != 0) {
        serial_print("ERROR: wad_init failed\n");
        serial_flush();
        for (;;);
    }
    serial_print("WAD lumps=");
    serial_print_uint(wad.numlumps);
    serial_putc('\n');

    uint32_t playpal_size;
    palette = wad_find_lump(&wad, "PLAYPAL", &playpal_size);
    if (!palette) {
        serial_print("ERROR: PLAYPAL not found\n");
        serial_flush();
        for (;;);
    }
    serial_print("PLAYPAL size=");
    serial_print_uint(playpal_size);
    serial_putc('\n');

    serial_flush();

    /* --- Framebuffer init --- */
    if (!(mb->flags & MULTIBOOT_INFO_FLAG_FRAMEBUFFER)) {
        serial_print("ERROR: no framebuffer info from bootloader\n");
        serial_flush();
        for (;;);
    }

    framebuffer_t fb;
    if (!fb_init_bgrx8888(&fb,
                          (uintptr_t)mb->framebuffer_addr,
                          mb->framebuffer_pitch,
                          mb->framebuffer_width,
                          mb->framebuffer_height,
                          mb->framebuffer_bpp)) {
        serial_print("ERROR: fb_init_bgrx8888 failed\n");
        serial_flush();
        for (;;);
    }

    fb_clear(&fb, 0, 0, 0);

    uint32_t num_flats = wad_count_flats(&wad);
    serial_print("Flats found: ");
    serial_print_uint(num_flats);
    serial_putc('\n');
    serial_flush();

    /* wad_get_flat rescans from lump 0 for each index — O(n*flats) total.
       Acceptable here: this runs once at boot, and worst case is ~260K
       iterations (192 grid cells × 1351 WAD lumps). */
    uint32_t flat_idx = 0;
    for (uint32_t row = 0; row < GRID_ROWS && flat_idx < num_flats; row++) {
        for (uint32_t col = 0; col < GRID_COLS && flat_idx < num_flats; col++) {
            const uint8_t *flat_data = wad_get_flat(&wad, flat_idx, (void *)0);
            if (flat_data) {
                flat_blit(&fb, flat_data, palette,
                          col * 64u * FLAT_SCALE,
                          row * 64u * FLAT_SCALE,
                          FLAT_SCALE);
            }
            flat_idx++;
        }
    }

    serial_print("Grid displayed on framebuffer.\n");
    serial_flush();

    for (;;);
}
