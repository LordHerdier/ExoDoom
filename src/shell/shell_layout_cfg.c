/*
 * shell_layout_cfg.c — see shell_layout_cfg.h (SCRUM-294).
 */
#include "shell/shell_layout_cfg.h"

#include "exo_volume.h"

/* A word of the line being parsed: text[p .. p + n). Never NUL-terminated. */
typedef struct {
    const char *p;
    size_t      n;
} tok_t;

static int tok_is(tok_t t, const char *word) {
    size_t k = 0;
    for (; word[k] != '\0'; k++) {
        if (k >= t.n || t.p[k] != word[k]) {
            return 0;
        }
    }
    return k == t.n;
}

static int tok_eq(tok_t t, const char *s) {
    size_t k = 0;
    for (; k < t.n; k++) {
        if (s[k] != t.p[k]) {
            return 0;
        }
    }
    return s[k] == '\0';
}

/* The next space/tab-separated word in [*pos, end). 0 when there is none. */
static int next_tok(const char *text, size_t *pos, size_t end, tok_t *t) {
    size_t i = *pos;
    while (i < end && (text[i] == ' ' || text[i] == '\t')) {
        i++;
    }
    if (i == end) {
        *pos = i;
        return 0;
    }
    size_t start = i;
    while (i < end && text[i] != ' ' && text[i] != '\t') {
        i++;
    }
    t->p = text + start;
    t->n = i - start;
    *pos = i;
    return 1;
}

static void set_what(shell_layout_err_t *e, tok_t t) {
    size_t n = t.n;
    size_t k = 0;
    if (n > SHELL_LAYOUT_WHAT_LEN - 1) {
        n = SHELL_LAYOUT_WHAT_LEN - 4;
    }
    for (; k < n; k++) {
        e->what[k] = t.p[k];
    }
    if (n < t.n) {
        e->what[k++] = '.';
        e->what[k++] = '.';
        e->what[k++] = '.';
    }
    e->what[k] = '\0';
}

static void set_what_str(shell_layout_err_t *e, const char *s) {
    tok_t t = { s, 0 };
    while (s[t.n] != '\0') {
        t.n++;
    }
    set_what(e, t);
}

static int fail(shell_layout_err_t *e, shell_layout_code_t code,
                uint32_t line, uint32_t other_line) {
    e->code = code;
    e->line = line;
    e->other_line = other_line;
    return (int)code;
}

/* A run of 1..max_digits decimal digits at t[*k], advancing *k. */
static int take_dec(tok_t t, size_t *k, size_t max_digits, uint32_t *out) {
    uint32_t v = 0;
    size_t n = 0;
    while (*k + n < t.n && t.p[*k + n] >= '0' && t.p[*k + n] <= '9') {
        if (n == max_digits) {
            return -1;
        }
        v = v * 10u + (uint32_t)(t.p[*k + n] - '0');
        n++;
    }
    if (n == 0) {
        return -1;
    }
    *k += n;
    *out = v;
    return 0;
}

/* Exactly four hex digits at t[*k], advancing *k. */
static int take_hex4(tok_t t, size_t *k, uint16_t *out) {
    uint32_t v = 0;
    if (*k + 4 > t.n) {
        return -1;
    }
    for (size_t n = 0; n < 4; n++) {
        char c = t.p[*k + n];
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A' + 10);
        } else {
            return -1;
        }
        v = v << 4 | d;
    }
    *k += 4;
    *out = (uint16_t)v;
    return 0;
}

static int valid_name(tok_t t) {
    if (t.n == 0 || t.n > SHELL_LAYOUT_NAME_LEN - 1) {
        return 0;
    }
    for (size_t k = 0; k < t.n; k++) {
        char c = t.p[k];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) {
            return 0;
        }
    }
    return 1;
}

