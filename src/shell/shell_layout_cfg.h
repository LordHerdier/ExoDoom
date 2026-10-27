/*
 * shell_layout_cfg.h — the layout manifest: which program runs in which pane,
 * reading which keyboard (SCRUM-294, epic SCRUM-195).
 *
 * Not "shell_layout.h": build.sh generates that name for the shell blob's
 * memory layout (build_ring3_link_target).
 *
 * A text file in ExoFS (SHELL_LAYOUT_DEFAULT_PATH by default) declares how
 * this machine arranges program instances on screen, so the dual-Doom split
 * is configuration rather than code:
 *
 *   # comments and blank lines are ignored
 *   pane doom1  program=doom.exo  rect=0,0,100%,50%    input=ps2    volume=2
 *   pane doom2  program=doom.exo  rect=0,50%,100%,50%  input=usb:0  volume=3
 *
 * It is deliberately separate from a package's own manifest (src/exo_pkg.h):
 * that says what one program needs, this says how instances are arranged.
 *
 * This module is the format only: parse, validate, resolve percentages to
 * pixels. Reading the file and launching anything is the `layout apply`
 * ticket's (SCRUM-298), so everything here is pure string and table logic --
 * no syscalls, no libc, no heap -- and the same TU links into the ring-3
 * shell blob and, under TESTING=1, into the kernel for
 * tests/kernel/test_shell_layout_cfg_k.c, as shell_mount.c does. The format and
 * its rules are documented in docs/libos-authoring.md §8.
 */
#ifndef EXODOOM_SHELL_LAYOUT_CFG_H
#define EXODOOM_SHELL_LAYOUT_CFG_H

#include <stddef.h>
#include <stdint.h>

#define SHELL_LAYOUT_DEFAULT_PATH "/etc/layout.cfg"

/* At most this many panes: FB_LAYOUT_MAX_PANES, the kernel pane table's size
 * (src/fb_layout.h, which ring-3 code does not include; the test checks the
 * two agree). */
#define SHELL_LAYOUT_MAX_PANES   8u

/* A pane name, NUL included: CONTEXT_NAME_MAX, so the name can double as the
 * context name `pslist` shows (the test checks the two agree). */
#define SHELL_LAYOUT_NAME_LEN    16u

/* A program name as `run` accepts it, NUL included. */
#define SHELL_LAYOUT_PROG_LEN    64u

/* Longest line accepted, not counting its line ending. Every valid line is
 * far shorter; the cap exists so a corrupt file is refused with a line
 * number rather than examined for kilobytes. */
#define SHELL_LAYOUT_LINE_MAX    255u

/* Largest pixel coordinate or size accepted in rect=. Bigger than any real
 * screen, small enough that x + w cannot overflow 32 bits. */
#define SHELL_LAYOUT_PX_MAX      65535u

/* Largest N accepted in input=usb:N. The kernel enumerates at most 16 USB
 * devices; whether keyboard N exists is decided when the layout is applied. */
#define SHELL_LAYOUT_USB_IDX_MAX 15u

/* One rect= component: a pixel count, or a percentage of the screen's width
 * (x, w) or height (y, h). */
typedef struct {
    uint32_t value;
    uint8_t  pct;        /* 1: `value` is a percentage, 0..100 */
} shell_layout_len_t;

typedef enum {
    SHELL_LAYOUT_IN_NONE = 0,  /* no keyboard: input=none or no input= at all */
    SHELL_LAYOUT_IN_PS2,       /* input=ps2                                    */
    SHELL_LAYOUT_IN_USB_IDX,   /* input=usb:N, the index `lsusb` prints        */
    SHELL_LAYOUT_IN_USB_ID,    /* input=usb:VVVV:PPPP, vendor:product in hex   */
} shell_layout_in_kind_t;

/*
 * Which keyboard a pane reads. usb:N is the enumeration index, which can
 * change when keyboards move between ports; usb:VVVV:PPPP names the device
 * itself, which survives that but cannot tell two identical keyboards apart.
 * Both are accepted. Two panes naming the same source is refused here; one
 * pane naming usb:0 and another naming the vendor:product of that same
 * keyboard can only be caught when the layout is applied.
 */
typedef struct {
    shell_layout_in_kind_t kind;
    uint32_t index;            /* SHELL_LAYOUT_IN_USB_IDX */
    uint16_t vendor;           /* SHELL_LAYOUT_IN_USB_ID  */
    uint16_t product;
} shell_layout_input_t;

typedef struct {
    char                 name[SHELL_LAYOUT_NAME_LEN];
    char                 program[SHELL_LAYOUT_PROG_LEN];
    shell_layout_len_t   rect[4];      /* x, y, w, h */
    shell_layout_input_t input;
    int                  has_volume;
    uint32_t             volume;       /* save-volume slot, < EXO_VOLUME_MAX_SLOTS */
    uint32_t             line;         /* 1-based line it was declared on */
} shell_layout_pane_t;

typedef struct {
    uint32_t            count;
    shell_layout_pane_t pane[SHELL_LAYOUT_MAX_PANES];
} shell_layout_t;

/* A pane's rect in real-framebuffer pixels, once resolved. */
typedef struct {
    uint32_t x, y, w, h;
} shell_layout_rect_t;

