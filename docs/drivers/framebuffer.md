# Driver: Framebuffer and FB Console

**Files:** `src/fb.c`, `src/fb.h`, `src/fb_console.c`, `src/fb_console.h`
**Status:** ✅ Implemented and active in the boot path
**Last updated:** 20 Apr 2026

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [Hardware background and pixel format](#2-hardware-background-and-pixel-format)
3. [Framebuffer initialisation](#3-framebuffer-initialisation)
4. [Drawing primitives](#4-drawing-primitives)
5. [Visual test patterns](#5-visual-test-patterns)
6. [FB console](#6-fb-console)
7. [DG_DrawFrame blit (planned)](#7-dg_drawframe-blit-planned)
8. [API reference](#8-api-reference)
9. [Design decisions and gotchas](#9-design-decisions-and-gotchas)

---

## 1. Purpose

The framebuffer driver maps pixel data into a VESA linear framebuffer provided
by GRUB, exposing simple drawing primitives (`clear`, `fill_rect`). On top of
this, the FB console provides an 8×16 character cell text interface — the
kernel's graphical output path, complementing the serial port.

In the final system, the framebuffer is the surface Doom renders to via
`DG_DrawFrame`. During development it is used for boot diagnostics, colour
sanity checks, and the test pattern.

---

## 2. Hardware background and pixel format

GRUB sets up a VESA linear framebuffer before handing off to the kernel. The
Multiboot 2 header in `src/boot.s` includes a framebuffer request tag (type 5):

```asm
.align 8
.short 5                    /* type: framebuffer request  */
.short 0                    /* flags (optional)           */
.long  20                   /* size                       */
.long  1024                 /* width                      */
.long  768                  /* height                     */
.long  32                   /* depth (bits per pixel)     */
```

GRUB picks a compatible mode and provides a framebuffer tag (type 8) in the
Multiboot 2 info struct:

| Field       | Description                                                    |
| ----------- | -------------------------------------------------------------- |
| `fbi->addr`   | Physical base address of the framebuffer (`uint64_t`)       |
| `fbi->pitch`  | Bytes per scanline (may be > `width * bpp/8` due to alignment) |
| `fbi->width`  | Width in pixels                                             |
| `fbi->height` | Height in pixels                                            |
| `fbi->bpp`    | Bits per pixel (32 in our case)                             |

### Pixel format: BGRX8888

The empirically confirmed pixel format on QEMU is **BGRX8888**. Each 4-byte
pixel is stored in memory as:

```
byte offset:   +0    +1    +2    +3
               [B]   [G]   [R]   [X]   (unused / padding)
```

As a 32-bit little-endian value, a pixel with RGB components (R, G, B) is packed
as:

```c
uint32_t px = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
// 0x00RRGGBB in hex → stored as [BB][GG][RR][00] in memory
```

**To verify the format with GDB** (from `docs/debugging.md`):

```gdb
(gdb) x/4bx fb.addr
```

After calling `fb_fill_rect(fb, 0, 0, 1, 1, 255, 0, 0)` (a red pixel), the
expected output is:

```
0x00 0x00 0xFF 0x00   → Blue=0, Green=0, Red=255, unused=0 ✓
```

If the colours look wrong on screen, run `fb_test_byte_lane_probe` to identify
which byte lane maps to which colour channel.

### Pitch vs. width

`pitch` (bytes per scanline) may be larger than `width * 4`. Always use `pitch`
when computing row offsets:

```c
// Correct: use pitch for row stride
uint32_t* row = (uint32_t*)(fb->addr + y * fb->pitch + x * 4);

// Wrong: assumes no padding between rows
uint32_t* row = (uint32_t*)(fb->addr + (y * fb->width + x) * 4);
```

---

## 3. Framebuffer initialisation

```c
typedef struct {
    uint8_t*   addr;    // linear framebuffer base address
    uint32_t   pitch;   // bytes per scanline
    uint32_t   width;   // pixels
    uint32_t   height;  // pixels
    uint8_t    bpp;     // bits per pixel
    fb_pixfmt_t fmt;    // FB_PIXFMT_BGRX8888
} framebuffer_t;

bool fb_init_bgrx8888(framebuffer_t* fb,
                      uintptr_t addr, uint32_t pitch,
                      uint32_t w, uint32_t h, uint8_t bpp);
```

`fb_init_bgrx8888` validates `bpp == 32` (returns `false` otherwise) and
populates the struct. Called from `kernel_main` with values from the
`multiboot_info`:

```c
const struct mb2_tag *fb_tag = mb2_find_tag(mb, MB2_TAG_FRAMEBUFFER);
const struct mb2_tag_framebuffer *fbi =
    (const struct mb2_tag_framebuffer *)fb_tag;

framebuffer_t fb;
if (!fb_init_bgrx8888(&fb,
                      (uintptr_t)fbi->addr,
                      fbi->pitch,
                      fbi->width,
                      fbi->height,
                      fbi->bpp)) {
    for (;;);  // halt — unsupported pixel format
}
```

`fbi->addr` is `uint64_t`. The cast to `uintptr_t` is lossless on x86_64. On
QEMU the framebuffer physical address is typically in the PCI MMIO aperture
(e.g., `0xFD000000`), which is within the 4 GB identity map established by the
boot trampoline. The framebuffer is initialised early in `kernel_main`, before
interrupts, so the boot banner and memory map are displayed on screen.

---

## 4. Drawing primitives

All drawing functions check `fb != NULL` and `fb->fmt == FB_PIXFMT_BGRX8888`
before operating. They return silently if the check fails.

### `fb_clear`

Fills the entire framebuffer with a solid colour:

```c
void fb_clear(framebuffer_t* fb, uint8_t r, uint8_t g, uint8_t b) {
    uint32_t px = pack_bgrx8888(r, g, b);
    for (uint32_t y = 0; y < fb->height; y++) {
        uint32_t* row = (uint32_t*)(fb->addr + y * fb->pitch);
        for (uint32_t x = 0; x < fb->width; x++) row[x] = px;
    }
}
```

No `memset` — the pixel value must be written as a 32-bit word, not as repeated
bytes. `memset` would only work correctly for a pixel value where all four bytes
are identical (e.g., black `0x00000000`).

### `fb_fill_rect`

Fills a rectangle with a solid colour. Clips to framebuffer bounds:

```c
void fb_fill_rect(framebuffer_t* fb,
                  uint32_t x0, uint32_t y0,
                  uint32_t w, uint32_t h,
                  uint8_t r, uint8_t g, uint8_t b);
```

Clipping is done by clamping `w` and `h` if the rectangle extends beyond the
framebuffer edge. Origin coordinates outside the framebuffer cause an early
return.

---

## 5. Visual test patterns

Two test functions are available for debugging colour and pixel format issues.
Neither is called during normal boot; they are invoked manually during hardware
bring-up.

### `fb_test_byte_lane_probe`

Draws four vertical bars, each setting one byte lane to `0xFF`:

| Bar | Raw pixel value | Expected colour (BGRX) |
| --- | --------------- | ---------------------- |
| 0   | `0x000000FF`    | Blue                   |
| 1   | `0x0000FF00`    | Green                  |
| 2   | `0x00FF0000`    | Red                    |
| 3   | `0xFF000000`    | Black (unused byte)    |

If the bars appear in a different colour order, the pixel format assumption is
wrong.

### `fb_test_color_sanity`

A comprehensive multi-section colour test:

- **Top band:** 8 colour bars — White, Yellow, Cyan, Green, Magenta, Red, Blue,
  Black (the SMPTE colour bar sequence)
- **Left half:** Horizontal greyscale ramp from black to white
- **Right half:** Four coloured squares (Red, Green, Blue, White)
- **Bottom section:** 6×6 RGB cube sampler showing a range of hue/saturation
  combinations

Used to verify that colour rendering is correct end-to-end before integrating
Doom's renderer.

---

## 6. FB console

`fb_console_t` implements a character-cell text console on top of the
framebuffer. Character cell size is **8×16 pixels** (an 8×8 bitmap font doubled
vertically).

```c
typedef struct {
    framebuffer_t* fb;
    uint32_t cols;       // fb->width / 8
    uint32_t rows;       // fb->height / 16
    uint32_t cursor_x;   // column (character units)
    uint32_t cursor_y;   // row (character units)
    uint8_t  fg_r, fg_g, fg_b;   // foreground colour
    uint8_t  bg_r, bg_g, bg_b;   // background colour
    bool     show_cursor;
} fb_console_t;
```

At 1024×768, the console is 128 columns × 48 rows.

### Font

A static 8×8 bitmap font (`font8x8_basic`) covers ASCII 32–127. Each glyph is 8
bytes — one byte per row, one bit per pixel, LSB = leftmost pixel. Control
characters (0–31) and characters above 127 are rendered as blank or `?`.

### Glyph rendering (`draw_glyph8x16`)

Each font row byte is drawn twice vertically (rows 0–7 drawn at pixel rows 0–1,
2–3, 4–5, etc.), giving an 8×16 cell from an 8×8 font. Only set bits are drawn
(foreground colour); the cell background is cleared first with `fb_fill_rect`.

```c
for (uint32_t row = 0; row < 8; row++) {
    uint8_t bits = g[row];
    for (uint32_t col = 0; col < 8; col++) {
        bool on = (bits & (1u << col)) != 0;
        if (!on) continue;
        fb_fill_rect(fb, px0 + col, py0 + row*2, 1, 2,
                     con->fg_r, con->fg_g, con->fg_b);
    }
}
```

If glyphs appear mirrored horizontally, change the bit test from `(1u << col)`
to `(1u << (7 - col))`.

### Scrolling

When the cursor moves past the last row, `scroll_up_one_row` shifts the
framebuffer contents up by 16 pixels:

```c
// memmove the pixel data up by one character row, whole 32-bit pixels at a
// time (SCRUM-162) -- the framebuffer is always 32bpp, so pitch/len are
// always a multiple of 4 and there is no leftover tail to special-case.
uint32_t* dst = (uint32_t*)fb->addr;
uint32_t* src = (uint32_t*)(fb->addr + 16 * fb->pitch);
uint32_t len_px = ((fb->height - 16) * fb->pitch) / 4;
for (uint32_t i = 0; i < len_px; i++) dst[i] = src[i];

// Clear the newly exposed bottom row
fb_fill_rect(fb, 0, fb->height - 16, fb->width, 16,
             con->bg_r, con->bg_g, con->bg_b);
```

This is a direct pixel copy rather than a `memmove` call (no libc in
freestanding mode). The copy direction (low to high addresses) is correct for
upward scrolling since `dst < src`. Before SCRUM-162 this copied one `uint8_t`
at a time; see §9 below for the measured cost of that and the word-wise fix.

### Control characters

| Character | Behaviour                                                        |
| --------- | ---------------------------------------------------------------- |
| `\n`      | Move to column 0, advance row; scroll if at bottom               |
| `\r`      | Move to column 0, stay on same row                               |
| `\t`      | Advance to next 4-column tab stop via repeated `fbcon_putc(' ')` |
| `>= 128`  | Rendered as `?`                                                  |

### Cursor

An underline cursor (1-pixel-tall filled rectangle at row 15 of the current
cell) is drawn after every character output and erased before the next.
`fbcon_enable_cursor(con, false)` disables it for cleaner output during rapid
scrolling.

---

## 7. DG_DrawFrame blit

Sprint 8 (SCRUM-77, SCRUM-78). `DG_DrawFrame` (`src/libos_doom/libos_doom.c`)
blits Doom's `DG_ScreenBuffer` to the hardware framebuffer every tick,
scaling from Doom's resolution up to whatever `exo_fb_acquire` reports
(1024×768 on the QEMU VESA mode this project targets).

**Scale factor:** 1024/640 = 1.6 — not an integer, and deliberately not
letterboxed. Doom renders 320×200 (doomgeneric doubles it to 640×400 —
"Auto-scaling factor: 2" in its own startup log), and 320×200 was designed
for a 4:3 display with non-square pixels 1.2× taller than wide. 1024×768
*is* 4:3, so a full non-integer nearest-neighbour stretch reproduces the
intended geometry rather than distorting it — preserving the 640×400 pixel
grid unscaled is what would squash the picture.

**Format conversion:** none needed. Doom reports its channel offsets at
startup (red 16, green 8, blue 0, alpha 24), which — little-endian — is B,G,R,X
in memory: exactly the framebuffer's BGRX8888 layout (empirically confirmed
on QEMU). Each pixel moves as one unmodified 32-bit word.

**Scaling (SCRUM-77 → SCRUM-78):** nearest-neighbour throughout, in two
generations:

- **SCRUM-77** walked a Bresenham-style accumulator per axis inline in the
  per-pixel loop (an add, a compare, a 32-bit store) — no multiply-and-divide,
  but the same walk was re-derived from scratch on every one of the 768 rows
  of every one of the ~35 frames/sec, even though it depends only on
  `fb.width`/`fb.height` and `DOOMGENERIC_RESX`/`RESY`, never on pixel
  content.
- **SCRUM-78** hoists that walk out into two lookup tables, `x_lut[dst_x] ->
  src_x` and `y_lut[dst_y] -> src_y`, built once (`build_scale_lut()`) the
  first time the framebuffer maps successfully. The per-pixel loop is then
  just `dst_row[dx] = src_row[x_lut[dx]]` — two array reads and a store, no
  accumulator state carried across pixels at all. It also skips redundant
  work at the row level: whenever `y_lut[dy] == y_lut[dy - 1]` — true for
  most rows once the vertical scale factor exceeds 1x, and *more* rows as
  resolution grows, not fewer — that row is byte-identical to the one just
  written, so it's produced by copying the previous destination row's
  already-computed bytes (sequential, cache-friendly) instead of re-walking
  `x_lut`/`DG_ScreenBuffer` a second time for content that's already known.

**Profiling (SCRUM-78's acceptance criterion — "frame blit takes <5ms
measured via serial profiling"):** `DG_DrawFrame` times its own blit body
with `DG_GetTicksMs()` and prints an averaged per-frame cost plus the
observed max every 35 frames (`profile_report()` in `libos_doom.c`) — a
single before/after pair would mostly just read "0ms" or "1ms" against the
PIT's 1ms resolution, so this follows the same repeat-and-average approach
§9's SCRUM-162 writeup used to get sub-ms-granularity numbers out of an
ms-granularity clock.

Measured on a normal `docker-run-kernel`-style boot (ISO/GRUB, `-display
none`, keystrokes injected at the shell to launch `doom`), over several
seconds of gameplay at the Freedoom title/level screen: **average 1.9–2.4ms
per frame, max 3–4ms** (one 7ms outlier observed in one window — TCG
scheduling jitter, not a per-pixel cost; see below) at the default 1024×768
mode, comfortably meeting the 5ms budget.

At a much larger resolution (3440×1440, tested manually in a fullscreen QEMU
window) the row-duplicate skip pays off exactly as expected: **average
1.2ms, max 3ms** — *faster* than 1024×768 despite ~6x the pixel count,
because the much larger scale factor (3440/640 ≈ 5.4x, 1440/400 = 3.6x)
means the row-duplicate fast path fires for most rows. This confirms
`DG_DrawFrame` itself is not what makes large windows feel slower — if
anything it gets cheaper. Any remaining sluggishness at that size is
downstream of this function: QEMU's own cost of compositing a much larger
guest VRAM buffer onto the actual host display window, which happens after
`DG_DrawFrame` returns and is entirely outside the LibOS's control.

Worth knowing before reading too much into the absolute numbers: **this
project's QEMU never has KVM acceleration** — no `docker-*` target in
`Makefile`/`docker/` passes `--device /dev/kvm` or `-accel kvm`, so every
`make docker-run`/`docker-test`/`docker-ci` boot, this measurement included,
runs under pure TCG software emulation. That is the project's own standard,
reproducible measurement environment (the same one §9's SCRUM-162 numbers
came from), but it means these milliseconds are TCG-emulated-CPU cost, not
real-hardware or KVM-accelerated cost — real hardware would be markedly
faster, and the visible KVM-vs-TCG gap a user might notice interactively has
nothing to do with `DG_DrawFrame`'s own cost, which these numbers show is
small and shrinking (relatively) as resolution grows.

---

## 8. API reference

### `fb.h`

```c
bool fb_init_bgrx8888(framebuffer_t* fb, uintptr_t addr,
                      uint32_t pitch, uint32_t w, uint32_t h, uint8_t bpp);
```

Initialise a `framebuffer_t`. Returns `false` if `bpp != 32` or `fb == NULL`.

---

```c
void fb_clear(framebuffer_t* fb, uint8_t r, uint8_t g, uint8_t b);
```

Fill entire framebuffer with solid colour (r, g, b).

---

```c
void fb_fill_rect(framebuffer_t* fb,
                  uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  uint8_t r, uint8_t g, uint8_t b);
```

Fill rectangle at (x, y) with dimensions w×h. Clipped to framebuffer bounds.

---

```c
void fb_test_byte_lane_probe(framebuffer_t* fb);
```

Draw four vertical bars, one per byte lane. Use to identify pixel format.

---

```c
void fb_test_color_sanity(framebuffer_t* fb);
```

Draw colour bars, greyscale ramp, colour squares, and RGB cube sampler.

---

### `fb_console.h`

```c
bool fbcon_init(fb_console_t* con, framebuffer_t* fb);
```

Initialise a text console. Sets white-on-black colours, cursor enabled, clears
screen. Returns `false` if `con`, `fb`, or computed dimensions are invalid.

---

```c
void fbcon_set_color(fb_console_t* con,
                     uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                     uint8_t bg_r, uint8_t bg_g, uint8_t bg_b);
```

Set foreground and background colours. Takes effect on subsequent character
output.

---

```c
void fbcon_clear(fb_console_t* con);
```

Clear screen to background colour, reset cursor to (0, 0).

---

```c
void fbcon_putc(fb_console_t* con, char c);
```

Output one character. Handles `\n`, `\r`, `\t`. Characters ≥ 128 rendered as
`?`.

---

```c
void fbcon_write(fb_console_t* con, const char* s);
```

Output a NUL-terminated string via repeated `fbcon_putc`.

---

```c
void fbcon_enable_cursor(fb_console_t* con, bool enable);
```

Show or hide the underline cursor.

---

```c
void fbcon_redraw_cursor(fb_console_t* con);
```

Redraw the cursor at the current position. Called internally after every
character; exposed for external use if the cursor needs refreshing after direct
framebuffer writes.

---

## 9. Design decisions and gotchas

**Why not use `memset` for `fb_clear`?** `memset` fills byte-by-byte. For a
non-black colour, the four bytes of a BGRX8888 pixel are different values, so a
single `memset` call would not produce valid pixels. A correct alternative would
be to use `memset` only for black (`0x00000000`) and a word-fill loop for
colours, but the current row-by-row word loop is cleaner and universally
correct.

**No double-buffering.** All drawing goes directly to the hardware framebuffer.
This means screen tearing is possible during fast updates. For Doom's 35
tics/sec game loop on a 60 Hz display this is not a severe issue, but a future
optimisation could add a back-buffer and `memcpy` on `DG_DrawFrame`.

**Scrolling used to be byte-wise slow (SCRUM-162).** `scroll_up_one_row`
copies `(768 - 16) * pitch ≈ 3 MB` of pixel data every time the text console
scrolls. Measured with `kernel_get_ticks_ms()` around the copy on a normal
QEMU boot (18 forced scrolls from the boot banner/mmap dump/self-check
output): the pre-SCRUM-162 byte-at-a-time copy cost **4-7 ms per scrolled
line**, matching the ticket's original estimate and the direct cause of the
boot timer demo's cadence drift diagnosed in SCRUM-163 (`kernel_sleep_ms(1000)`
iterations that print-then-scroll cost ~1006 ms, not 1000). SCRUM-162 switched
the copy to whole 32-bit pixels (§"Scrolling" above), the same wide-store
idiom `fb_clear`/`fb_fill_rect` already use; the identical measurement
afterward dropped to **0-2 ms per scrolled line** — roughly a 3-4x
improvement, consistent with trading 4 MMIO stores/pixel for 1. It is still
`(768 - 16) * pitch` of uncached MMIO traffic every scroll — a circular
buffer that tracks a virtual top-of-screen offset instead of physically
moving pixels remains the option if high-frequency output ever needs more
headroom than a wide-store loop buys — but the per-scroll cost is no longer
dominated by single-byte stores.

**`DG_DrawFrame`'s per-row accumulator walk used to be redone every frame
(SCRUM-78).** Full measurement and the precomputed-LUT fix are in §7 above;
noted here because it's the same shape of mistake as SCRUM-162's scrolling
copy — repeated work that doesn't depend on the data being processed, caught
by the same technique (measure with `DG_GetTicksMs()`/`kernel_get_ticks_ms()`
around the hot path, averaged over many repeats to beat 1ms PIT resolution).

**Framebuffer is active in the boot path.** The framebuffer is initialised early
in `kernel_main` (before interrupts) and displays the boot banner, BIOS memory
map, allocator info, and a 9-second timer countdown demo. The `fb_console_t` is
fully exercised during every normal boot.

**`exo_fb_acquire` syscall (SCRUM-154).** The LibOS does not access the
framebuffer by reading `multiboot_info`. It calls `exo_fb_acquire(info_out)`,
which writes the physical address, pitch, width, height and bpp to a user-space
struct, then maps the physical pages into its own address space via
`exo_page_map`. This keeps the LibOS from knowing or assuming the framebuffer's
physical address, and lets the kernel enforce exclusive access.

The exclusivity is not advisory. `exo_fb_acquire` **binds** the framebuffer to
the calling context in `src/fb_binding.c`, and `fb_binding_check_map()` is the
gate `exo_page_map` consults before mapping any physical page: framebuffer
pages are mappable only by the LibOS that acquired them, and only after it has
acquired them — an unheld framebuffer denies too. A second acquirer gets
`-EBUSY`; a machine the bootloader gave no framebuffer answers `-ENODEV`; the
owner may re-acquire without penalty. `fb_binding_release()` drops the binding
when the owner exits (SCRUM-155), so a LibOS that dies holding the screen does
not lock the display for the rest of the boot. Full mechanism in
`docs/syscall_spec.md` §3.5; the enforcement point inside `exo_page_map` itself
lands with SCRUM-153.

**Taking the screen back.** The binding is revocable (SCRUM-156,
`docs/syscall_spec.md` §3.6). `fb_binding_revoke_mark(who)` records that the
kernel wants the framebuffer back without disturbing the binding — a marked
owner still passes `fb_binding_check_map()`, so it can finish the frame it is
drawing before handing over — and `fb_binding_reclaim(who)` takes it, reporting
whether there was anything to take. Both are scoped to the named holder: a
reclaim on behalf of one context never drops another's binding.
`fb_binding_release()` is the voluntary form of the same operation, and
`revoke_all(context)` in `src/revoke.h` takes the framebuffer alongside the
context's pages.

Framebuffer memory needs this separate table rather than the PMM's per-page
owner tags (SCRUM-152) for a concrete reason: the framebuffer is MMIO, outside
the usable-RAM region `page_alloc_init()` manages, so `page_owner()` reports
`PAGE_OWNER_FREE` for every framebuffer page and cannot speak for them.

**The kernel console still owns the screen.** Nothing yet stops `fb_console`
from drawing after a LibOS has acquired the framebuffer — the kernel reaches it
through the identity map, not through `exo_page_map`, so the binding does not
see those writes. The mechanism for handing the screen over and taking it back
now exists (SCRUM-156, above); what is missing is the *policy* that makes
`fb_console` stand down while a LibOS holds the binding, and the upcall that
would let the kernel ask for it back mid-frame (SCRUM-147).