/* "x,y,w,h", each digits optionally followed by '%'. */
static int parse_rect(tok_t v, shell_layout_len_t rect[4]) {
    size_t k = 0;
    for (int i = 0; i < 4; i++) {
        if (i > 0) {
            if (k >= v.n || v.p[k] != ',') {
                return -1;
            }
            k++;
        }
        uint32_t n;
        if (take_dec(v, &k, 5, &n) != 0) {
            return -1;
        }
        rect[i].pct = 0;
        if (k < v.n && v.p[k] == '%') {
            k++;
            rect[i].pct = 1;
            if (n > 100) {
                return -1;
            }
        } else if (n > SHELL_LAYOUT_PX_MAX) {
            return -1;
        }
        rect[i].value = n;
    }
    return k == v.n ? 0 : -1;
}

static int parse_input(tok_t v, shell_layout_input_t *in) {
    in->kind = SHELL_LAYOUT_IN_NONE;
    in->index = 0;
    in->vendor = 0;
    in->product = 0;

    if (tok_is(v, "none")) {
        return 0;
    }
    if (tok_is(v, "ps2")) {
        in->kind = SHELL_LAYOUT_IN_PS2;
        return 0;
    }
    if (v.n < 5 || !(v.p[0] == 'u' && v.p[1] == 's' && v.p[2] == 'b' &&
                     v.p[3] == ':')) {
        return -1;
    }
    size_t k = 4;
    int second_colon = 0;
    for (size_t j = k; j < v.n; j++) {
        if (v.p[j] == ':') {
            second_colon = 1;
        }
    }
    if (second_colon) {
        if (take_hex4(v, &k, &in->vendor) != 0 || k >= v.n ||
            v.p[k] != ':') {
            return -1;
        }
        k++;
        if (take_hex4(v, &k, &in->product) != 0 || k != v.n) {
            return -1;
        }
        in->kind = SHELL_LAYOUT_IN_USB_ID;
        return 0;
    }
    uint32_t idx;
    if (take_dec(v, &k, 2, &idx) != 0 || k != v.n ||
        idx > SHELL_LAYOUT_USB_IDX_MAX) {
        return -1;
    }
    in->kind = SHELL_LAYOUT_IN_USB_IDX;
    in->index = idx;
    return 0;
}

static int same_input(const shell_layout_input_t *a,
                      const shell_layout_input_t *b) {
    if (a->kind != b->kind) {
        return 0;
    }
    switch (a->kind) {
    case SHELL_LAYOUT_IN_USB_IDX:
        return a->index == b->index;
    case SHELL_LAYOUT_IN_USB_ID:
        return a->vendor == b->vendor && a->product == b->product;
    case SHELL_LAYOUT_IN_PS2:
        return 1;
    case SHELL_LAYOUT_IN_NONE:
    default:
        return 0;   /* any number of panes may read no keyboard */
    }
}

enum {
    KEY_PROGRAM = 1u << 0,
    KEY_RECT    = 1u << 1,
    KEY_INPUT   = 1u << 2,
    KEY_VOLUME  = 1u << 3,
};

