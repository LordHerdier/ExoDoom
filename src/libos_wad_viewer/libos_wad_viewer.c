/*
 * libos_wad_viewer.c — the WAD/flat/automap showcase, running at ring 3.
 *
 * Everything src/kernel.c's old run_wad_showcase() did at ring 0 (render
 * every flat as a texture grid, then an interactive <-/-> automap viewer
 * over every MAPxx lump), now running as a real LibOS: launched by
 * libos_enter()/iretq into its own address space (src/libos_launch.c,
 * SCRUM-47/-49), talking to the kernel only through the ordinary
 * exo_syscall.h ABI -- exo_fb_acquire + exo_page_map for the screen
 * (src/libos_fb.c, SCRUM-36), exo_kbd_poll for input, exo_get_ticks for
 * timing, exo_serial_write for diagnostics.
 *
 * The one thing this LibOS cannot get through that ABI is the WAD itself --
 * v1 has no filesystem and no exo_wad_acquire syscall, so kernel_main
 * (src/kernel.c) stages it before launch: libos_map_wad() (src/libos_wad_map.c)
 * maps freedoom2.wad's *existing* physical pages read-only into this
 * LibOS's own address space at the fixed LIBOS_WAD_VADDR, and passes that
 * address plus the WAD's byte length in through libos_wad_params_t
 * (src/libos_wad_params.h), the only argument this LibOS receives. Nothing
 * else about the WAD parser needed to change to run here: wad_t (src/wad.h)
 * only ever stores offsets relative to its own `data` pointer, never an
 * absolute physical address, so src/wad.c, src/flat.c and src/automap.c link
 * into this ring-3 target and run completely unmodified.
 *
 * Built by docker/scripts/build.sh's unconditional "[2c/7]" step (not the
 * TESTING-only ring-3 targets further down) -- this is what a normal boot
 * actually shows on screen, not a test probe. Lives in its own
 * subdirectory, not directly under src/, so step 3's general src/ compile
 * loop never compiles it with -DEXO_KERNEL -- see that build step's own
 * comment.
 *
 * libos_wad_viewer_main() carries `__attribute__((section(".text.entry")))`
 * so it lands at offset 0 of the linked .text blob regardless of where it
 * falls in this file's source order or how GCC schedules everything else --
 * see tests/kernel/ring3_link_target.ld.in's own comment on why that
 * attribute exists and why source order alone (every earlier ring-3
 * target's convention) is not reliable enough here. This file must still
 * stay first in build.sh's source list for this target, though: source
 * order is what build.sh's own comment documents for every target, and
 * dropping this one from that convention silently would be surprising.
 */

#include "exo_syscall.h"
#include "libos_launch.h"
#include "libos_fb.h"
#include "libos_wad_params.h"
#include "wad.h"
#include "flat.h"
#include "automap.h"
#include "fb.h"
#include "fb_console.h"
#include "ps2.h"   /* ps2_key_t (KEY_LEFT/KEY_RIGHT) only -- no kernel-only
                    * symbol from this header is called or linked in here. */

#include <stdint.h>

__attribute__((section(".text.entry")))
void libos_wad_viewer_main(void);

/* .data, not .rodata, and non-zero-initialized so it actually lands in
 * .data rather than the zero-initialized .bss a plain `= 0` global would
 * (GCC never stores real bytes for a zero initializer): this target has no
 * other mutable global, and build.sh's objcopy step cannot embed an empty
 * .data section as a blob. Same trick tests/kernel/libc_shim_probe/
 * libc_shim_probe.c's libos_c_probe_msg uses, and like that one, this also
 * proves the compiled .data region genuinely loads at LIBOS_LAUNCH_DATA_VADDR
 * -- logged over exo_serial_write below, right after libos_wad_params_t
 * (this LibOS's real payload) is read from the very same region. */
char libos_wad_viewer_banner[] = "ring3 wad viewer: entering libos_wad_viewer_main\n";

/* ---- small ring-3-only helpers (no libc shim linked into this target) -- */

static uint32_t ring3_strlen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Fire-and-forget diagnostic, mirroring serial_print()'s ring-0 role: this
 * LibOS has no business touching COM1 directly (exo_serial_write is the only
 * door), and the return value (bytes written, or -EXO_EFAULT) is not this
 * demo's concern. */
static void ring3_log(const char *s)
{
    exo_serial_write(s, ring3_strlen(s));
}

/* No hlt/sleep syscall exists (ring 3 cannot execute `hlt` itself, and v1
 * has no scheduler for exo_yield to hand off to) -- a busy poll on the
 * monotonic tick count is the only timing primitive available here. */
