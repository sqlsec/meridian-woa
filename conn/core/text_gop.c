// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Abdelkader Boudih <oss@seuros.com>
// SPDX-FileCopyrightText: 2026 meridian-woa contributors

#include "conn_core.h"
#include "obsidian_font.h"

#define CH 27

/* Obsidian draws directly into the normal Conn GOP surface. No disk assets,
 * filesystem access, allocation, floating point or changes to boot dispatch.
 *
 * The layout is authored on the 1104x723 design canvas (conn_text_width/height)
 * and drawn at the surface's own resolution: design lengths go through OS(),
 * design y positions through OY(), and text uses the baked Inter set closest to
 * the scale, so a 2944x1840 panel gets native pixels instead of an upscale. */
#define OBS_DESIGN_W 1104
#define OBS_DESIGN_H 723

static UINT32 gObsScale = 1024; /* surface px per design px, 1/1024 units */
static INTN gObsOy = 0;         /* vertical centring when the surface is taller */
static UINTN gObsSet = 0;       /* index into the baked font sets */

#define OS(v) ((INTN)(((INT64)(v) * (INT64)gObsScale) / 1024))
#define OY(v) (gObsOy + OS(v))

static VOID obs_set_scale(UINTN sw, UINTN sh)
{
    UINT64 sx = ((UINT64)sw << 10) / OBS_DESIGN_W;
    UINT64 sy = ((UINT64)sh << 10) / OBS_DESIGN_H;
    UINT32 best = 0xFFFFFFFFu;

    gObsScale = (UINT32)(sx < sy ? sx : sy);
    if (gObsScale < 512) gObsScale = 512;
    gObsOy = ((INTN)sh - OS(OBS_DESIGN_H)) / 2;
    if (gObsOy < 0) gObsOy = 0;
    gObsSet = 0;
    for (UINTN i = 0; i < OBS_FONT_SETS; i++) {
        UINT32 d = obs_font_set_scale[i] > gObsScale ? obs_font_set_scale[i] - gObsScale
                                                     : gObsScale - obs_font_set_scale[i];
        if (d < best) { best = d; gObsSet = i; }
    }
}

typedef struct {
    INTN x, y, w, h, gap, shown, start;
} ObsidianLayout;

/* Card column in surface pixels. Horizontal margins stay proportional to the
 * surface width so wide panels are filled instead of pillarboxed. */
static ObsidianLayout obs_layout(const ConnEntryList *list, UINTN selected, UINTN sw)
{
    ObsidianLayout l;
    INTN count = list != NULL ? (INTN)list->count : 0;
    l.x = (INTN)((sw * 164) / OBS_DESIGN_W);
    l.w = (INTN)sw - 2 * l.x;
    l.shown = count < 5 ? count : 5;
    l.h = OS(count <= 3 ? 100 : 62);
    l.gap = OS(14);
    l.y = OY(count <= 2 ? 246 : count == 3 ? 218 : 202);
    l.start = 0;
    if (count > l.shown) {
        l.start = (INTN)selected - l.shown / 2;
        if (l.start < 0) l.start = 0;
        if (l.start > count - l.shown) l.start = count - l.shown;
    }
    return l;
}

INTN conn_obsidian_hit_index_px(const ConnEntryList *list, UINTN selected, INTN x, INTN y,
                                UINTN sw, UINTN sh)
{
    obs_set_scale(sw, sh);
    ObsidianLayout l = obs_layout(list, selected, sw);
    if (list == NULL || x < l.x || x >= l.x + l.w || y < l.y) return -1;
    INTN row = (y - l.y) / (l.h + l.gap);
    if (row >= l.shown || (y - l.y) % (l.h + l.gap) >= l.h) return -1;
    return l.start + row;
}

INTN conn_obsidian_hit_index(const ConnEntryList *list, UINTN selected, INTN x, INTN y)
{
    return conn_obsidian_hit_index_px(list, selected, x, y, OBS_DESIGN_W, OBS_DESIGN_H);
}

static UINT32 obs_isqrt(UINT64 v)
{
    UINT64 x = v, r = 0, b = (UINT64)1 << 62;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return (UINT32)r;
}

static VOID obs_px(ConnSurface *s, INTN x, INTN y, ConnColor c, UINT32 cov)
{
    UINT32 a = (CONN_A(c) * cov) / 255u;
    if (a == 0) return;
    conn_fill_rect(s, x, y, 1, 1, CONN_RGBA(CONN_R(c), CONN_G(c), CONN_B(c), a));
}