/* One "pane ..." line, words in [pos, end). */
static int parse_pane(const char *text, size_t pos, size_t end, uint32_t line,
                      shell_layout_t *out, shell_layout_err_t *e) {
    tok_t t;
    if (!next_tok(text, &pos, end, &t)) {
        return fail(e, SHELL_LAYOUT_E_NO_NAME, line, 0);
    }
    if (!valid_name(t)) {
        set_what(e, t);
        return fail(e, SHELL_LAYOUT_E_BAD_NAME, line, 0);
    }
    set_what(e, t);
    if (out->count == SHELL_LAYOUT_MAX_PANES) {
        return fail(e, SHELL_LAYOUT_E_TOO_MANY, line, 0);
    }
    for (uint32_t j = 0; j < out->count; j++) {
        if (tok_eq(t, out->pane[j].name)) {
            return fail(e, SHELL_LAYOUT_E_DUP_NAME, line, out->pane[j].line);
        }
    }

    shell_layout_pane_t *p = &out->pane[out->count];
    for (size_t k = 0; k < t.n; k++) {
        p->name[k] = t.p[k];
    }
    p->name[t.n] = '\0';
    p->program[0] = '\0';
    p->input.kind = SHELL_LAYOUT_IN_NONE;
    p->input.index = 0;
    p->input.vendor = 0;
    p->input.product = 0;
    p->has_volume = 0;
    p->volume = 0;
    p->line = line;

    unsigned seen = 0;
    while (next_tok(text, &pos, end, &t)) {
        size_t eq = 0;
        while (eq < t.n && t.p[eq] != '=') {
            eq++;
        }
        tok_t key = { t.p, eq };
        tok_t val = { t.p, 0 };
        if (eq < t.n) {
            val.p = t.p + eq + 1;
            val.n = t.n - eq - 1;
        }
        unsigned bit;
        if (eq == t.n) {
            bit = 0;
        } else if (tok_is(key, "program")) {
            bit = KEY_PROGRAM;
        } else if (tok_is(key, "rect")) {
            bit = KEY_RECT;
        } else if (tok_is(key, "input")) {
            bit = KEY_INPUT;
        } else if (tok_is(key, "volume")) {
            bit = KEY_VOLUME;
        } else {
            bit = 0;
        }
        if (bit == 0) {
            set_what(e, eq == t.n ? t : key);
            return fail(e, SHELL_LAYOUT_E_UNKNOWN_KEY, line, 0);
        }
        if (seen & bit) {
            set_what(e, key);
            return fail(e, SHELL_LAYOUT_E_DUP_KEY, line, 0);
        }
        seen |= bit;

        set_what(e, val);
        switch (bit) {
        case KEY_PROGRAM:
            if (val.n == 0 || val.n > SHELL_LAYOUT_PROG_LEN - 1) {
                return fail(e, SHELL_LAYOUT_E_BAD_PROGRAM, line, 0);
            }
            for (size_t k = 0; k < val.n; k++) {
                p->program[k] = val.p[k];
            }
            p->program[val.n] = '\0';
            break;
        case KEY_RECT:
            if (parse_rect(val, p->rect) != 0) {
                return fail(e, SHELL_LAYOUT_E_BAD_RECT, line, 0);
            }
            break;
        case KEY_INPUT:
            if (parse_input(val, &p->input) != 0) {
                return fail(e, SHELL_LAYOUT_E_BAD_INPUT, line, 0);
            }
            break;
        case KEY_VOLUME: {
            size_t k = 0;
            uint32_t slot;
            if (take_dec(val, &k, 2, &slot) != 0 || k != val.n ||
                slot >= EXO_VOLUME_MAX_SLOTS) {
                return fail(e, SHELL_LAYOUT_E_BAD_VOLUME, line, 0);
            }
            p->has_volume = 1;
            p->volume = slot;
            break;
        }
        }
    }

    set_what_str(e, p->name);
    if (!(seen & KEY_PROGRAM)) {
        return fail(e, SHELL_LAYOUT_E_NO_PROGRAM, line, 0);
    }
    if (!(seen & KEY_RECT)) {
        return fail(e, SHELL_LAYOUT_E_NO_RECT, line, 0);
    }
    for (uint32_t j = 0; j < out->count; j++) {
        const shell_layout_pane_t *q = &out->pane[j];
        if (same_input(&p->input, &q->input)) {
            char buf[SHELL_LAYOUT_WHAT_LEN];
            shell_layout_input_str(&p->input, buf, sizeof(buf));
            set_what_str(e, buf);
            return fail(e, SHELL_LAYOUT_E_DUP_INPUT, line, q->line);
        }
        if (p->has_volume && q->has_volume && p->volume == q->volume) {
            e->what[0] = (char)('0' + p->volume);
            e->what[1] = '\0';
            return fail(e, SHELL_LAYOUT_E_DUP_VOLUME, line, q->line);
        }
    }
    out->count++;
    return 0;
}

