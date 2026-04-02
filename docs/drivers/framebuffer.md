# Driver: Framebuffer and FB Console

**Files:** `src/fb.c`, `src/fb.h`, `src/fb_console.c`, `src/fb_console.h`
**Status:** ✅ Implemented — not yet wired into normal boot path (Sprint 2+)
**Last updated:** 2 Apr 2026

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
multiboot header in `src/boot.s` requests:

```asm
.long 0      // mode_type: 0 = linear graphics (not text mode)
.long 1024   // width
.long 768    // height
.long 32     // depth (bits per pixel)
```

GRUB picks a compatible mode and fills the `multiboot_info` struct:

| Field                    | Description                                                    |
| ------------------------ | -------------------------------------------------------------- |
| `mb->framebuffer_addr`   | Physical base address of the framebuffer (`uint64_t`)          |
| `mb->framebuffer_pitch`  | Bytes per scanline (may be > `width * bpp/8` due to alignment) |
| `mb->framebuffer_width`  | Width in pixels                                                |
| `mb->framebuffer_height` | Height in pixels                                               |
| `mb->framebuffer_bpp`    | Bits per pixel (32 in our case)                                |

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
framebuffer_t fb;
if (!fb_init_bgrx8888(&fb,
                      (uintptr_t)mb->framebuffer_addr,
                      mb->framebuffer_pitch,
                      mb->framebuffer_width,
                      mb->framebuffer_height,
                      mb->framebuffer_bpp)) {
    for (;;);  // halt — unsupported pixel format
}
```

`mb->framebuffer_addr` is `uint64_t`. The cast to `uintptr_t` truncates to 32
bits on i386. On QEMU the framebuffer physical address is typically in the PCI
MMIO aperture (e.g., `0xFD000000`), which fits in 32 bits. On machines with > 4
GiB RAM the framebuffer could be above 4 GiB — pre-paging this is inaccessible
on i386 and `fb_init_bgrx8888` would silently produce a bad pointer. In practice
QEMU will not place the framebuffer above 32-bit addressable space.

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
// memmove the pixel data up by one character row
uint8_t* dst = fb->addr;
uint8_t* src = fb->addr + 16 * fb->pitch;
uint32_t len = (fb->height - 16) * fb->pitch;
for (uint32_t i = 0; i < len; i++) dst[i] = src[i];

// Clear the newly exposed bottom row
fb_fill_rect(fb, 0, fb->height - 16, fb->width, 16,
             con->bg_r, con->bg_g, con->bg_b);
```

This is a direct pixel copy rather than a `memmove` call (no libc in
freestanding mode). The copy direction (low to high addresses) is correct for
upward scrolling since `dst < src`.

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

## 7. DG_DrawFrame blit (planned)

Sprint 8 (SCRUM-77, SCRUM-78). `DG_DrawFrame` must blit Doom's `DG_ScreenBuffer`
(640×400 RGBA8888) to the hardware framebuffer (1024×768 BGRX8888) with integer
scaling.

**Scale factor:** 1024/640 = 1.6 — not an integer. Options:

- **Integer scale ×2 + letterbox:** Scale to 1280×800 — too large for 1024×768.
- **Integer scale ×1 + centred:** 640×400 centred in 1024×768 (192px left
  margin, 184px top margin). Simple, no distortion.
- **Nearest-neighbour non-integer scale:** Scale to 1024×640, centred vertically
  (64px top/bottom margin). Slightly distorted but fills width.

SCRUM-77 specifies scaling to 1024×768. The nearest-neighbour approach at
non-integer scale is the most likely implementation.

**Format conversion per pixel:** Doom outputs RGBA8888 (`0xRRGGBBAA`). The
hardware expects BGRX8888 (`0x00RRGGBB` as a 32-bit value). The alpha channel is
unused (always 0xFF from Doom). Per-pixel conversion:

```c
// Doom: pixel = 0xRRGGBBAA (big-endian field order, but stored little-endian)
// Actual 32-bit value in memory: 0xAABBGGRR (little-endian byte order)
uint32_t doom_px = DG_ScreenBuffer[src_y * 640 + src_x];
uint8_t r = (doom_px >> 0)  & 0xFF;
uint8_t g = (doom_px >> 8)  & 0xFF;
uint8_t b = (doom_px >> 16) & 0xFF;
// ignore alpha: (doom_px >> 24) & 0xFF

uint32_t hw_px = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
*(uint32_t*)(fb_addr + dst_y * pitch + dst_x * 4) = hw_px;
```

SCRUM-78 optimises this with 32-bit aligned writes and a precomputed scale table
to avoid per-pixel division.

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

**Scrolling is slow.** `scroll_up_one_row` copies `(768 - 16) * pitch ≈ 3 MB` of
pixel data every time the text console scrolls. At serial-debug speeds
(relatively infrequent output) this is fine. If the console is used for
high-frequency output, consider a circular buffer approach that tracks a virtual
top-of-screen offset instead of physically moving pixels.

**The `qemu_exit(0)` in `kernel_main` makes the framebuffer unreachable.** The
current `kernel_main` calls `qemu_exit(0)` after the timer demo, before the
framebuffer initialisation code. This is intentional — the framebuffer path is
future work — but means `fb.c` and `fb_console.c` are not exercised by the
normal boot path yet. The framebuffer code is tested by inspection and the
colour sanity test only.

**`exo_fb_acquire` syscall (Sprint 4, SCRUM-36).** The LibOS will not access the
framebuffer directly by reading `multiboot_info`. Instead it calls
`exo_fb_acquire(info_out)`, which writes the physical address, pitch, width,
height, and bpp to a user-space struct, then the LibOS maps the physical pages
into its own address space via `exo_page_map`. This keeps the LibOS from knowing
or assuming the framebuffer's physical address, and allows the kernel to enforce
exclusive access (returning `-EBUSY` if another LibOS already holds the
framebuffer).