static void ring3_delay_ms(uint32_t ms)
{
    int64_t start = exo_get_ticks();
    if (start < 0) return;
    while ((exo_get_ticks() - start) < (int64_t)ms) { }
}

static void write_u32(fb_console_t *con, uint32_t val)
{
    if (val == 0) { fbcon_write(con, "0"); return; }
    char buf[11];
    buf[10] = '\0';
    int i = 10;
    while (val > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
    fbcon_write(con, &buf[i]);
}

/* ---- the "Ring 3 boot" banner ------------------------------------------ */

static void ring3_boot_banner(fb_console_t *con, framebuffer_t *fb)
{
    fb_clear(fb, 0, 0, 0);
    fbcon_init(con, fb);

    fbcon_set_color(con, 100, 220, 255, 0, 0, 0);
    fbcon_write(con, "== RING 3 BOOT ==\n");
    fbcon_set_color(con, 60, 60, 60, 0, 0, 0);
    fbcon_write(con, "-----------------------------------------------"
                     "--------------------------------\n");
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, "LibOS entered at CPL 3 (libos_enter -> iretq)\n");
    fbcon_write(con, "Running on its own address space (vmm_create_address_space)\n");
    fbcon_write(con, "Framebuffer mapped via exo_fb_acquire + exo_page_map (#4, #2)\n");
    fbcon_write(con, "WAD mapped read-only, staged by the kernel before launch\n");
    fbcon_write(con, "Keyboard polled via exo_kbd_poll (#6)\n");
    fbcon_write(con, "\n");

    ring3_log("ring3 wad viewer: booted at CPL 3\n");
    ring3_delay_ms(1200);
}

/* ---- flat grid + automap, ported from the old ring-0 showcase ---------- */

static void render_flat_grid(framebuffer_t *fb, const wad_t *wad,
                             const uint8_t *palette, uint32_t num_flats)
{
    uint32_t grid_cols = fb->width  / 64u;
    uint32_t grid_rows = fb->height / 64u;

    fb_clear(fb, 0, 0, 0);

    uint32_t flat_idx = 0;
    for (uint32_t row = 0; row < grid_rows && flat_idx < num_flats; row++) {
        for (uint32_t col = 0; col < grid_cols && flat_idx < num_flats; col++) {
            const uint8_t *flat_data = wad_get_flat(wad, flat_idx, (void *)0);
            if (flat_data)
                flat_blit(fb, flat_data, palette, col * 64u, row * 64u, 1);
            flat_idx++;
        }
    }

    ring3_log("ring3 wad viewer: flat grid rendered\n");
}

static void render_automap_frame(fb_console_t *con, framebuffer_t *fb, const wad_t *wad,
                                 const char *map_name, uint32_t current_map,
                                 uint32_t num_maps)
{
    fb_clear(fb, 0, 0, 0);
    fbcon_init(con, fb);

    fbcon_set_color(con, 100, 220, 255, 0, 0, 0);
    fbcon_write(con, "ExoDoom Automap (ring 3)");
    fbcon_set_color(con, 60, 60, 60, 0, 0, 0);
    fbcon_write(con, " | ");
    fbcon_set_color(con, 255, 220, 80, 0, 0, 0);
    fbcon_write(con, map_name);
    fbcon_set_color(con, 60, 60, 60, 0, 0, 0);
    fbcon_write(con, " | ");
    fbcon_set_color(con, 140, 140, 140, 0, 0, 0);
    write_u32(con, current_map + 1);
    fbcon_write(con, "/");
    write_u32(con, num_maps);
    fbcon_set_color(con, 60, 60, 60, 0, 0, 0);
    fbcon_write(con, " | ");
    fbcon_set_color(con, 140, 140, 140, 0, 0, 0);
    fbcon_write(con, "<-/-> to navigate\n");

    fbcon_set_color(con, 60, 60, 60, 0, 0, 0);
    fbcon_write(con, "-----------------------------------------------"
                     "--------------------------------\n");

    if (automap_render(fb, wad, map_name, 40) != 0) {
        fbcon_set_color(con, 230, 50, 50, 0, 0, 0);
        fbcon_write(con, "  Could not render ");
        fbcon_write(con, map_name);
        fbcon_write(con, " (missing VERTEXES/LINEDEFS?)\n");
    }
}

/* Interactive automap viewer: <-/-> cycle MAPxx lumps. Runs until halted --
 * the last thing this LibOS does, so (like the ring-0 version it replaces)
 * it never returns and never calls libos_return(). */