int shell_layout_parse(const char *text, size_t len, shell_layout_t *out,
                       shell_layout_err_t *err) {
    shell_layout_err_t scratch;
    shell_layout_err_t *e = err != NULL ? err : &scratch;

    e->code = SHELL_LAYOUT_OK;
    e->line = 0;
    e->other_line = 0;
    e->what[0] = '\0';
    e->screen_w = 0;
    e->screen_h = 0;
    out->count = 0;
    if (text == NULL) {
        len = 0;
    }

    uint32_t line = 0;
    size_t i = 0;
    while (i < len) {
        line++;
        size_t start = i;
        while (i < len && text[i] != '\n') {
            i++;
        }
        size_t end = i;
        if (i < len) {
            i++;                        /* past the '\n' */
        }
        if (end > start && text[end - 1] == '\r') {
            end--;
        }
        if (end - start > SHELL_LAYOUT_LINE_MAX) {
            return fail(e, SHELL_LAYOUT_E_LINE_TOO_LONG, line, 0);
        }

        /* Anything goes inside a comment; outside one, printable ASCII and
         * tabs only, so every token an error quotes is safe to print. */
        size_t stop = start;
        for (; stop < end && text[stop] != '#'; stop++) {
            unsigned char c = (unsigned char)text[stop];
            if (c != '\t' && (c < 0x20 || c >= 0x7f)) {
                return fail(e, SHELL_LAYOUT_E_BAD_CHAR, line, 0);
            }
        }

        size_t pos = start;
        tok_t t;
        if (!next_tok(text, &pos, stop, &t)) {
            continue;                   /* blank or comment-only */
        }
        if (!tok_is(t, "pane")) {
            set_what(e, t);
            return fail(e, SHELL_LAYOUT_E_NOT_PANE, line, 0);
        }
        int rc = parse_pane(text, pos, stop, line, out, e);
        if (rc != 0) {
            return rc;
        }
    }
    e->what[0] = '\0';
    return 0;
}

static uint32_t pct_of(uint32_t pct, uint32_t dim) {
    return (uint32_t)((uint64_t)pct * dim / 100u);
}

static uint32_t len_px(shell_layout_len_t l, uint32_t dim) {
    return l.pct ? pct_of(l.value, dim) : l.value;
}

/* The pixel span of an origin/size pair along one axis of `dim` pixels. */
static void span(shell_layout_len_t at, shell_layout_len_t size, uint32_t dim,
                 uint32_t *start, uint32_t *extent) {
    *start = len_px(at, dim);
    if (at.pct && size.pct) {
        *extent = pct_of(at.value + size.value, dim) - *start;
    } else {
        *extent = len_px(size, dim);
    }
}

int shell_layout_resolve(const shell_layout_t *l, uint32_t screen_w,
                         uint32_t screen_h, shell_layout_rect_t *out,
                         shell_layout_err_t *err) {
    shell_layout_err_t scratch;
    shell_layout_err_t *e = err != NULL ? err : &scratch;

    e->code = SHELL_LAYOUT_OK;
    e->line = 0;
    e->other_line = 0;
    e->what[0] = '\0';
    e->screen_w = screen_w;
    e->screen_h = screen_h;

    for (uint32_t i = 0; i < l->count && i < SHELL_LAYOUT_MAX_PANES; i++) {
        const shell_layout_pane_t *p = &l->pane[i];
        shell_layout_rect_t *r = &out[i];
        span(p->rect[0], p->rect[2], screen_w, &r->x, &r->w);
        span(p->rect[1], p->rect[3], screen_h, &r->y, &r->h);

        set_what_str(e, p->name);
        if (r->w == 0 || r->h == 0) {
            return fail(e, SHELL_LAYOUT_E_RECT_EMPTY, p->line, 0);
        }
        if ((uint64_t)r->x + r->w > screen_w ||
            (uint64_t)r->y + r->h > screen_h) {
            return fail(e, SHELL_LAYOUT_E_RECT_OFFSCREEN, p->line, 0);
        }
        for (uint32_t j = 0; j < i; j++) {
            const shell_layout_rect_t *q = &out[j];
            if (r->x < q->x + q->w && q->x < r->x + r->w &&
                r->y < q->y + q->h && q->y < r->y + r->h) {
                return fail(e, SHELL_LAYOUT_E_OVERLAP, p->line,
                            l->pane[j].line);
            }
        }
    }
    e->what[0] = '\0';
    return 0;
}

/* Append as much of `s` as fits, keeping room for the NUL. */
static void put(char *buf, size_t n, size_t *pos, const char *s) {
    while (*s != '\0' && *pos + 1 < n) {
        buf[(*pos)++] = *s++;
    }
}

