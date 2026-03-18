#include <stdint.h>
#include "multiboot.h"
#include "serial.h"
#include "fb.h"
#include "fnv1a.h"
#include "wad.h"
#include "flat.h"

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

    /* Declared at function scope so they are accessible after the module block,
       when the framebuffer section is added in Task 2. */
    wad_t wad;
    const uint8_t *palette  = (void *)0;
    const uint8_t *flat     = (void *)0;
    char flat_name[9] = {0};

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

    flat = wad_first_flat(&wad, flat_name);
    if (!flat) {
        serial_print("ERROR: no flat found\n");
        serial_flush();
        for (;;);
    }
    serial_print("First flat: ");
    serial_print(flat_name);
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

    /* Blit first flat centred on 1024x768: scale=8 → 512x512, offset (256,128) */
    flat_blit(&fb, flat, palette, 256, 128, 8);

    serial_print("Flat displayed on framebuffer.\n");
    serial_flush();

    for (;;);
}
