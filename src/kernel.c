#include "automap.h"
#include "fb.h"
#include "fb_console.h"
#include "flat.h"
#include "idt.h"
#include "memory.h"
#include "mmap.h"
#include "multiboot.h"
#include "pic.h"
#include "pit.h"
#include "serial.h"
#include "sleep.h"
#include "wad.h"
#include <stddef.h>
#include <stdint.h>

extern void irq0_stub();

static inline void qemu_exit(uint32_t code) {
  __asm__ volatile("outl %0, %1" : : "a"(code), "Nd"(0xF4));
}

// ── Console number formatters
// ─────────────────────────────────────────────────

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
  if (val == 0) {
    fbcon_write(con, "0");
    return;
  }
  char buf[11];
  buf[10] = '\0';
  int i = 10;
  while (val > 0) {
    buf[--i] = '0' + (val % 10);
    val /= 10;
  }
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

// Print timestamp as "SSSS.mmm"
static void write_ts(fb_console_t *con, uint32_t ms) {
  uint32_t s = ms / 1000;
  uint32_t f = ms % 1000;
  char buf[9];
  buf[8] = '\0';
  buf[7] = '0' + f % 10;
  f /= 10;
  buf[6] = '0' + f % 10;
  f /= 10;
  buf[5] = '0' + f % 10;
  buf[4] = '.';
  buf[3] = '0' + s % 10;
  s /= 10;
  buf[2] = '0' + s % 10;
  s /= 10;
  buf[1] = '0' + s % 10;
  s /= 10;
  buf[0] = '0' + s % 10;
  fbcon_write(con, buf);
}

// Print "[SSSS.mmm] " prefix in green, leave color as light gray for message
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

// ── Memory map helpers
// ────────────────────────────────────────────────────────

static const char *mmap_type_name(uint32_t type) {
  switch (type) {
  case MULTIBOOT_MMAP_AVAILABLE:
    return "usable";
  case MULTIBOOT_MMAP_RESERVED:
    return "reserved";
  case MULTIBOOT_MMAP_ACPI_RECLAIM:
    return "acpi reclaimable";
  case MULTIBOOT_MMAP_ACPI_NVS:
    return "acpi nvs";
  case MULTIBOOT_MMAP_BADRAM:
    return "bad ram";
  default:
    return "unknown";
  }
}