/* Rounded rectangle with anti-aliased corner edges (coverage per edge pixel). */
static VOID obs_round(ConnSurface *s, INTN x, INTN y, INTN w, INTN h, INTN r, ConnColor c)
{
    if (w <= 0 || h <= 0) return;
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;
    if (r < 0) r = 0;
    for (INTN row = 0; row < h; row++) {
        INTN dyc = row < r ? (r - row) * 256 - 128 : row >= h - r ? (row - (h - r)) * 256 + 128 : 0;
        if (dyc <= 0) {
            conn_fill_rect(s, x, y + row, w, 1, c);
            continue;
        }
        UINT64 rr = (UINT64)r * 256 * (UINT64)r * 256;
        UINT64 dd = (UINT64)dyc * (UINT64)dyc;
        INTN half = dd >= rr ? 0 : (INTN)obs_isqrt(rr - dd); /* 1/256 px */
        INTN inset = r * 256 - half;
        INTN full = inset >> 8, frac = inset & 255;
        INTN solid = full + (frac ? 1 : 0);
        if (frac && full < w - full) {
            UINT32 cov = (UINT32)(255 - frac);
            obs_px(s, x + full, y + row, c, cov);
            obs_px(s, x + w - 1 - full, y + row, c, cov);
        }
        if (w - 2 * solid > 0) conn_fill_rect(s, x + solid, y + row, w - 2 * solid, 1, c);
    }
}

static UINT32 obs_cp(const char **p)
{
    const UINT8 *u = (const UINT8 *)*p;
    UINT32 cp = u[0];
    UINTN n = 1;
    if (u[0] >= 0xC2 && u[0] < 0xE0 && u[1] != 0) {
        cp = ((u[0] & 31u) << 6) | (u[1] & 63u); n = 2;
    } else if (u[0] >= 0xE0 && u[0] < 0xF0 && u[1] != 0 && u[2] != 0) {
        cp = ((u[0] & 15u) << 12) | ((u[1] & 63u) << 6) | (u[2] & 63u); n = 3;
    }
    *p += n;
    return cp;
}

static INTN obs_face_height(UINTN face) { return obs_font_height[gObsSet][face]; }

static VOID obs_text_face(ConnSurface *s, INTN x, INTN y, const char *text,
                          ConnColor ink, UINTN face, INTN max_cols)
{
    if (text == NULL) return;
    INTN pen = x * 64;
    for (INTN n = 0; *text && n < max_cols; n++) {
        UINT32 cp = obs_cp(&text);
        UINTN gi;
        for (gi = 0; gi < OBS_FONT_GLYPHS; gi++) if (obs_font_cp[gi] == cp) break;
        if (gi == OBS_FONT_GLYPHS) continue;
        UINTN width = obs_font_width[gObsSet][face][gi], height = obs_font_height[gObsSet][face];
        const UINT8 *cover = obs_font_pixels + obs_font_offset[gObsSet][face][gi];
        for (UINTN py = 0; py < height; py++) {
            INTN yy = y + (INTN)py;
            if (yy < 0 || yy >= (INTN)s->h) continue;
            for (UINTN px = 0; px < width; px++) {
                INTN xx = (pen + 32) / 64 + (INTN)px;
                if (xx < 0 || xx >= (INTN)s->w) continue;
                UINT32 a = cover[py * width + px];
                if (a == 0) continue;
                EFI_GRAPHICS_OUTPUT_BLT_PIXEL *p = s->px + (UINTN)yy * s->w + (UINTN)xx;
                p->Red = (UINT8)((CONN_R(ink) * a + p->Red * (255 - a)) / 255);
                p->Green = (UINT8)((CONN_G(ink) * a + p->Green * (255 - a)) / 255);
                p->Blue = (UINT8)((CONN_B(ink) * a + p->Blue * (255 - a)) / 255);
                p->Reserved = 0;
            }
        }
        pen += obs_font_advance[gObsSet][face][gi];
    }
}

static VOID obs_text(ConnSurface *s, INTN x, INTN y, const char *text,
                     ConnColor ink, BOOLEAN bold, INTN max_cols)
{
    obs_text_face(s, x, y, text, ink, bold ? 1 : 0, max_cols);
}