/* What went wrong. Parse errors stop at the first one. */
typedef enum {
    SHELL_LAYOUT_OK = 0,
    SHELL_LAYOUT_E_LINE_TOO_LONG,  /* over SHELL_LAYOUT_LINE_MAX characters   */
    SHELL_LAYOUT_E_BAD_CHAR,       /* control byte or non-ASCII               */
    SHELL_LAYOUT_E_NOT_PANE,       /* line does not start with "pane"         */
    SHELL_LAYOUT_E_NO_NAME,        /* "pane" with nothing after it            */
    SHELL_LAYOUT_E_BAD_NAME,       /* too long, or not [A-Za-z0-9._-]         */
    SHELL_LAYOUT_E_DUP_NAME,       /* name used by an earlier pane            */
    SHELL_LAYOUT_E_UNKNOWN_KEY,    /* not program=/rect=/input=/volume=       */
    SHELL_LAYOUT_E_DUP_KEY,        /* the same key twice on one line          */
    SHELL_LAYOUT_E_NO_PROGRAM,     /* program= missing                        */
    SHELL_LAYOUT_E_BAD_PROGRAM,    /* program= empty or too long              */
    SHELL_LAYOUT_E_NO_RECT,        /* rect= missing                           */
    SHELL_LAYOUT_E_BAD_RECT,       /* not four pixel/percent values           */
    SHELL_LAYOUT_E_BAD_INPUT,      /* not ps2, none, usb:N or usb:VVVV:PPPP   */
    SHELL_LAYOUT_E_DUP_INPUT,      /* keyboard bound by an earlier pane       */
    SHELL_LAYOUT_E_BAD_VOLUME,     /* not a slot number < EXO_VOLUME_MAX_SLOTS */
    SHELL_LAYOUT_E_DUP_VOLUME,     /* slot used by an earlier pane            */
    SHELL_LAYOUT_E_TOO_MANY,       /* over SHELL_LAYOUT_MAX_PANES             */
    /* shell_layout_resolve() only: */
    SHELL_LAYOUT_E_RECT_EMPTY,     /* zero width or height on this screen     */
    SHELL_LAYOUT_E_RECT_OFFSCREEN, /* extends past the screen                 */
    SHELL_LAYOUT_E_OVERLAP,        /* covers part of an earlier pane          */
    SHELL_LAYOUT_E_COUNT
} shell_layout_code_t;

/* Longest offending token an error quotes, NUL included; longer tokens are
 * cut short and end in "...". */
#define SHELL_LAYOUT_WHAT_LEN 32u

typedef struct {
    shell_layout_code_t code;
    uint32_t line;         /* 1-based; the pane's line for resolve errors   */
    uint32_t other_line;   /* the earlier pane, for DUP_* and OVERLAP; else 0 */
    char     what[SHELL_LAYOUT_WHAT_LEN]; /* offending token, or ""         */
    uint32_t screen_w;     /* resolve errors: the screen it was checked on  */
    uint32_t screen_h;
} shell_layout_err_t;

/*
 * Parse `len` bytes of manifest text into *out.
 *
 * Lines end in "\n" or "\r\n"; the last may have no line ending. A '#'
 * starts a comment that runs to the end of the line. Spaces and tabs separate
 * words; keywords and keys are lowercase. Each non-blank line is
 *
 *   pane <name> program=<p> rect=<x>,<y>,<w>,<h> [input=<in>] [volume=<n>]
 *
 * with the key=value words in any order. Returns 0 with out->count panes, or
 * the first error's SHELL_LAYOUT_E_* code with *err filled in (*out is then
 * unspecified). `err` may be NULL. Never reads past text[len - 1], so the
 * text need not be NUL-terminated and may contain NULs (an error).
 */
int shell_layout_parse(const char *text, size_t len, shell_layout_t *out,
                       shell_layout_err_t *err);

/*
 * Resolve every pane's rect against a `screen_w` x `screen_h` screen into
 * out[0 .. l->count - 1], and check the result: each rect non-empty, wholly
 * on screen, and overlapping no other pane (the kernel would let panes
 * overlap and draw the later one on top, but a manifest that does so is far
 * more likely to be a typo than a wish).
 *
 * A percentage is floor(pct * dim / 100). When x and w are both percentages
 * the right edge is computed as one percentage too (w = edge(x + w) -
 * edge(x)), so panes declared as 0%+50% and 50%+50% tile an odd-width screen
 * with no gap; likewise for y and h.
 *
 * Returns 0, or the first error's code with *err filled in. `err` may be
 * NULL.
 */
int shell_layout_resolve(const shell_layout_t *l, uint32_t screen_w,
                         uint32_t screen_h, shell_layout_rect_t *out,
                         shell_layout_err_t *err);

/* Write `in` the way input= spells it ("ps2", "usb:0", "usb:0627:0001",
 * "none") into buf, truncated to n bytes with the NUL. Returns the length
 * written. */
size_t shell_layout_input_str(const shell_layout_input_t *in, char *buf,
                              size_t n);

/*
 * Format *err as one line for the user, prefixed with `path` (or "layout"
 * when NULL): "/etc/layout.cfg line 3: unknown setting 'colour' (expected
 * program=, rect=, input= or volume=)". Truncated to n bytes with the NUL.
 * Returns the length written.
 */
size_t shell_layout_format_err(char *buf, size_t n, const char *path,
                               const shell_layout_err_t *err);

#endif /* EXODOOM_SHELL_LAYOUT_CFG_H */
