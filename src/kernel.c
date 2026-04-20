#include <stdint.h>
#include "multiboot2.h"
#include "serial.h"
#include "memory.h"
#include "mmap.h"

#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "ps2.h"
#include "sleep.h"
#include "fb.h"
#include "fb_console.h"

extern void irq0_stub();
extern void irq1_stub();
extern void kbd_init();

#ifdef TESTING
extern int run_tests(void);
#endif

static inline void qemu_exit(uint32_t code) {
    __asm__ volatile ("outl %0, %1" : : "a"(code), "Nd"(0xF4));
}

// ── Console number formatters ─────────────────────────────────────────────

static void fbcon_write_hex64(fb_console_t *con, uint64_t val) {
    const char hex[] = "0123456789abcdef";
    char buf[17];
    buf[16] = '\0';
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    fbcon_write(con, buf);
}

static void fbcon_write_hex32(fb_console_t *con, uint32_t val) {
    const char hex[] = "0123456789abcdef";
    char buf[9];
    buf[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    fbcon_write(con, buf);
}

static void fbcon_write_u32(fb_console_t *con, uint32_t val) {
    if (val == 0) { fbcon_write(con, "0"); return; }
    char buf[11];
    buf[10] = '\0';
    int i = 10;
    while (val > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
    fbcon_write(con, &buf[i]);
}

static void fbcon_write_memsize(fb_console_t *con, uint64_t bytes) {
    if (bytes >= 1024ULL * 1024ULL) {
        fbcon_write_u32(con, (uint32_t)(bytes / (1024ULL * 1024ULL)));
        fbcon_write(con, " MB");
    } else if (bytes >= 1024ULL) {
        fbcon_write_u32(con, (uint32_t)(bytes / 1024ULL));
        fbcon_write(con, " KB");
    } else {
        fbcon_write_u32(con, (uint32_t)bytes);
        fbcon_write(con, " B");
    }
}

static void write_ts(fb_console_t *con, uint32_t ms) {
    uint32_t s = ms / 1000;
    uint32_t f = ms % 1000;
    char buf[9];
    buf[8] = '\0';
    buf[7] = '0' + f % 10; f /= 10;
    buf[6] = '0' + f % 10; f /= 10;
    buf[5] = '0' + f % 10;
    buf[4] = '.';
    buf[3] = '0' + s % 10; s /= 10;
    buf[2] = '0' + s % 10; s /= 10;
    buf[1] = '0' + s % 10; s /= 10;
    buf[0] = '0' + s % 10;
    fbcon_write(con, buf);
}

static void log_prefix(fb_console_t *con, uint32_t ms) {
    fbcon_set_color(con, 80, 200, 80, 0, 0, 0);
    fbcon_write(con, "[");
    write_ts(con, ms);
    fbcon_write(con, "] ");
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
}

static void klog(fb_console_t *con, uint32_t ms, const char *msg) {
    log_prefix(con, ms);
    fbcon_write(con, msg);
    fbcon_write(con, "\n");
}

// ── Memory map helpers ────────────────────────────────────────────────────

static const char *mmap_type_name(uint32_t type) {
    switch (type) {
    case MB2_MMAP_AVAILABLE:    return "usable";
    case MB2_MMAP_RESERVED:     return "reserved";
    case MB2_MMAP_ACPI_RECLAIM: return "acpi reclaimable";
    case MB2_MMAP_ACPI_NVS:     return "acpi nvs";
    case MB2_MMAP_BADRAM:       return "bad ram";
    default:                    return "unknown";
    }
}

static void mmap_type_color(fb_console_t *con, uint32_t type) {
    switch (type) {
    case MB2_MMAP_AVAILABLE:
        fbcon_set_color(con, 80, 210, 80, 0, 0, 0); break;
    case MB2_MMAP_ACPI_RECLAIM:
    case MB2_MMAP_ACPI_NVS:
        fbcon_set_color(con, 220, 190, 60, 0, 0, 0); break;
    case MB2_MMAP_BADRAM:
        fbcon_set_color(con, 230, 50, 50, 0, 0, 0); break;
    default:
        fbcon_set_color(con, 140, 140, 140, 0, 0, 0); break;
    }
}

static void print_mmap(fb_console_t *con) {
    uint32_t count;
    const mmap_region_t *regions = mmap_get_regions(&count);

    klog(con, 0, "BIOS-provided physical RAM map:");

    uint64_t total_usable = 0;
    for (uint32_t i = 0; i < count; i++) {
        log_prefix(con, 0);
        mmap_type_color(con, regions[i].type);
        fbcon_write(con, "  [mem 0x");
        fbcon_write_hex64(con, regions[i].base);
        fbcon_write(con, "-0x");
        fbcon_write_hex64(con, regions[i].base + regions[i].length - 1);
        fbcon_write(con, "]  ");
        fbcon_write_memsize(con, regions[i].length);
        fbcon_write(con, "  ");
        fbcon_write(con, mmap_type_name(regions[i].type));
        fbcon_write(con, "\n");

        if (regions[i].type == MB2_MMAP_AVAILABLE)
            total_usable += regions[i].length;
    }

    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    log_prefix(con, 0);
    fbcon_write(con, "Total usable RAM: ");
    fbcon_set_color(con, 80, 210, 80, 0, 0, 0);
    fbcon_write_memsize(con, total_usable);
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, "\n");
}

// ── Kernel entry ──────────────────────────────────────────────────────────

void kernel_main(void *mb2_info_ptr) {
    serial_init();

    const struct mb2_info *mb = (const struct mb2_info *)mb2_info_ptr;
    serial_print("Kernel Booted (x86_64)\n");

    // ── Framebuffer init ────────────────────────────────────────────────
    const struct mb2_tag *fb_tag = mb2_find_tag(mb, MB2_TAG_FRAMEBUFFER);
    framebuffer_t fb;
    fb_console_t con;
    int have_fb = 0;

    if (fb_tag) {
        const struct mb2_tag_framebuffer *fbi =
            (const struct mb2_tag_framebuffer *)fb_tag;

        serial_print("Framebuffer: addr=0x");
        serial_print_hex64(fbi->addr);
        serial_print(" ");
        serial_print_u32(fbi->width);
        serial_print("x");
        serial_print_u32(fbi->height);
        serial_print(" ");
        serial_print_u32(fbi->bpp);
        serial_print("bpp\n");

        if (fb_init_bgrx8888(&fb,
                              (uintptr_t)fbi->addr,
                              fbi->pitch,
                              fbi->width,
                              fbi->height,
                              fbi->bpp) &&
            fbcon_init(&con, &fb)) {
            have_fb = 1;
            fb_clear(&fb, 0, 0, 0);
        }
    }

    if (!have_fb) {
        serial_print("FATAL: no framebuffer\n");
        for (;;);
    }

    // ── Boot banner ─────────────────────────────────────────────────────
    fbcon_set_color(&con, 100, 220, 255, 0, 0, 0);
    fbcon_write(&con, "ExoDoom 0.1.0 (x86_64)\n");
    fbcon_set_color(&con, 60, 60, 60, 0, 0, 0);
    fbcon_write(&con, "-----------------------------------------------"
                      "--------------------------------\n");

    klog(&con, 0, "Booted via GRUB Multiboot 2");

    log_prefix(&con, 0);
    fbcon_write(&con, "Framebuffer: ");
    fbcon_set_color(&con, 100, 180, 255, 0, 0, 0);
    {
        const struct mb2_tag_framebuffer *fbi =
            (const struct mb2_tag_framebuffer *)fb_tag;
        fbcon_write_u32(&con, fbi->width);
        fbcon_write(&con, "x");
        fbcon_write_u32(&con, fbi->height);
        fbcon_write(&con, " ");
        fbcon_write_u32(&con, fbi->bpp);
        fbcon_write(&con, "bpp @ 0x");
        fbcon_write_hex64(&con, fbi->addr);
    }
    fbcon_set_color(&con, 220, 220, 220, 0, 0, 0);
    fbcon_write(&con, "\n");

    // ── Memory map ──────────────────────────────────────────────────────
    mmap_init(mb);
    print_mmap(&con);

    // ── Memory subsystem ────────────────────────────────────────────────
    memory_init();

#ifdef TESTING
    serial_flush();
    qemu_exit((uint32_t)run_tests());
#endif

    log_prefix(&con, 0);
    fbcon_write(&con, "Kernel allocator base: 0x");
    fbcon_set_color(&con, 100, 180, 255, 0, 0, 0);
    fbcon_write_hex32(&con, (uint32_t)memory_base_address());
    fbcon_set_color(&con, 220, 220, 220, 0, 0, 0);
    fbcon_write(&con, "\n");

    // ── IDT / PIC / PIT / PS2 ─────────────────────────────────────���────
    idt_init();
    klog(&con, 0, "IDT initialized (256 entries)");

    pic_remap();
    klog(&con, 0, "PIC remapped (IRQs -> vectors 0x20-0x2F)");

    idt_set_gate(32, (uintptr_t)irq0_stub);
    pit_init(1000);
    klog(&con, 0, "PIT initialized at 1000 Hz (IRQ0 -> vector 0x20)");

    idt_set_gate(33, (uintptr_t)irq1_stub);
    kbd_init();
    klog(&con, 0, "PS/2 keyboard initialized (IRQ1 -> vector 0x21)");

    __asm__ volatile ("sti");
    klog(&con, 0, "Interrupts enabled (STI)");

    // ── Timer demo ──────────────────────────────────────────────────────
    fbcon_write(&con, "\n");
    klog(&con, 0, "Starting timer demo...");
    fbcon_write(&con, "\n");

    for (int tick = 1; tick <= 9; tick++) {
        kernel_sleep_ms(1000);
        uint32_t ms = kernel_get_ticks_ms();
        log_prefix(&con, ms);
        fbcon_write(&con, "uptime: ");
        fbcon_set_color(&con, 100, 220, 255, 0, 0, 0);
        fbcon_write_u32(&con, ms);
        fbcon_set_color(&con, 220, 220, 220, 0, 0, 0);
        fbcon_write(&con, " ms\n");
    }

    fbcon_write(&con, "\n");
    klog(&con, kernel_get_ticks_ms(), "Timer demo complete. Halting.");

    for (;;) __asm__ volatile ("hlt");
}