static void mmap_type_color(fb_console_t *con, uint32_t type) {
  switch (type) {
  case MULTIBOOT_MMAP_AVAILABLE:
    fbcon_set_color(con, 80, 210, 80, 0, 0, 0);
    break; // green
  case MULTIBOOT_MMAP_ACPI_RECLAIM:
  case MULTIBOOT_MMAP_ACPI_NVS:
    fbcon_set_color(con, 220, 190, 60, 0, 0, 0);
    break; // yellow
  case MULTIBOOT_MMAP_BADRAM:
    fbcon_set_color(con, 230, 50, 50, 0, 0, 0);
    break; // red
  default:
    fbcon_set_color(con, 140, 140, 140, 0, 0, 0);
    break; // gray
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

    if (regions[i].type == MULTIBOOT_MMAP_AVAILABLE)
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

// ── Kernel entry
// ──────────────────────────────────────────────────────────────

void kernel_main(uint32_t mb_info_addr) {
  serial_init();
  struct multiboot_info *mb = (struct multiboot_info *)mb_info_addr;
  serial_print("Kernel Booted\n");
  serial_print("ExoDoom 0.1.0 initializing...\n");

  // ── Framebuffer init ──────────────────────────────────────────────────────
  framebuffer_t fb;
  fb_console_t con;
  int have_fb = 0;

  if ((mb->flags & MULTIBOOT_INFO_FLAG_FRAMEBUFFER) &&
      fb_init_bgrx8888(&fb, (uintptr_t)mb->framebuffer_addr,
                       mb->framebuffer_pitch, mb->framebuffer_width,
                       mb->framebuffer_height, mb->framebuffer_bpp) &&
      fbcon_init(&con, &fb)) {
    have_fb = 1;
    fb_clear(&fb, 0, 0, 0);
  }

  if (!have_fb) {
    serial_print("FATAL: no framebuffer\n");
    for (;;)
      ;
  }

  // ── Boot banner ───────────────────────────────────────────────────────────
  fbcon_set_color(&con, 100, 220, 255, 0, 0, 0);
  fbcon_write(&con, "ExoDoom 0.1.0 (i386)\n");
  fbcon_set_color(&con, 60, 60, 60, 0, 0, 0);
  fbcon_write(&con, "----------------------------------------------------------"
                    "---------------------\n");

  klog(&con, 0, "Booted via GRUB Multiboot 1");

  log_prefix(&con, 0);
  fbcon_write(&con, "Framebuffer: ");
  fbcon_set_color(&con, 100, 180, 255, 0, 0, 0);
  fbcon_write_u32(&con, mb->framebuffer_width);
  fbcon_write(&con, "x");
  fbcon_write_u32(&con, mb->framebuffer_height);
  fbcon_write(&con, " ");
  fbcon_write_u32(&con, mb->framebuffer_bpp);
  fbcon_write(&con, "bpp @ 0x");
  fbcon_write_hex64(&con, mb->framebuffer_addr);
  fbcon_set_color(&con, 220, 220, 220, 0, 0, 0);
  fbcon_write(&con, "\n");

  // ── Memory map ────────────────────────────────────────────────────────────
  mmap_init(mb);
  print_mmap(&con);

  // ── Memory subsystem ──────────────────────────────────────────────────────
  memory_init();
  log_prefix(&con, 0);
  fbcon_write(&con, "Kernel allocator base: 0x");
  fbcon_set_color(&con, 100, 180, 255, 0, 0, 0);
  fbcon_write_hex32(&con, memory_base_address());
  fbcon_set_color(&con, 220, 220, 220, 0, 0, 0);
  fbcon_write(&con, "\n");

  // ── IDT / PIC / PIT ───────────────────────────────────────────────────────
  idt_init();
  klog(&con, 0, "IDT initialized (256 entries)");

  pic_remap();
  klog(&con, 0, "PIC remapped (IRQs -> vectors 0x20-0x2F)");

  idt_set_gate(32, (uint32_t)irq0_stub);
  pit_init(1000);
  klog(&con, 0, "PIT initialized at 1000 Hz (IRQ0 -> vector 0x20)");

  __asm__ volatile("sti");
  klog(&con, 0, "Interrupts enabled (STI)");

  // ── Timer demo ────────────────────────────────────────────────────────────

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
  klog(&con, kernel_get_ticks_ms(), "Timer demo complete.");

  kernel_sleep_ms(3000);

  // ── WAD / flat display ────────────────────────────────────────────────────
  if (!(mb->flags & MULTIBOOT_INFO_FLAG_MODS) || mb->mods_count == 0) {
    klog(&con, kernel_get_ticks_ms(),
         "No multiboot modules — skipping WAD display.");
    qemu_exit(0);
  }

  struct multiboot_module *mods = (struct multiboot_module *)mb->mods_addr;
  const uint8_t *wad_data = (const uint8_t *)(uintptr_t)mods[0].mod_start;
  uint32_t wad_size = mods[0].mod_end - mods[0].mod_start;

  log_prefix(&con, kernel_get_ticks_ms());
  fbcon_write(&con, "Loading WAD: ");
  fbcon_set_color(&con, 100, 180, 255, 0, 0, 0);
  fbcon_write(&con, (const char *)(uintptr_t)mods[0].cmdline);
  fbcon_set_color(&con, 220, 220, 220, 0, 0, 0);
  fbcon_write(&con, " (");
  fbcon_write_memsize(&con, wad_size);
  fbcon_write(&con, ")\n");

  wad_t wad;
  if (wad_init(&wad, wad_data, wad_size) != 0) {
    klog(&con, kernel_get_ticks_ms(), "ERROR: WAD parse failed.");
    qemu_exit(1);
  }

  log_prefix(&con, kernel_get_ticks_ms());
  fbcon_write(&con, "WAD initialized: ");
  fbcon_set_color(&con, 100, 180, 255, 0, 0, 0);
  fbcon_write_u32(&con, wad.numlumps);
  fbcon_set_color(&con, 220, 220, 220, 0, 0, 0);
  fbcon_write(&con, " lumps\n");

  uint32_t playpal_size;
  const uint8_t *palette = wad_find_lump(&wad, "PLAYPAL", &playpal_size);
  if (!palette) {
    klog(&con, kernel_get_ticks_ms(), "ERROR: PLAYPAL not found.");
    qemu_exit(1);
  }
  klog(&con, kernel_get_ticks_ms(), "PLAYPAL palette loaded");

  uint32_t num_flats = wad_count_flats(&wad);
  log_prefix(&con, kernel_get_ticks_ms());
  fbcon_write(&con, "Flats found: ");
  fbcon_set_color(&con, 100, 180, 255, 0, 0, 0);
  fbcon_write_u32(&con, num_flats);
  fbcon_set_color(&con, 220, 220, 220, 0, 0, 0);
  fbcon_write(&con, "\n\n");

  klog(&con, kernel_get_ticks_ms(), "Rendering flat texture grid...");
  kernel_sleep_ms(2000);

  // Clear to black and blit flat grid (scale=1: 64x64 per flat, 16x12 = 192
  // cells)
#define FLAT_SCALE 1
#define GRID_COLS (1024 / (64 * FLAT_SCALE))
#define GRID_ROWS (768 / (64 * FLAT_SCALE))

  fb_clear(&fb, 0, 0, 0);

  uint32_t flat_idx = 0;
  for (uint32_t row = 0; row < GRID_ROWS && flat_idx < num_flats; row++) {
    for (uint32_t col = 0; col < GRID_COLS && flat_idx < num_flats; col++) {
      const uint8_t *flat_data = wad_get_flat(&wad, flat_idx, (void *)0);
      if (flat_data)
        flat_blit(&fb, flat_data, palette, col * 64u * FLAT_SCALE,
                  row * 64u * FLAT_SCALE, FLAT_SCALE);
      flat_idx++;
    }
  }

  serial_print("Flat grid rendered.\n");
  serial_flush();

  kernel_sleep_ms(5000);

  // ── Automap display ─────────────────────────────────────────────────────
  fb_clear(&fb, 0, 0, 0);
  fbcon_init(&con, &fb);

  fbcon_set_color(&con, 100, 220, 255, 0, 0, 0);
  fbcon_write(&con, "ExoDoom Automap — ");
  fbcon_set_color(&con, 220, 220, 220, 0, 0, 0);
  fbcon_write(&con, "MAP01\n");
  fbcon_set_color(&con, 60, 60, 60, 0, 0, 0);
  fbcon_write(&con, "-----------------------------------------------------------"
                    "--------------------\n");

  klog(&con, kernel_get_ticks_ms(), "Parsing MAP01 geometry from WAD...");

  if (automap_render(&fb, &wad, "MAP01", 40) == 0) {
    klog(&con, kernel_get_ticks_ms(), "Automap rendered successfully.");
    serial_print("Automap: MAP01 rendered.\n");
  } else {
    klog(&con, kernel_get_ticks_ms(), "ERROR: Could not render MAP01 automap.");
    serial_print("Automap: MAP01 render failed.\n");
  }

  serial_flush();

  for (;;)
    ;
}