static BOOLEAN obs_contains(const char *s, const char *needle)
{
    if (s == NULL) return FALSE;
    for (; *s; s++) {
        UINTN i = 0;
        for (; needle[i]; i++) {
            char c = s[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
            if (c != needle[i]) break;
        }
        if (needle[i] == 0) return TRUE;
    }
    return FALSE;
}

static INTN obs_kind(const ConnEntry *e)
{
    if (obs_contains(e->name, "windows") || obs_contains(e->loader, "bootmgfw")) return 1;
    if (obs_contains(e->name, "omarchy")) return 2;
    return 0;
}

static ConnColor obs_accent(const ConnEntry *e)
{
    INTN kind = obs_kind(e);
    return kind == 1 ? CONN_RGB(0x70, 0xB8, 0xFF)
         : kind == 2 ? CONN_RGB(0xA3, 0xCE, 0x7B) : CONN_RGB(0xAC, 0xB5, 0xC4);
}

static VOID obs_icon(ConnSurface *s, INTN x, INTN y, INTN size, INTN kind, ConnColor ink)
{
    if (kind == 1) {
        INTN gap = OS(5), pane = (size - gap) / 2;
        for (INTN row = 0; row < 2; row++)
            for (INTN col = 0; col < 2; col++)
                obs_round(s, x + col * (pane + gap), y + row * (pane + gap),
                          pane, pane, OS(2), ink);
    } else if (kind == 2) {
        /* Lossless 15x15 cell geometry from conn/art/splash/omarchy.svg.
         * Keep the actual Omarchy labyrinth; do not substitute an Arch logo. */
        static const UINT8 rects[][4] = {
            {0,0,15,1}, {0,1,1,14}, {7,1,1,2}, {14,1,1,14},
            {2,2,5,1}, {11,2,2,1}, {2,3,1,10}, {12,3,1,10},
            {1,7,1,1}, {3,12,9,1}, {7,13,1,2}, {1,14,6,1}, {9,14,5,1}
        };
        for (UINTN i = 0; i < sizeof rects / sizeof rects[0]; i++) {
            INTN a = rects[i][0], b = rects[i][1];
            INTN c = a + rects[i][2], d = b + rects[i][3];
            conn_fill_rect(s, x + a * size / 15, y + b * size / 15,
                           c * size / 15 - a * size / 15,
                           d * size / 15 - b * size / 15, ink);
        }
    } else {
        INTN in = OS(3);
        obs_round(s, x, y + size / 8, size, size * 3 / 4, OS(7), ink);
        obs_round(s, x + in, y + size / 8 + in, size - 2 * in, size * 3 / 4 - 2 * in, OS(4),
                  CONN_RGB(0x16, 0x1A, 0x21));
        conn_fill_rect(s, x + size * 3 / 4, y + size / 2, in, in, ink);
    }
}

static VOID obs_seconds(char *out, UINT32 seconds)
{
    UINTN n = 0;
    char digits[10];
    do { digits[n++] = (char)('0' + seconds % 10); seconds /= 10; } while (seconds && n < 10);
    UINTN j = 0;
    while (n > 0) out[j++] = digits[--n];
    out[j++] = 's'; out[j] = 0;
}

#define OBS_ARROW_RIGHT "\xE2\x86\x92"
#define OBS_ARROW_UP "\xE2\x86\x91"
#define OBS_ARROW_DOWN "\xE2\x86\x93"

static VOID obs_render(ConnSurface *s, const ConnEntryList *list, const ConnModel *model)
{
    const ConnColor text = CONN_RGB(0xEC, 0xEF, 0xF4);
    const ConnColor dim = CONN_RGB(0x85, 0x8E, 0x9D);
    const ConnColor muted = CONN_RGB(0x50, 0x59, 0x68);
    const ConnColor bg = CONN_RGB(0x0C, 0x0E, 0x12);
    obs_set_scale(s->w, s->h);
    ObsidianLayout l = obs_layout(list, model->ui.selected, s->w);
    INTN line = OS(1) > 0 ? OS(1) : 1;
    conn_clear(s, bg);

    obs_round(s, l.x, OY(75), OS(7), OS(7), OS(3), CONN_RGB(0xB4, 0xC6, 0xDD));
    obs_text(s, l.x + OS(24), OY(65), "M E R I D I A N", dim, FALSE, 24);
    obs_text_face(s, l.x, OY(118), "Choose your system", text, 2, 32);
    obs_text(s, l.x, OY(161), "A quiet beginning.", muted, FALSE, 32);

    for (INTN i = 0; i < l.shown; i++) {
        INTN idx = l.start + i, y = l.y + i * (l.h + l.gap);
        const ConnEntry *e = &list->entry[idx];
        BOOLEAN selected = (UINTN)idx == model->ui.selected;
        BOOLEAN tall = l.h >= OS(90);
        ConnColor accent = obs_accent(e);
        INTN kind = obs_kind(e), icon_size = OS(tall ? 48 : 30);
        ConnColor fill = selected ? CONN_RGB(0x1B, 0x21, 0x2A) : CONN_RGB(0x12, 0x16, 0x1C);
        obs_round(s, l.x, y + OS(5), l.w, l.h, OS(18), CONN_RGBA(0,0,0,55));
        obs_round(s, l.x, y, l.w, l.h, OS(18),
                  selected ? CONN_RGB(0x43, 0x53, 0x69) : CONN_RGB(0x23, 0x29, 0x33));
        obs_round(s, l.x + line, y + line, l.w - 2 * line, l.h - 2 * line, OS(18) - line, fill);
        if (selected) {
            INTN bar = l.h - OS(52) > OS(8) ? l.h - OS(52) : OS(8);
            obs_round(s, l.x + line, y + OS(26), OS(3), bar, OS(1), accent);
        }
        obs_icon(s, l.x + OS(30), y + (l.h - icon_size) / 2, icon_size, kind, accent);
        const char *name = kind == 1 ? "Windows 11" : kind == 2 ? "Omarchy Linux" : e->name;
        INTN text_x = l.x + OS(tall ? 106 : 80);
        obs_text_face(s, text_x, y + OS(tall ? 19 : 17), name,
                      selected ? text : dim, 1, 36);
        if (tall) {
            const char *sub = kind == 1 ? "Microsoft Windows"
                             : kind == 2 ? "Arch Linux / Omarchy"
                             : e->sub[0] ? e->sub : e->loader;
            obs_text(s, text_x, y + OS(51), sub, muted, FALSE, 36);
        }
        if (e->locked) {
            obs_text(s, l.x + l.w - OS(112), y + (l.h - obs_face_height(0)) / 2, "LOCKED", muted,
                     FALSE, 6);
        }
        else if (selected) {
            INTN bw = OS(35), bh = OS(34);
            INTN bx = l.x + l.w - OS(63), by = y + (l.h - bh) / 2;
            obs_round(s, bx, by, bw, bh, OS(9), CONN_RGB(0x2B, 0x34, 0x42));
            obs_text_face(s, bx + (bw - OS(13)) / 2, by + (bh - obs_face_height(1)) / 2,
                          OBS_ARROW_RIGHT, text, 1, 1);
        }
    }

    if (l.shown == 0) obs_text(s, l.x + OS(140), OY(320), "No bootable systems found", dim, FALSE, 40);
    if (list->count > (UINTN)l.shown) {
        if (l.start > 0) obs_text(s, l.x + l.w + OS(20), l.y, OBS_ARROW_UP, dim, TRUE, 1);
        if (l.start + l.shown < (INTN)list->count)
            obs_text(s, l.x + l.w + OS(20), l.y + (l.shown - 1) * (l.h + l.gap), OBS_ARROW_DOWN,
                     dim, TRUE, 1);
    }

    INTN bottom = OY(OBS_DESIGN_H - 118);
    if (model->ui.status == CONN_UI_LAUNCH_FAILED) {
        obs_text(s, l.x, bottom, "Launch failed. Choose another entry or press E.",
                 CONN_RGB(0xE0, 0xA0, 0x98), FALSE, 58);
    } else if (model->ui.autoboot && !model->ui.cancelled && list->count > 0) {
        char seconds[12];
        obs_seconds(seconds, model->ui.countdown_s);
        obs_text(s, l.x, bottom, "Starting selected system in", dim, FALSE, 32);
        obs_text(s, l.x + OS(227), bottom - OS(3), seconds, text, TRUE, 12);
        obs_round(s, l.x, bottom + OS(37), l.w, OS(2), OS(1), CONN_RGB(0x25, 0x2D, 0x39));
        obs_round(s, l.x, bottom + OS(37), OS(44), OS(2), OS(1),
                  obs_accent(&list->entry[model->ui.selected < list->count ? model->ui.selected : 0]));
    }
    obs_text(s, l.x, OY(OBS_DESIGN_H - 54),
             OBS_ARROW_UP OBS_ARROW_DOWN " Select   Enter Start   E Options   T Theme", muted,
             FALSE, 58);
    obs_text(s, l.x + l.w - OS(119), OY(OBS_DESIGN_H - 54), "OBSIDIAN", muted, FALSE, 12);
}

VOID conn_text_to_gop(ConnSurface *s, const ConnGrid *g, const ConnTextTheme *th) {
    conn_clear(s, th->bg);

    for (INTN row = 0; row < CONN_ROWS; row++) {
        INTN y = conn_row_y(row);
        for (INTN col = 0; col < CONN_COLS; col++) {
            const ConnCell *c = &g->cell[row][col];
            INTN x  = conn_col_x(col);
            INTN xw = conn_col_x(col + 1) - x;
            if (CONN_A(c->bg) != 0)
                conn_fill_rect(s, x, y, xw, CH, c->bg);
            if (c->ch != 0x20)
                conn_draw_glyph(s, x, y, c->ch, c->bold, c->fg);
        }
    }

    if (th->crt) {
        conn_scanlines(s, 76);
        conn_vignette(s, 153);
    }
}

VOID conn_render_to_surface(ConnSurface *s, const ConnEntryList *list,
                            const ConnModel *model, const ConnTextTheme *th) {
    if (th == &CONN_TXT_OBSIDIAN && !conn_ui_in_saver(&model->ui)) {
        obs_render(s, list, model);
        return;
    }
    ConnGrid grid;
    conn_build_text_grid(&grid, list, model, th);
    conn_text_to_gop(s, &grid, th);
}