static void put_u32(char *buf, size_t n, size_t *pos, uint32_t v) {
    char digits[11];
    size_t k = sizeof(digits) - 1;
    digits[k] = '\0';
    do {
        digits[--k] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    put(buf, n, pos, digits + k);
}

static void put_hex4(char *buf, size_t n, size_t *pos, uint16_t v) {
    static const char hex[] = "0123456789abcdef";
    char s[5];
    for (int k = 0; k < 4; k++) {
        s[k] = hex[(v >> (12 - 4 * k)) & 0xFu];
    }
    s[4] = '\0';
    put(buf, n, pos, s);
}

size_t shell_layout_input_str(const shell_layout_input_t *in, char *buf,
                              size_t n) {
    size_t pos = 0;
    if (n == 0) {
        return 0;
    }
    switch (in->kind) {
    case SHELL_LAYOUT_IN_PS2:
        put(buf, n, &pos, "ps2");
        break;
    case SHELL_LAYOUT_IN_USB_IDX:
        put(buf, n, &pos, "usb:");
        put_u32(buf, n, &pos, in->index);
        break;
    case SHELL_LAYOUT_IN_USB_ID:
        put(buf, n, &pos, "usb:");
        put_hex4(buf, n, &pos, in->vendor);
        put(buf, n, &pos, ":");
        put_hex4(buf, n, &pos, in->product);
        break;
    case SHELL_LAYOUT_IN_NONE:
    default:
        put(buf, n, &pos, "none");
        break;
    }
    buf[pos] = '\0';
    return pos;
}

static void put_screen(char *buf, size_t n, size_t *pos,
                       const shell_layout_err_t *e) {
    put(buf, n, pos, " on a ");
    put_u32(buf, n, pos, e->screen_w);
    put(buf, n, pos, "x");
    put_u32(buf, n, pos, e->screen_h);
    put(buf, n, pos, " screen");
}

size_t shell_layout_format_err(char *buf, size_t n, const char *path,
                               const shell_layout_err_t *e) {
    size_t pos = 0;
    if (n == 0) {
        return 0;
    }
    put(buf, n, &pos, path != NULL ? path : "layout");
    if (e->line != 0) {
        put(buf, n, &pos, " line ");
        put_u32(buf, n, &pos, e->line);
    }
    put(buf, n, &pos, ": ");

    const char *w = e->what;
    switch (e->code) {
    case SHELL_LAYOUT_OK:
        put(buf, n, &pos, "ok");
        break;
    case SHELL_LAYOUT_E_LINE_TOO_LONG:
        put(buf, n, &pos, "line is longer than ");
        put_u32(buf, n, &pos, SHELL_LAYOUT_LINE_MAX);
        put(buf, n, &pos, " characters");
        break;
    case SHELL_LAYOUT_E_BAD_CHAR:
        put(buf, n, &pos, "unexpected control or non-ASCII character");
        break;
    case SHELL_LAYOUT_E_NOT_PANE:
        put(buf, n, &pos, "expected 'pane <name> ...', found '");
        put(buf, n, &pos, w);
        put(buf, n, &pos, "'");
        break;
    case SHELL_LAYOUT_E_NO_NAME:
        put(buf, n, &pos, "'pane' needs a name");
        break;
    case SHELL_LAYOUT_E_BAD_NAME:
        put(buf, n, &pos, "pane name '");
        put(buf, n, &pos, w);
        put(buf, n, &pos, "' must be 1-");
        put_u32(buf, n, &pos, SHELL_LAYOUT_NAME_LEN - 1);
        put(buf, n, &pos, " letters, digits, '.', '-' or '_'");
        break;
    case SHELL_LAYOUT_E_DUP_NAME:
        put(buf, n, &pos, "pane name '");
        put(buf, n, &pos, w);
        put(buf, n, &pos, "' is already used on line ");
        put_u32(buf, n, &pos, e->other_line);
        break;
    case SHELL_LAYOUT_E_UNKNOWN_KEY:
        put(buf, n, &pos, "unknown setting '");
        put(buf, n, &pos, w);
        put(buf, n, &pos, "' (expected program=, rect=, input= or volume=)");
        break;
    case SHELL_LAYOUT_E_DUP_KEY:
        put(buf, n, &pos, "'");
        put(buf, n, &pos, w);
        put(buf, n, &pos, "=' is given twice");
        break;
    case SHELL_LAYOUT_E_NO_PROGRAM:
        put(buf, n, &pos, "pane '");
        put(buf, n, &pos, w);
        put(buf, n, &pos, "' has no program= setting");
        break;
    case SHELL_LAYOUT_E_BAD_PROGRAM:
        put(buf, n, &pos, "program= needs a program name of 1-");
        put_u32(buf, n, &pos, SHELL_LAYOUT_PROG_LEN - 1);
        put(buf, n, &pos, " characters");
        break;
    case SHELL_LAYOUT_E_NO_RECT:
        put(buf, n, &pos, "pane '");
        put(buf, n, &pos, w);
        put(buf, n, &pos, "' has no rect= setting");
        break;
    case SHELL_LAYOUT_E_BAD_RECT:
        put(buf, n, &pos, "rect=");
        put(buf, n, &pos, w);
        put(buf, n, &pos, " must be x,y,w,h, each pixels (up to ");
        put_u32(buf, n, &pos, SHELL_LAYOUT_PX_MAX);
        put(buf, n, &pos, ") or a percentage (up to 100%)");
        break;
    case SHELL_LAYOUT_E_BAD_INPUT:
        put(buf, n, &pos, "input=");
        put(buf, n, &pos, w);
        put(buf, n, &pos, " must be ps2, none, usb:<n> (n up to ");
        put_u32(buf, n, &pos, SHELL_LAYOUT_USB_IDX_MAX);
        put(buf, n, &pos, ") or usb:<vendor>:<product> in hex");
        break;
    case SHELL_LAYOUT_E_DUP_INPUT:
        put(buf, n, &pos, "keyboard ");
        put(buf, n, &pos, w);
        put(buf, n, &pos, " is already used by the pane on line ");
        put_u32(buf, n, &pos, e->other_line);
        break;
    case SHELL_LAYOUT_E_BAD_VOLUME:
        put(buf, n, &pos, "volume=");
        put(buf, n, &pos, w);
        put(buf, n, &pos, " must be a save-volume slot from 0 to ");
        put_u32(buf, n, &pos, EXO_VOLUME_MAX_SLOTS - 1);
        break;
    case SHELL_LAYOUT_E_DUP_VOLUME:
        put(buf, n, &pos, "volume slot ");
        put(buf, n, &pos, w);
        put(buf, n, &pos, " is already used by the pane on line ");
        put_u32(buf, n, &pos, e->other_line);
        break;
    case SHELL_LAYOUT_E_TOO_MANY:
        put(buf, n, &pos, "too many panes: '");
        put(buf, n, &pos, w);
        put(buf, n, &pos, "' would be number ");
        put_u32(buf, n, &pos, SHELL_LAYOUT_MAX_PANES + 1);
        put(buf, n, &pos, ", the limit is ");
        put_u32(buf, n, &pos, SHELL_LAYOUT_MAX_PANES);
        break;
    case SHELL_LAYOUT_E_RECT_EMPTY:
        put(buf, n, &pos, "pane '");
        put(buf, n, &pos, w);
        put(buf, n, &pos, "' has zero width or height");
        put_screen(buf, n, &pos, e);
        break;
    case SHELL_LAYOUT_E_RECT_OFFSCREEN:
        put(buf, n, &pos, "pane '");
        put(buf, n, &pos, w);
        put(buf, n, &pos, "' extends past the edge");
        put_screen(buf, n, &pos, e);
        break;
    case SHELL_LAYOUT_E_OVERLAP:
        put(buf, n, &pos, "pane '");
        put(buf, n, &pos, w);
        put(buf, n, &pos, "' overlaps the pane on line ");
        put_u32(buf, n, &pos, e->other_line);
        put_screen(buf, n, &pos, e);
        break;
    case SHELL_LAYOUT_E_COUNT:
    default:
        put(buf, n, &pos, "invalid layout");
        break;
    }
    buf[pos] = '\0';
    return pos;
}