static void run_automap_viewer(fb_console_t *con, framebuffer_t *fb, const wad_t *wad,
                               uint32_t num_maps)
{
    uint32_t current_map = 0;
    int need_redraw = 1;

    for (;;) {
        if (need_redraw) {
            char map_name[9];
            wad_get_map_name(wad, current_map, map_name);
            render_automap_frame(con, fb, wad, map_name, current_map, num_maps);
            need_redraw = 0;
        }

        exo_kbd_event_t ev;
        if (exo_kbd_poll(&ev) > 0 && ev.pressed) {
            if (ev.key == KEY_RIGHT && current_map + 1 < num_maps) {
                current_map++;
                need_redraw = 1;
            } else if (ev.key == KEY_LEFT && current_map > 0) {
                current_map--;
                need_redraw = 1;
            }
        }

        /* Ring 3 cannot execute `hlt` -- a tight poll loop is the only
         * option without a scheduler (exo_yield) to hand the CPU to. */
    }
}

/* ---- entry point --------------------------------------------------------- */

__attribute__((section(".text.entry")))
void libos_wad_viewer_main(void)
{
    ring3_log(libos_wad_viewer_banner);

    libos_fb_t libfb;
    if (libos_fb_map(&libfb) != 0 || libfb.vaddr == NULL) {
        ring3_log("ring3 wad viewer: framebuffer map failed\n");
        exo_syscall1(LIBOS_RETURN_SYSCALL_NUM, (uint64_t)-1);
        for (;;) { }
    }

    framebuffer_t fb;
    fb_console_t con;
    if (!fb_init_bgrx8888(&fb, (uintptr_t)libfb.vaddr, libfb.pitch,
                          libfb.width, libfb.height, libfb.bpp)) {
        ring3_log("ring3 wad viewer: fb_init_bgrx8888 failed\n");
        exo_syscall1(LIBOS_RETURN_SYSCALL_NUM, (uint64_t)-1);
        for (;;) { }
    }

    ring3_boot_banner(&con, &fb);

    const libos_wad_params_t *params =
        (const libos_wad_params_t *)(uintptr_t)LIBOS_WAD_PARAMS_VADDR;

    wad_t wad;
    if (wad_init(&wad, (const uint8_t *)(uintptr_t)params->wad_vaddr,
                (uint32_t)params->wad_size) != 0) {
        fbcon_set_color(&con, 230, 50, 50, 0, 0, 0);
        fbcon_write(&con, "ERROR: WAD parse failed.\n");
        ring3_log("ring3 wad viewer: wad_init failed\n");
        exo_syscall1(LIBOS_RETURN_SYSCALL_NUM, (uint64_t)-1);
        for (;;) { }
    }

    fbcon_write(&con, "WAD mapped read-only at ring 3: ");
    write_u32(&con, wad.numlumps);
    fbcon_write(&con, " lumps\n");

    uint32_t playpal_size;
    const uint8_t *palette = wad_find_lump(&wad, "PLAYPAL", &playpal_size);
    if (!palette) {
        fbcon_set_color(&con, 230, 50, 50, 0, 0, 0);
        fbcon_write(&con, "ERROR: PLAYPAL not found.\n");
        ring3_log("ring3 wad viewer: PLAYPAL missing\n");
        exo_syscall1(LIBOS_RETURN_SYSCALL_NUM, (uint64_t)-1);
        for (;;) { }
    }
    fbcon_write(&con, "PLAYPAL palette loaded\n");

    uint32_t num_flats = wad_count_flats(&wad);
    fbcon_write(&con, "Flats found: ");
    write_u32(&con, num_flats);
    fbcon_write(&con, "\n\n");
    ring3_delay_ms(1000);

    if (num_flats > 0) {
        render_flat_grid(&fb, &wad, palette, num_flats);
        ring3_delay_ms(3000);
    }

    uint32_t num_maps = wad_count_maps(&wad);
    if (num_maps == 0) {
        fbcon_write(&con, "No MAPxx lumps found in WAD.\n");
        ring3_log("ring3 wad viewer: no maps found\n");
        exo_syscall1(LIBOS_RETURN_SYSCALL_NUM, 0);
        for (;;) { }
    }

    ring3_log("ring3 wad viewer: entering automap loop\n");
    run_automap_viewer(&con, &fb, &wad, num_maps);

    /* unreachable: run_automap_viewer never returns */
    exo_syscall1(LIBOS_RETURN_SYSCALL_NUM, 0);
    for (;;) { }
}
