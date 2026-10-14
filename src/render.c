/*
 * TermmiK
 * Copyright (C) 2026 SfymmiK
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include "alloc.h"
#include "backend.h"
#include "render.h"
#include "config.h"
#include "font_list.h"
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

// Exact integer division by 255 for v in [0, 65025] (v/255 == (v+1)*257 >> 16).
// The blend loops below run per pixel per frame — this saves a real division.
static inline uint32_t div255(uint32_t v) { return (v + 1) * 257 >> 16; }

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "stb_image.h" // declarations only — implementation lives in vt_parser.c

int g_cell_width = 9;
int g_cell_height = 18;
int g_baseline = 14;

// ---------------------------------------------------------------------------
// Background image support: the image is pre-composited once into bg_buffer
// (image blended over the background color, alpha = window opacity), so the
// per-frame cost is a plain copy and transparency still works everywhere.
// ---------------------------------------------------------------------------
static unsigned char *bg_src = NULL; // RGBA image pixels (stb_image owned)
static int bg_src_w = 0, bg_src_h = 0;
static uint32_t *bg_buffer = NULL;
static int bg_bw = 0, bg_bh = 0;
static int bg_dirty = 1;

void render_invalidate_background(void) { bg_dirty = 1; }

// Monotonic milliseconds — shared by the visual bell timing
int64_t bell_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Bilinear sample of the source image, u/v in [0,1)
static uint32_t sample_bg_image(float u, float v) {
    float x = u * bg_src_w - 0.5f;
    float y = v * bg_src_h - 0.5f;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > bg_src_w - 1) x = bg_src_w - 1;
    if (y > bg_src_h - 1) y = bg_src_h - 1;
    int x0 = (int)x, y0 = (int)y;
    int x1 = (x0 + 1 < bg_src_w) ? x0 + 1 : bg_src_w - 1;
    int y1 = (y0 + 1 < bg_src_h) ? y0 + 1 : bg_src_h - 1;
    float fx = x - x0, fy = y - y0;
    const unsigned char *p00 = bg_src + (y0 * bg_src_w + x0) * 4;
    const unsigned char *p10 = bg_src + (y0 * bg_src_w + x1) * 4;
    const unsigned char *p01 = bg_src + (y1 * bg_src_w + x0) * 4;
    const unsigned char *p11 = bg_src + (y1 * bg_src_w + x1) * 4;
    unsigned out[4];
    for (int c = 0; c < 4; c++) {
        float val = p00[c] * (1 - fx) * (1 - fy) + p10[c] * fx * (1 - fy) +
                    p01[c] * (1 - fx) * fy + p11[c] * fx * fy;
        out[c] = (unsigned)(val + 0.5f);
        if (out[c] > 255) out[c] = 255;
    }
    return (out[3] << 24) | (out[0] << 16) | (out[1] << 8) | out[2];
}

static void build_background(int w, int h) {
    if (bg_buffer) { my_free(bg_buffer); bg_buffer = NULL; }

    // Without an image the background is a flat color. The renderer paints
    // that directly into the rows it draws, so the full-window composite is
    // never materialized — a whole framebuffer of RAM saved in the common
    // case (and no full-screen memcpy per frame either).
    if (!bg_src) {
        bg_bw = w;
        bg_bh = h;
        bg_dirty = 0;
        return;
    }

    bg_buffer = my_malloc(w * h * 4);
    if (!bg_buffer) { bg_bw = 0; bg_bh = 0; return; }
    bg_bw = w;
    bg_bh = h;
    bg_dirty = 0;

    uint32_t alpha = (uint32_t)(g_config.opacity * 255.0f);
    if (alpha > 255) alpha = 255;
    uint32_t base_r = (g_config.bg_color >> 16) & 0xFF;
    uint32_t base_g = (g_config.bg_color >> 8) & 0xFF;
    uint32_t base_b = g_config.bg_color & 0xFF;
    uint32_t plain = (alpha << 24) |
                     (div255(base_r * alpha) << 16) |
                     (div255(base_g * alpha) << 8) |
                     (div255(base_b * alpha));

    float t = g_config.background_image_opacity;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int mode = g_config.background_image_mode;
    int ox = (w - bg_src_w) / 2, oy = (h - bg_src_h) / 2; // center mode offset

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t px;
            if (mode == 2) {
                // tile
                px = *(const uint32_t *)(bg_src + ((y % bg_src_h) * bg_src_w + (x % bg_src_w)) * 4);
            } else if (mode == 1) {
                // center
                int ix = x - ox, iy = y - oy;
                if (ix >= 0 && ix < bg_src_w && iy >= 0 && iy < bg_src_h)
                    px = *(const uint32_t *)(bg_src + (iy * bg_src_w + ix) * 4);
                else {
                    bg_buffer[y * w + x] = plain;
                    continue;
                }
            } else {
                // stretch
                px = sample_bg_image((x + 0.5f) / w, (y + 0.5f) / h);
            }
            float mix = t * (((px >> 24) & 0xFF) / 255.0f);
            uint32_t cr = (uint32_t)((1.0f - mix) * base_r + mix * ((px >> 16) & 0xFF));
            uint32_t cg = (uint32_t)((1.0f - mix) * base_g + mix * ((px >> 8) & 0xFF));
            uint32_t cb = (uint32_t)((1.0f - mix) * base_b + mix * (px & 0xFF));
            bg_buffer[y * w + x] = (alpha << 24) |
                                   (div255(cr * alpha) << 16) |
                                   (div255(cg * alpha) << 8) |
                                   (div255(cb * alpha));
        }
    }
}

// Glyph type lives in render.h so the GPU renderer shares the same cache.

#define GLYPH_CACHE_SIZE 65536
static Glyph g_glyph_cache[GLYPH_CACHE_SIZE];
// Compact "this slot is populated" bitmaps. Iterating these (a few KB) keeps
// cache clearing from faulting in the whole 3 MB entry array on zoom.
static uint64_t g_glyph_used[GLYPH_CACHE_SIZE / 64];

// Wide (CJK/emoji/astral) glyph cache — open-addressing hash, allocated
// lazily so we don't pay for a second full-size bitmap cache like the narrow
// one.
typedef struct {
    uint32_t cp;
    int used;
    Glyph g;
} WideSlot;

#define WIDE_CACHE_SIZE 8192 // power of two
static WideSlot *g_wide_cache = NULL;
static uint64_t g_wide_used[WIDE_CACHE_SIZE / 64];

// Candidate faces to try. Coverage of the common scripts, CJK, icons and
// symbols fits well within this; a higher cap only makes a search for an
// unrenderable glyph (color emoji) map more fonts for nothing.
#define MAX_FONTS 40
static stbtt_fontinfo g_fonts[MAX_FONTS];
static float g_font_scales[MAX_FONTS];
static int g_num_fonts = 0;

// Candidate faces from the font finder. Page-cache readahead makes every
// mapping fault in ~64 KB even for fonts that are never used, so fallback
// faces are mapped lazily the first time a glyph lookup needs one instead of
// all at startup. An ASCII-only session then keeps exactly one font mapped.
static FontMatch g_font_matches[MAX_FONTS];
static int g_num_matches = 0;
static int g_next_match = 0;
static const char *g_mapped_path = NULL;
static unsigned char *g_mapped_buf = NULL;

static unsigned char gamma_table[256];


static void* map_font(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }
    void *ptr = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (ptr != MAP_FAILED) {
        // Fonts are read at scattered offsets (table directory, cmap, glyph
        // data). Without this the kernel's fault-around/readahead charges
        // ~64 KB of RSS to every mapped face — even ones a glyph lookup only
        // probes once — so ask for page-granular faults instead.
        madvise(ptr, st.st_size, MADV_RANDOM);
    }
    return (ptr == MAP_FAILED) ? NULL : ptr;
}

// Extra pixel-scale factor on top of the configured font size: Wayland
// output scale / Xft.dpi, applied so text stays physically the same size and
// crisp on scaled displays.
static float g_render_scale = 1.0f;

// Called after the glyph caches are dropped (zoom/scale change) so the GPU
// renderer can rebuild its atlas. Set by gpu_init; CPU-only builds never set it.
static void (*g_glyph_reset_cb)(void) = NULL;

void render_set_glyph_reset_callback(void (*cb)(void)) {
    g_glyph_reset_cb = cb;
}

static float font_scale_for(const stbtt_fontinfo *f) {
    return stbtt_ScaleForPixelHeight(f, g_config.font_size * 1.333333f * g_render_scale);
}

// Map and initialize the next candidate face. Returns 1 when a font was
// added, 0 when the candidates are exhausted (or this one was unusable).
static int load_next_font(void) {
    if (g_next_match >= g_num_matches || g_num_fonts >= MAX_FONTS) return 0;
    FontMatch *m = &g_font_matches[g_next_match++];
    // Faces of one collection share a mapping.
    if (!g_mapped_path || strcmp(g_mapped_path, m->path) != 0) {
        g_mapped_buf = map_font(m->path);
        g_mapped_path = g_mapped_buf ? m->path : NULL;
    }
    if (!g_mapped_buf) return 0;
    int offset = stbtt_GetFontOffsetForIndex(g_mapped_buf, m->face);
    if (offset < 0 || !stbtt_InitFont(&g_fonts[g_num_fonts], g_mapped_buf, offset)) return 0;
    g_font_scales[g_num_fonts] = font_scale_for(&g_fonts[g_num_fonts]);
    g_num_fonts++;
    return 1;
}

// Drop every rasterized glyph so they are re-rendered at the current metrics.
// Only the used-bitmaps and populated entries are touched, so untouched cache
// pages stay uncommitted.
static void clear_glyph_caches(void) {
    for (int w = 0; w < GLYPH_CACHE_SIZE / 64; w++) {
        uint64_t bits = g_glyph_used[w];
        while (bits) {
            int b = __builtin_ctzll(bits);
            bits &= bits - 1;
            int cp = w * 64 + b;
            if (g_glyph_cache[cp].bitmap) my_free(g_glyph_cache[cp].bitmap);
            g_glyph_cache[cp].bitmap = NULL;
            g_glyph_cache[cp].in_atlas = 0;
        }
        g_glyph_used[w] = 0;
    }
    if (g_wide_cache) {
        for (int w = 0; w < WIDE_CACHE_SIZE / 64; w++) {
            uint64_t bits = g_wide_used[w];
            while (bits) {
                int b = __builtin_ctzll(bits);
                bits &= bits - 1;
                int idx = w * 64 + b;
                if (g_wide_cache[idx].g.bitmap) my_free(g_wide_cache[idx].g.bitmap);
                g_wide_cache[idx].used = 0;
                g_wide_cache[idx].g.bitmap = NULL;
                g_wide_cache[idx].g.in_atlas = 0;
            }
            g_wide_used[w] = 0;
        }
    }
    if (g_glyph_reset_cb) g_glyph_reset_cb();
}

// (Re)compute the cell metrics from the current font size and scale.
static void recompute_cell_metrics(void) {
    if (g_num_fonts <= 0) return;
    for (int i = 0; i < g_num_fonts; i++) {
        g_font_scales[i] = font_scale_for(&g_fonts[i]);
    }
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&g_fonts[0], &ascent, &descent, &lineGap);
    g_cell_height = (int)ceilf((ascent - descent + lineGap) * g_font_scales[0]);
    if (g_cell_height < 1) g_cell_height = 18;
    g_baseline = (int)roundf(ascent * g_font_scales[0]);

    int advance, lsb;
    stbtt_GetCodepointHMetrics(&g_fonts[0], 'M', &advance, &lsb);
    g_cell_width = (int)roundf(advance * g_font_scales[0]);
    if (g_cell_width < 1) g_cell_width = 9;

    // Kitty-style cell metric tuning
    g_cell_width += g_config.adjust_column_width;
    g_cell_height += g_config.adjust_line_height;
    g_baseline += g_config.adjust_baseline;
    if (g_cell_width < 1) g_cell_width = 1;
    if (g_cell_height < 1) g_cell_height = 1;
    if (g_baseline < 1) g_baseline = 1;
}

// Re-render every glyph at the current font size (runtime zoom).
void render_update_font(void) {
    if (g_num_fonts <= 0) return;
    recompute_cell_metrics();
    clear_glyph_caches();
}

// Apply a display scale factor (1.0 = unscaled) and re-metric everything.
void render_set_scale(float scale) {
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 4.0f) scale = 4.0f;
    if (scale == g_render_scale) return;
    g_render_scale = scale;
    render_update_font();
}

// Rasterize `cp` from the loaded fonts, mapping further fallback faces as
// needed. Returns NULL once every candidate has been tried.
static unsigned char *rasterize_from_fonts(uint32_t cp, int *w, int *h, int *xoff, int *yoff) {
    for (;;) {
        for (int i = 0; i < g_num_fonts; i++) {
            if (stbtt_FindGlyphIndex(&g_fonts[i], cp) != 0) {
                unsigned char *bitmap = stbtt_GetCodepointBitmap(&g_fonts[i], 0, g_font_scales[i],
                                                                 cp, w, h, xoff, yoff);
                if (bitmap) return bitmap;
            }
        }
        if (g_num_fonts >= MAX_FONTS) return NULL;
        if (!load_next_font() && g_next_match >= g_num_matches) return NULL;
    }
}

int render_init(const char *font_pattern) {
    for (int i = 0; i < 256; i++) {
        // Gamma correction to thicken text (makes fonts look "fatter" like Kitty)
        float v = i / 255.0f;
        gamma_table[i] = (unsigned char)(powf(v, 1.0f / 1.5f) * 255.0f);
    }

    // Family matching is done by the built-in finder (no fontconfig): the
    // requested family comes first, then fallback faces for missing glyphs.
    // Only the first usable face is mapped now (the cell metrics need it);
    // the fallbacks load on demand, when a glyph is actually missing.
    g_num_matches = font_list_match(font_pattern, g_font_matches, MAX_FONTS);
    g_next_match = 0;
    g_num_fonts = 0;
    g_mapped_path = NULL;
    g_mapped_buf = NULL;
    while (g_num_fonts == 0 && load_next_font()) {
        // keep trying until one face loads
    }
    // The caches live in BSS (already zero) and untouched pages are never
    // faulted in; the metrics are computed once here.
    recompute_cell_metrics();

    // Load the configured background image (kept for the process lifetime)
    if (g_config.background_image[0]) {
        int w, h, n;
        unsigned char *data = stbi_load(g_config.background_image, &w, &h, &n, 4);
        if (data) {
            bg_src = data;
            bg_src_w = w;
            bg_src_h = h;
        }
    }

    return 0;
}

extern int g_select_active;
extern int g_select_start_row;
extern int g_select_start_col;
extern int g_select_end_row;
extern int g_select_end_col;
extern int g_cursor_blink_on; // cursor blink phase (main.c)

// ---------------------------------------------------------------------------
// Wide (CJK/emoji/astral) glyph cache — declarations live at the top so the
// zoom cache-clearing code can reach them.
// ---------------------------------------------------------------------------
static Glyph *wide_glyph_lookup(uint32_t cp) {
    if (!g_wide_cache) {
        g_wide_cache = calloc(WIDE_CACHE_SIZE, sizeof(WideSlot));
        if (!g_wide_cache) return NULL;
    }
    uint32_t h = (cp * 2654435761u) & (WIDE_CACHE_SIZE - 1);
    for (int probe = 0; probe < 64; probe++) {
        int idx = (int)((h + probe) & (WIDE_CACHE_SIZE - 1));
        WideSlot *s = &g_wide_cache[idx];
        if (!s->used) {
            int w = 0, h2 = 0, xoff = 0, yoff = 0;
            unsigned char *bitmap = rasterize_from_fonts(cp, &w, &h2, &xoff, &yoff);
            if (!bitmap) {
                bitmap = malloc(1);
                if (bitmap) bitmap[0] = 0;
                w = 1; h2 = 1; xoff = 0; yoff = 0;
                if (!bitmap) return NULL;
            }
            s->cp = cp;
            s->used = 1;
            g_wide_used[idx >> 6] |= 1ull << (idx & 63);
            s->g.bitmap = bitmap;
            s->g.w = w; s->g.h = h2; s->g.xoff = xoff; s->g.yoff = yoff;
            return &s->g;
        }
        if (s->cp == cp) return &s->g;
    }
    return NULL; // cache pressure — skip rendering rather than evict
}

// ---------------------------------------------------------------------------
// Procedural glyphs for box drawing (U+2500-U+257F), block elements
// (U+2580-U+259F) and scan lines (U+23BA-U+23BD).
// They are synthesized to span the entire cell so adjacent characters
// connect seamlessly (no gaps at cell boundaries), like Kitty does.
// ---------------------------------------------------------------------------

static void fill_rect(unsigned char *bm, int w, int h, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (y1 >= h) y1 = h - 1;
    for (int y = y0; y <= y1; y++) {
        unsigned char *row = bm + y * w;
        for (int x = x0; x <= x1; x++) row[x] = 255;
    }
}

static int line_thickness(int h) {
    int t = (h + 8) / 12;
    return t < 1 ? 1 : t;
}

// Per-arm descriptor for U+2500-U+257F. Arms packed 2 bits each:
// up | right<<2 | down<<4 | left<<6, style: 0=none 1=light 2=double 3=heavy.
// Dashes (0x04-0x0B, 0x4C-0x4F), arcs (0x6D-0x70) and diagonals (0x71-0x73)
// are handled separately and marked 0.
static const uint8_t box_arms[0x80] = {
    0x44, 0xCC, 0x11, 0x33, 0, 0, 0, 0, 0, 0, 0, 0,             // ─━│┃┄┅┆┇┈┉┊┋
    0x14, 0x1C, 0x34, 0x3C, 0x50, 0xD0, 0x70, 0xF0,             // ┌┍┎┏┐┑┒┓
    0x05, 0x0D, 0x07, 0x0F, 0x41, 0xC1, 0x43, 0xC3,             // └┕┖┗┘┙┚┛
    0x15, 0x1D, 0x17, 0x35, 0x37, 0x17, 0x35, 0x37,             // ├┝┞┟┠┡┢┣
    0x51, 0xD1, 0x53, 0x71, 0x73, 0x53, 0x71, 0xF3,             // ┤┥┦┧┨┩┪┫
    0x54, 0xD4, 0x5C, 0xDC, 0x74, 0xF4, 0x7C, 0xFC,             // ┬┭┮┯┰┱┲┳
    0x45, 0xC5, 0x4D, 0xCD, 0x47, 0xC7, 0x4F, 0xCF,             // ┴┵┶┷┸┹┺┻
    0x55, 0xD5, 0x5D, 0xDD, 0x57, 0x75, 0x77, 0xD7,             // ┼┽┾┿╀╁╂╃
    0x5F, 0xF5, 0x7D, 0xDF, 0xFD, 0xF7, 0x7F, 0xFF,             // ╄╅╆╇╈╉╊╋
    0, 0, 0, 0,                                                 // ╌╍╎╏ (dashes)
    0x88, 0x22, 0x18, 0x24, 0x28, 0x90, 0x60, 0xA0,             // ═║╒╓╔╕╖╗
    0x09, 0x06, 0x0A, 0x81, 0x42, 0x82, 0x19, 0x26,             // ╘╙╚╛╜╝╞╟
    0x2A, 0x91, 0x62, 0xA2, 0x98, 0x64, 0xA8, 0x89,             // ╠╡╢╣╤╥╦╧
    0x46, 0x8A, 0x99, 0x66, 0xAA, 0, 0, 0,                     // ╨╩╪╫╬╭╮╯
    0, 0, 0, 0, 0x40, 0x01, 0x04, 0x10,                        // ╰╱╲╳╴╵╶╷
    0xC0, 0x03, 0x0C, 0x30, 0x4C, 0x31, 0xC4, 0x13             // ╸╹╺╻╼╽╾╿
};

// Draw one arm (0=up 1=right 2=down 3=left) with the given style.
// cx/cy = light stroke center column/row, hx/hy = heavy stroke column/row.
static void draw_arm(unsigned char *bm, int w, int h, int dir, int style,
                     int cx, int cy, int hx, int hy, int off, int t,
                     int from, int to) {
    if (style == 0) return;
    if (dir == 0 || dir == 2) { // vertical arm, spans rows [from, to]
        if (style == 1) {
            fill_rect(bm, w, h, cx, from, cx + t - 1, to);
        } else if (style == 3) {
            fill_rect(bm, w, h, hx, from, hx + 2 * t - 1, to);
        } else {
            fill_rect(bm, w, h, cx - off, from, cx - 1, to);
            fill_rect(bm, w, h, cx + off, from, cx + off + t - 1, to);
        }
    } else { // horizontal arm, spans cols [from, to]
        if (style == 1) {
            fill_rect(bm, w, h, from, cy, to, cy + t - 1);
        } else if (style == 3) {
            fill_rect(bm, w, h, from, hy, to, hy + 2 * t - 1);
        } else {
            fill_rect(bm, w, h, from, cy - off, to, cy - 1);
            fill_rect(bm, w, h, from, cy + off, to, cy + off + t - 1);
        }
    }
}

static void synth_box_glyph(unsigned char *bm, int w, int h, int cp) {
    int t = line_thickness(h);
    uint8_t desc = box_arms[cp - 0x2500];
    int U = desc & 3, R = (desc >> 2) & 3, D = (desc >> 4) & 3, L = (desc >> 6) & 3;

    int cx = (w - t) / 2;
    int cy = (h - t) / 2;
    int hx = (w - 2 * t) / 2;
    int hy = (h - 2 * t) / 2;
    int off = t;

    // Outer extents of strokes crossing the center, so arms connect.
    int v_lo = cx, v_hi = cx + t - 1;     // vertical stroke cols (light default)
    int h_lo = cy, h_hi = cy + t - 1;     // horizontal stroke rows
    if (U == 2 || D == 2) { v_lo = cx - off; v_hi = cx + off + t - 1; }
    else if (U == 3 || D == 3) { v_lo = hx; v_hi = hx + 2 * t - 1; }
    if (L == 2 || R == 2) { h_lo = cy - off; h_hi = cy + off + t - 1; }
    else if (L == 3 || R == 3) { h_lo = hy; h_hi = hy + 2 * t - 1; }

    if (U) draw_arm(bm, w, h, 0, U, cx, cy, hx, hy, off, t, 0, (L || R) ? h_hi : h / 2);
    if (D) draw_arm(bm, w, h, 2, D, cx, cy, hx, hy, off, t, (L || R) ? h_lo : h / 2, h - 1);
    if (L) draw_arm(bm, w, h, 3, L, cx, cy, hx, hy, off, t, 0, (U || D) ? v_hi : w / 2);
    if (R) draw_arm(bm, w, h, 1, R, cx, cy, hx, hy, off, t, (U || D) ? v_lo : w / 2, w - 1);
}

// Rounded corners ╭╮╰╯ — quarter-ellipse arc centered on the OUTER corner of
// the cell, tangent to the vertical stroke at the top/bottom edge and to the
// horizontal stroke at the side edge, so it joins adjacent lines seamlessly.
static void synth_arc_glyph(unsigned char *bm, int w, int h, int cp) {
    int t = line_thickness(h);
    double ccx = (w - 1) / 2.0;
    double ccy = (h - 1) / 2.0;
    // Ellipse center sits on the outer corner; semi-axes reach the edge midpoints.
    double Cx = (cp == 0x256D || cp == 0x2570) ? w - 1.0 : 0.0;   // outer corner x
    double Cy = (cp == 0x256D || cp == 0x256E) ? 0.0 : h - 1.0;   // outer corner y
    double rx = (cp == 0x256D || cp == 0x2570) ? (w - 1) - ccx : ccx;
    double ry = (cp == 0x256D || cp == 0x256E) ? ccy : (h - 1) - ccy;
    if (rx < 0.5) rx = 0.5;
    if (ry < 0.5) ry = 0.5;
    double rmin = (rx < ry) ? rx : ry;
    double band = t * 0.6 + 0.5;

    int x0 = (cp == 0x256D || cp == 0x2570) ? (int)ccx : 0;
    int x1 = (cp == 0x256D || cp == 0x2570) ? w - 1 : (int)ccx;
    int y0 = (cp == 0x256D || cp == 0x256E) ? 0 : (int)ccy;
    int y1 = (cp == 0x256D || cp == 0x256E) ? (int)ccy : h - 1;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            double u = (x - Cx) / rx;
            double v = (y - Cy) / ry;
            double d = sqrt(u * u + v * v);
            if (fabs(d - 1.0) * rmin <= band) bm[y * w + x] = 255;
        }
    }
}

// Diagonals ╱ ╲ ╳ — distance to the corner-to-corner line.
static void synth_diag_glyph(unsigned char *bm, int w, int h, int cp) {
    int t = line_thickness(h);
    double band = t * 0.6 + 0.5;
    double len = sqrt((double)(w - 1) * (w - 1) + (double)(h - 1) * (h - 1));
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (cp == 0x2571 || cp == 0x2573) {
                // line through (0,h-1) and (w-1,0)
                double dist = fabs((h - 1.0) * x + (w - 1.0) * y - (w - 1.0) * (h - 1.0)) / len;
                if (dist <= band) { bm[y * w + x] = 255; continue; }
            }
            if (cp == 0x2572 || cp == 0x2573) {
                // line through (0,0) and (w-1,h-1)
                double dist = fabs((h - 1.0) * x - (w - 1.0) * y) / len;
                if (dist <= band) bm[y * w + x] = 255;
            }
        }
    }
}

// Dashed lines ┄┅┆┇┈┉┊┋ ╌╍╎╏
static void synth_dash_glyph(unsigned char *bm, int w, int h, int cp) {
    int t = line_thickness(h);
    int heavy = (cp == 0x2505 || cp == 0x2507 || cp == 0x2509 || cp == 0x250B ||
                 cp == 0x254D || cp == 0x254F);
    int vert  = (cp == 0x2506 || cp == 0x2507 || cp == 0x250A || cp == 0x250B ||
                 cp == 0x254E || cp == 0x254F);
    int nseg = (cp <= 0x2507) ? 3 : (cp <= 0x250B) ? 4 : 2;
    int th = heavy ? 2 * t : t;
    int period, dash, gap;
    if (vert) {
        period = h / nseg; if (period < 2) period = 2;
        dash = period / 2; if (dash < 1) dash = 1;
        gap = (period - dash) / 2;
        int x = heavy ? (w - th) / 2 : (w - t) / 2;
        for (int i = 0; i < nseg; i++) {
            int y = i * period + gap;
            fill_rect(bm, w, h, x, y, x + th - 1, y + dash - 1);
        }
    } else {
        period = w / nseg; if (period < 2) period = 2;
        dash = period / 2; if (dash < 1) dash = 1;
        gap = (period - dash) / 2;
        int y = heavy ? (h - th) / 2 : (h - t) / 2;
        for (int i = 0; i < nseg; i++) {
            int x = i * period + gap;
            fill_rect(bm, w, h, x, y, x + dash - 1, y + th - 1);
        }
    }
}

// Block elements ▀-▟ and shades ░▒▓
static void synth_block_glyph(unsigned char *bm, int w, int h, int cp) {
    int hw = w / 2, hh = h / 2;
    switch (cp) {
        case 0x2580: fill_rect(bm, w, h, 0, 0, w - 1, hh - 1); return;          // ▀
        case 0x2584: fill_rect(bm, w, h, 0, hh, w - 1, h - 1); return;          // ▄
        case 0x2588: fill_rect(bm, w, h, 0, 0, w - 1, h - 1); return;           // █
        case 0x258C: fill_rect(bm, w, h, 0, 0, hw - 1, h - 1); return;          // ▌
        case 0x2590: fill_rect(bm, w, h, hw, 0, w - 1, h - 1); return;          // ▐
        case 0x2594: fill_rect(bm, w, h, 0, 0, w - 1, (h + 7) / 8 - 1); return; // ▔
        case 0x2595: fill_rect(bm, w, h, w - (w + 7) / 8, 0, w - 1, h - 1); return; // ▕
        case 0x2591: case 0x2592: case 0x2593: {                                // ░▒▓
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    int on;
                    if (cp == 0x2592) on = ((x + y) & 1) == 0;
                    else if (cp == 0x2591) on = ((x & 1) | (y & 1)) == 0;
                    else on = !(((x & 1) | (y & 1)) == 0);
                    if (on) bm[y * w + x] = 255;
                }
            }
            return;
        }
    }
    if (cp >= 0x2581 && cp <= 0x2587) { // ▁▂▃▄▅▆▇ lower n/8
        int n = cp - 0x2580;
        int rows = (h * n + 4) / 8;
        fill_rect(bm, w, h, 0, h - rows, w - 1, h - 1);
    } else if (cp >= 0x2589 && cp <= 0x258F) { // ▉▊▋▌▍▎▏ left n/8 (7..1)
        int n = 0x2590 - cp;
        int cols = (w * n + 4) / 8;
        fill_rect(bm, w, h, 0, 0, cols - 1, h - 1);
    } else if (cp >= 0x2596 && cp <= 0x259F) { // quadrant blocks
        // 2596▖=LL 2597▗=LR 2598▘=UL 2599▙=UL+LL+LR 259A▚=UL+LR 259B▛=UL+UR+LL
        // 259C▜=UL+UR+LR 259D▝=UR 259E▞=UR+LL 259F▟=UR+LL+LR
        int ul = (cp >= 0x2598 && cp <= 0x259C);
        int ur = (cp >= 0x259B && cp <= 0x259F);
        int ll = (cp == 0x2596 || cp == 0x2599 || cp == 0x259B || cp == 0x259E || cp == 0x259F);
        int lr = (cp == 0x2597 || cp == 0x2599 || cp == 0x259A || cp == 0x259C || cp == 0x259F);
        if (ul) fill_rect(bm, w, h, 0, 0, hw - 1, hh - 1);
        if (ur) fill_rect(bm, w, h, hw, 0, w - 1, hh - 1);
        if (ll) fill_rect(bm, w, h, 0, hh, hw - 1, h - 1);
        if (lr) fill_rect(bm, w, h, hw, hh, w - 1, h - 1);
    }
}

// Horizontal scan lines ⎺⎻⎼⎽ (U+23BA-U+23BD, used by DEC special graphics)
static void synth_scanline_glyph(unsigned char *bm, int w, int h, int cp) {
    int t = line_thickness(h) < 2 ? 1 : 2;
    int y;
    switch (cp) {
        case 0x23BA: y = (h - t) / 4; break;
        case 0x23BB: y = (h - t) / 2; break;
        case 0x23BC: y = (3 * (h - t)) / 4; break;
        default:     y = h - t; break;
    }
    fill_rect(bm, w, h, 0, y, w - 1, y + t - 1);
}

static int is_procedural_glyph(uint32_t cp) {
    return (cp >= 0x23BA && cp <= 0x23BD) || (cp >= 0x2500 && cp <= 0x259F);
}

static void synth_glyph(uint32_t cp, unsigned char *bm, int w, int h) {
    if (cp >= 0x23BA && cp <= 0x23BD) synth_scanline_glyph(bm, w, h, cp);
    else if (cp >= 0x2580 && cp <= 0x259F) synth_block_glyph(bm, w, h, cp);
    else if (cp >= 0x256D && cp <= 0x2570) synth_arc_glyph(bm, w, h, cp);
    else if (cp >= 0x2571 && cp <= 0x2573) synth_diag_glyph(bm, w, h, cp);
    else if ((cp >= 0x2504 && cp <= 0x250B) || (cp >= 0x254C && cp <= 0x254F)) synth_dash_glyph(bm, w, h, cp);
    else synth_box_glyph(bm, w, h, cp);
}

// Shared glyph lookup for the CPU and GPU renderers: returns the cached glyph
// for a codepoint, rasterizing it (font or procedural) on first use.
Glyph *render_lookup_glyph(uint32_t cp, int wide) {
    if (g_num_matches <= 0) return NULL;
    if (wide) {
        if (cp >= 32 && cp != ' ') return wide_glyph_lookup(cp);
        return NULL;
    }
    if (cp < 32 || cp == ' ') return NULL;
    if (cp < GLYPH_CACHE_SIZE) {
        if (!g_glyph_cache[cp].bitmap) {
            int w = 0, h = 0, xoff = 0, yoff = 0;
            unsigned char *bitmap = NULL;
            if (is_procedural_glyph(cp)) {
                w = g_cell_width; h = g_cell_height;
                bitmap = calloc(1, w * h);
                if (bitmap) {
                    synth_glyph(cp, bitmap, w, h);
                    xoff = 0;
                    yoff = -g_baseline;
                } else {
                    w = 1; h = 1;
                }
            }
            if (!bitmap) {
                bitmap = rasterize_from_fonts(cp, &w, &h, &xoff, &yoff);
            }
            if (!bitmap) {
                bitmap = malloc(1); bitmap[0] = 0; w = 1; h = 1; xoff = 0; yoff = 0;
            }
            g_glyph_cache[cp].bitmap = bitmap;
            g_glyph_cache[cp].w = w; g_glyph_cache[cp].h = h;
            g_glyph_cache[cp].xoff = xoff; g_glyph_cache[cp].yoff = yoff;
            g_glyph_used[cp >> 6] |= 1ull << (cp & 63);
        }
        return &g_glyph_cache[cp];
    }
    if (cp >= 32) return wide_glyph_lookup(cp);
    return NULL;
}

unsigned char render_gamma(unsigned char v) {
    return gamma_table[v];
}

const unsigned char *render_bg_image(int *w, int *h) {
    if (w) *w = bg_src_w;
    if (h) *h = bg_src_h;
    return bg_src;
}

static void draw_kitty_images(VTState *state, uint32_t *fb, int z_limit, int dir) {
    if (!state->kitty_placements) return;
    
    int count = 0;
    for (KittyPlacement *p = state->kitty_placements; p; p = p->next) count++;
    if (count == 0) return;
    
    KittyPlacement **arr = my_malloc(count * sizeof(KittyPlacement*));
    if (!arr) return;
    int i = 0;
    for (KittyPlacement *p = state->kitty_placements; p; p = p->next) arr[i++] = p;
    
    for (int k = count - 1; k >= 0; k--) {
        KittyPlacement *p = arr[k];
        if (dir < 0 && p->z_index >= z_limit) continue;
        if (dir >= 0 && p->z_index < z_limit) continue;
        
        KittyImage *img = NULL;
        for (KittyImage *im = state->kitty_images; im; im = im->next) {
            if (im->id == p->image_id) { img = im; break; }
        }
        if (!img || !img->pixels) continue;
        
        // Compute screen pixel coords — scroll_offset shifts the view
        int start_x = g_config.padding_left + p->cell_x * g_cell_width;
        int start_y = g_config.padding_top + (p->cell_y + state->scroll_offset) * g_cell_height;
        int target_w = p->cols * g_cell_width;
        int target_h = p->rows * g_cell_height;
        
        // Skip if completely off-screen
        if (start_y + target_h <= 0 || start_y >= g_height) continue;
        if (start_x + target_w <= 0 || start_x >= g_width) continue;
        
        for (int dy = 0; dy < target_h; dy++) {
            int screen_y = start_y + dy;
            if (screen_y < 0 || screen_y >= g_height) continue;
            
            int src_y = (p->src_h > 0) ? (dy * p->src_h) / target_h + p->src_y : 0;
            if (src_y < 0) src_y = 0;
            if (src_y >= img->h) src_y = img->h - 1;
            
            for (int dx = 0; dx < target_w; dx++) {
                int screen_x = start_x + dx;
                if (screen_x < 0 || screen_x >= g_width) continue;
                
                int src_x = (p->src_w > 0) ? (dx * p->src_w) / target_w + p->src_x : 0;
                if (src_x < 0) src_x = 0;
                if (src_x >= img->w) src_x = img->w - 1;
                
                uint32_t src_pixel = img->pixels[src_y * img->w + src_x];
                uint32_t a = (src_pixel >> 24) & 0xFF;
                if (a == 255) {
                    fb[screen_y * g_width + screen_x] = src_pixel;
                } else if (a > 0) {
                    uint32_t dst = fb[screen_y * g_width + screen_x];
                    uint32_t dst_r = (dst >> 16) & 0xFF;
                    uint32_t dst_g = (dst >> 8) & 0xFF;
                    uint32_t dst_b = dst & 0xFF;
                    uint32_t src_r = (src_pixel >> 16) & 0xFF;
                    uint32_t src_g = (src_pixel >> 8) & 0xFF;
                    uint32_t src_b = src_pixel & 0xFF;
                    uint32_t out_r = div255(src_r * a + dst_r * (255 - a));
                    uint32_t out_g = div255(src_g * a + dst_g * (255 - a));
                    uint32_t out_b = div255(src_b * a + dst_b * (255 - a));
                    fb[screen_y * g_width + screen_x] = (dst & 0xFF000000) | (out_r << 16) | (out_g << 8) | out_b;
                }
            }
        }
    }
    my_free(arr);
}

// ---------------------------------------------------------------------------
// Frame damage tracking
//
// render_draw() writes into a presentation buffer (Wayland alternates two SHM
// buffers, X11 uses one). Repainting the full window on every frame is by far
// the largest per-frame cost — a full-screen background copy plus every cell —
// and it is also what opens the tearing/black-flash window on screen. Instead,
// each presentation buffer keeps a shadow copy of the cell grid it last
// showed, and a row is repainted only when it actually differs from that
// shadow. Cursor motion, selection, scroll, the visual bell and kitty images
// force the rows (or the whole frame) they affect.
// ---------------------------------------------------------------------------

int g_damage_y0 = 0, g_damage_y1 = 0;

typedef struct {
    uint32_t *fb;                 // presentation buffer identity
    int w, h;                     // buffer size the shadow was taken at
    int cols, rows;               // grid size the shadow was taken with
    Cell *cells;                  // last drawn visible grid, rows*cols cells
    int cached;                   // shadow matches what this buffer shows
    int cursor_x, cursor_y, cursor_on, cursor_shape, scroll_offset;
    int sel_on, sel_r1, sel_c1, sel_r2, sel_c2;
    int bell_on, had_images;
} FrameShadow;

#define MAX_FRAME_SHADOWS 4
static FrameShadow g_shadows[MAX_FRAME_SHADOWS];

// A backend destroyed its presentation buffers: cached frame state is
// meaningless for whatever buffer shows up next.
void render_forget_all_buffers(void) {
    for (int i = 0; i < MAX_FRAME_SHADOWS; i++) {
        if (g_shadows[i].cells) my_free(g_shadows[i].cells);
        memset(&g_shadows[i], 0, sizeof(FrameShadow));
    }
}

// Shadow slot for a presentation buffer, sized for the current grid.
// Returns NULL when the shadow cannot be allocated — the frame is then drawn
// fully every time, which is always correct, just slower.
static FrameShadow *shadow_for(uint32_t *fb, int cols, int rows) {
    FrameShadow *slot = NULL;
    for (int i = 0; i < MAX_FRAME_SHADOWS; i++) {
        FrameShadow *sh = &g_shadows[i];
        if (sh->fb == fb && sh->cells && sh->w == g_width && sh->h == g_height)
            return sh;
        if (!sh->fb && !slot) slot = sh;
    }
    if (!slot) { // every slot is taken — recycle the first one
        slot = &g_shadows[0];
        my_free(slot->cells);
        memset(slot, 0, sizeof(*slot));
    }
    if (!slot->cells || slot->cols != cols || slot->rows != rows) {
        my_free(slot->cells);
        slot->cells = my_malloc((size_t)cols * (size_t)rows * sizeof(Cell));
        slot->cols = cols;
        slot->rows = rows;
    }
    if (!slot->cells) return NULL;
    slot->fb = fb;
    slot->w = g_width;
    slot->h = g_height;
    slot->cached = 0;
    return slot;
}

// The visible row `y` of `state` (scrollback included), or NULL for a row
// with no backing cells (scrolled past the oldest history line).
static const Cell *visible_row(VTState *state, int y) {
    int logical_y = y - state->scroll_offset;
    if (logical_y < 0) {
        int max_sb = g_config.scrollback_lines;
        if (max_sb > MAX_SCROLLBACK) max_sb = MAX_SCROLLBACK;
        if (max_sb <= 0) max_sb = 1;
        int back = -logical_y;
        if (back <= state->scrollback_count) {
            int real_idx = (state->scrollback_head - back + max_sb) % max_sb;
            return state->scrollback[real_idx].cells;
        }
        return NULL;
    }
    if (logical_y < state->rows) {
        return &state->cells[logical_y * state->cols];
    }
    return NULL;
}

static Cell blank_row_cell(void) {
    Cell blank;
    blank.char_code = ' ';
    blank.fg_color = g_config.fg_color;
    blank.bg_color = g_config.bg_color;
    blank.wrapped = 0;
    blank.attrs = 0;
    blank.link = 0;
    return blank;
}

static int row_is_blank(const Cell *row, int n, Cell blank) {
    for (int x = 0; x < n; x++) {
        if (row[x].char_code != ' ' || row[x].fg_color != blank.fg_color ||
            row[x].bg_color != blank.bg_color || row[x].wrapped ||
            row[x].attrs) return 0;
    }
    return 1;
}

static void row_fill_blank(Cell *row, int n, Cell blank) {
    for (int x = 0; x < n; x++) row[x] = blank;
}

// Paint cells [x0, x1) of one terminal row into the framebuffer. `row` is the
// backing cell row (NULL paints blanks). Selection bounds are pre-normalized
// by render_draw.
static void draw_row_span(VTState *state, int y, int x0, int x1, const Cell *row,
                          uint32_t clear_bg, int cursor_shown_now,
                          int sel_on, int r1, int c1, int r2, int c2) {
    int logical_y = y - state->scroll_offset;
    int start_y = g_config.padding_top + y * g_cell_height;
    if (start_y >= g_height) return;
    Cell blank = blank_row_cell();

    for (int x = x0; x < x1; x++) {
        Cell c = row ? row[x] : blank;

        uint32_t bg = c.bg_color;
        uint32_t fg = c.fg_color;
        uint8_t cattrs = c.attrs;

        // Double-width characters span two cells
        int cell_w = (cattrs & CELL_WIDE) ? 2 * g_cell_width : g_cell_width;

        int is_selected = 0;
        if (sel_on) {
            if (logical_y > r1 && logical_y < r2) is_selected = 1;
            else if (logical_y == r1 && logical_y == r2) {
                if (x >= c1 && x <= c2) is_selected = 1;
            } else if (logical_y == r1 && x >= c1) is_selected = 1;
            else if (logical_y == r2 && x <= c2) is_selected = 1;
        }

        if (is_selected) {
            // Explicit selection colors from the config; fall back to the
            // classic fg/bg swap for unconfigured channels.
            bg = g_config.selection_bg_set ? g_config.selection_background : c.fg_color;
            fg = g_config.selection_fg_set ? g_config.selection_foreground : c.bg_color;
        }

        int is_cursor = (cursor_shown_now && logical_y == state->cursor_y && x == state->cursor_x);
        if (is_cursor && g_config.cursor_shape == 0) {
            bg = g_config.cursor_color;
            fg = g_config.cursor_text_color_set ? g_config.cursor_text_color : c.bg_color;
        }


        uint32_t bg_pixel = clear_bg;
        int start_x = g_config.padding_left + x * g_cell_width;

        if (bg != g_config.bg_color) {
            bg_pixel = bg | 0xFF000000;
            for (int cy = 0; cy < g_cell_height; cy++) {
                int screen_y = start_y + cy;
                if (screen_y >= g_height) break;
                for (int cx = 0; cx < cell_w; cx++) {
                    int screen_x = start_x + cx;
                    if (screen_x >= g_width) break;
                    g_framebuffer[screen_y * g_width + screen_x] = bg_pixel;
                }
            }
        }

        if (!(cattrs & CELL_TRAIL)) {
            // Shared with the GPU renderer; handles procedural glyphs, the
            // direct narrow cache, the wide/astral hash cache and lazy
            // fallback-font loading in one place.
            Glyph *b = render_lookup_glyph(c.char_code, (cattrs & CELL_WIDE) ? 1 : 0);

            if (b && b->bitmap && c.char_code != ' ') {
                int passes = (cattrs & CELL_BOLD) ? 2 : 1;
                uint32_t fg_r = (fg >> 16) & 0xFF, fg_g = (fg >> 8) & 0xFF, fg_b = fg & 0xFF;
                // Glyphs are clipped to their cell box so a repaint of one
                // cell can never leave ink in a neighbouring cell's pixels.
                // Italic is the exception: its shear leans past the cell edge,
                // so rows containing italic cells are repainted in full.
                int clip_h = !(cattrs & CELL_ITALIC);
                int clip_x0 = start_x, clip_x1 = start_x + cell_w;
                int clip_y1 = start_y + g_cell_height;
                if (clip_y1 > g_height) clip_y1 = g_height;
                for (int pass = 0; pass < passes; pass++) {
                    int w = b->w; int h = b->h;
                    int xoff = b->xoff; int yoff = b->yoff + g_baseline;
                    // Keep double-width bitmaps inside their 2-cell span
                    int max_cx = w;
                    if ((cattrs & CELL_WIDE) && xoff + w > cell_w) max_cx = cell_w - xoff;
                    if (max_cx < 0) max_cx = 0;
                    for (int cy = 0; cy < h; cy++) {
                        // Italic: shear proportional to height above the baseline
                        int shear = 0;
                        if (cattrs & CELL_ITALIC) {
                            shear = (g_baseline - (b->yoff + cy)) / 3;
                        }
                        int pY = start_y + yoff + cy;
                        if (pY < start_y || pY >= clip_y1) continue;
                        const unsigned char *src_row = b->bitmap + cy * w;
                        uint32_t *dst_row = g_framebuffer + pY * g_width;
                        for (int cx = 0; cx < max_cx; cx++) {
                            int pX = start_x + xoff + cx + shear + pass;
                            if (pX >= 0 && pX < g_width &&
                                (!clip_h || (pX >= clip_x0 && pX < clip_x1))) {
                                unsigned char alpha = gamma_table[src_row[cx]];
                                if (cattrs & CELL_DIM) alpha = (alpha * 2) / 3;
                                if (alpha > 0) {
                                    uint32_t dst = dst_row[pX];
                                    uint32_t bg_r = (dst >> 16) & 0xFF, bg_g = (dst >> 8) & 0xFF, bg_b = dst & 0xFF;
                                    uint32_t dst_a = (dst >> 24) & 0xFF;
                                    uint32_t r = div255(fg_r * alpha + bg_r * (255 - alpha));
                                    uint32_t g2 = div255(fg_g * alpha + bg_g * (255 - alpha));
                                    uint32_t b2 = div255(fg_b * alpha + bg_b * (255 - alpha));
                                    uint32_t new_a = alpha + div255(dst_a * (255 - alpha));
                                    dst_row[pX] = (new_a << 24) | (r << 16) | (g2 << 8) | b2;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Underline / strikethrough decorations (wide chars draw them
        // across the full two-cell span; the trailing half is skipped).
        // OSC 8 hyperlink cells are underlined like links in other terminals.
        int link_underline = (c.link != 0 && c.char_code != ' ');
        if (!(cattrs & CELL_TRAIL) && fg != bg &&
            ((cattrs & (CELL_UNDERLINE | CELL_STRIKE)) || link_underline)) {
            int t = line_thickness(g_cell_height);
            if ((cattrs & CELL_UNDERLINE) || link_underline) {
                int uy = start_y + g_baseline + 1;
                if (uy + t > start_y + g_cell_height) uy = start_y + g_cell_height - t;
                for (int cy = 0; cy < t; cy++) {
                    int sy = uy + cy;
                    if (sy < 0 || sy >= g_height) continue;
                    for (int cx = 0; cx < cell_w; cx++) {
                        int sx = start_x + cx;
                        if (sx < 0 || sx >= g_width) continue;
                        g_framebuffer[sy * g_width + sx] = fg | 0xFF000000;
                    }
                }
            }
            if (cattrs & CELL_STRIKE) {
                int sy = start_y + (g_baseline * 2) / 3;
                for (int cy = 0; cy < t; cy++) {
                    int sy2 = sy + cy;
                    if (sy2 < 0 || sy2 >= g_height) continue;
                    for (int cx = 0; cx < cell_w; cx++) {
                        int sx = start_x + cx;
                        if (sx < 0 || sx >= g_width) continue;
                        g_framebuffer[sy2 * g_width + sx] = fg | 0xFF000000;
                    }
                }
            }
        }

        if (is_cursor) {
            uint32_t c_color = g_config.cursor_color | 0xFF000000;
            if (g_config.cursor_shape == 1) {
                int cy = start_y + g_cell_height - 2;
                for (int cx = 0; cx < cell_w; cx++) {
                    int sx = start_x + cx;
                    if (sx < g_width && cy < g_height && cy >= 0) {
                        g_framebuffer[cy * g_width + sx] = c_color;
                        if (cy + 1 < g_height) g_framebuffer[(cy + 1) * g_width + sx] = c_color;
                    }
                }
            } else if (g_config.cursor_shape == 2) {
                for (int cy = 0; cy < g_cell_height; cy++) {
                    int sy = start_y + cy;
                    if (start_x < g_width && sy < g_height && sy >= 0) {
                        g_framebuffer[sy * g_width + start_x] = c_color;
                        if (start_x + 1 < g_width) g_framebuffer[sy * g_width + start_x + 1] = c_color;
                    }
                }
            }
        }
    }
}

extern int64_t g_bell_flash_until;
extern int64_t g_bell_flash_start;

int render_draw(VTState *state) {
    if (!g_framebuffer) return -1;
    if (g_width <= 0 || g_height <= 0 || state->cols <= 0 || state->rows <= 0) return 0;

    // Cursor visibility: cursor_blink is tri-state — 0 = no cursor at all,
    // 1 = blinking, 2 = steady. Both 1 and 2 respect the application's ?25
    // visibility: full-screen apps (cava, vim, ...) hide the cursor, and a
    // steady override would paint a block cursor on top of their output.
    int cursor_shown_now;
    if (g_config.cursor_blink == 0) cursor_shown_now = 0;
    else if (g_config.cursor_blink == 2) cursor_shown_now = state->cursor_visible;
    else cursor_shown_now = state->cursor_visible && g_cursor_blink_on;

    uint32_t alpha = (uint32_t)(g_config.opacity * 255.0f);
    if (alpha > 255) alpha = 255;
    uint32_t bg_r = (g_config.bg_color >> 16) & 0xFF;
    uint32_t bg_g = (g_config.bg_color >> 8) & 0xFF;
    uint32_t bg_b = g_config.bg_color & 0xFF;
    bg_r = div255(bg_r * alpha);
    bg_g = div255(bg_g * alpha);
    bg_b = div255(bg_b * alpha);
    uint32_t clear_bg = (alpha << 24) | (bg_r << 16) | (bg_g << 8) | bg_b;

    // Composite the background (plain color or image) — rebuilt on resize or
    // when a runtime color change (OSC 11) invalidated it.
    int bg_rebuilt = (bg_dirty || bg_bw != g_width || bg_bh != g_height);
    if (bg_rebuilt) build_background(g_width, g_height);

    // Normalized selection bounds (shared by every cell in this frame)
    int sel_on = g_select_active;
    int r1 = g_select_start_row, c1 = g_select_start_col;
    int r2 = g_select_end_row, c2 = g_select_end_col;
    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        int tr = r1; r1 = r2; r2 = tr;
        int tc = c1; c1 = c2; c2 = tc;
    }

    FrameShadow *sh = shadow_for(g_framebuffer, state->cols, state->rows);

    int bell_on = g_bell_flash_until != 0;
    int had_images = state->kitty_placements != NULL;
    int cursor_changed = 0;
    int full = 1;
    if (sh && sh->cached) {
        full = bg_rebuilt || bell_on || sh->bell_on || had_images ||
               sh->had_images != had_images ||
               sh->scroll_offset != state->scroll_offset ||
               sh->sel_on != sel_on ||
               (sel_on && (sh->sel_r1 != r1 || sh->sel_c1 != c1 ||
                           sh->sel_r2 != r2 || sh->sel_c2 != c2));
        cursor_changed = (sh->cursor_x != state->cursor_x ||
                          sh->cursor_y != state->cursor_y ||
                          sh->cursor_on != cursor_shown_now ||
                          sh->cursor_shape != g_config.cursor_shape);
    }

    // A full frame repaints the whole window background first (margins and
    // any strip below the last grid row included).
    if (full) {
        if (bg_buffer) {
            memcpy(g_framebuffer, bg_buffer, (size_t)g_width * g_height * 4);
        } else {
            for (int i = 0; i < g_width * g_height; i++) g_framebuffer[i] = clear_bg;
        }
        draw_kitty_images(state, g_framebuffer, 0, -1);
    }

    Cell blank = blank_row_cell();
    int y0 = g_height, y1 = 0;
    if (full) { y0 = 0; y1 = g_height; } // the whole background was rewritten


    for (int y = 0; y < state->rows; y++) {
        int start_y = g_config.padding_top + y * g_cell_height;
        if (start_y >= g_height) break;
        const Cell *src = visible_row(state, y);
        Cell *shadow_row = sh ? sh->cells + (size_t)y * state->cols : NULL;

        // Collect the exact dirty cell runs for this row: a few scattered
        // cells (animations like cmatrix) then repaint a few cells instead
        // of whole lines. Fragmented or italic rows fall back to a full row.
        #define MAX_ROW_RUNS 12
        int run_start[MAX_ROW_RUNS], run_end[MAX_ROW_RUNS];
        int nruns = 0;
        int full_row = full;

        // The cursor changed position/visibility: the row it left and the
        // row it moved to must be repainted regardless of cell content.
        if (!full_row && cursor_changed) {
            int logical_y = y - state->scroll_offset;
            if ((state->cursor_y == logical_y && cursor_shown_now) ||
                (sh && sh->cursor_y == logical_y && sh->cursor_on)) full_row = 1;
        }

        if (!full_row) {
            if (!src) {
                // A row with no backing cells (scrolled past the oldest line)
                if (!row_is_blank(shadow_row, state->cols, blank)) full_row = 1;
            } else if (memcmp(shadow_row, src, (size_t)state->cols * sizeof(Cell)) != 0) {
                int i = 0;
                while (i < state->cols) {
                    while (i < state->cols &&
                           memcmp(&shadow_row[i], &src[i], sizeof(Cell)) == 0) i++;
                    if (i >= state->cols) break;
                    int start = i;
                    while (i < state->cols &&
                           memcmp(&shadow_row[i], &src[i], sizeof(Cell)) != 0) i++;
                    if (nruns == MAX_ROW_RUNS) { full_row = 1; break; }
                    run_start[nruns] = start;
                    run_end[nruns] = i;
                    nruns++;
                }

                if (!full_row) {
                    // Expand runs to cover both halves of double-width
                    // glyphs, then merge whatever that makes overlap.
                    int out = 0;
                    for (int r = 0; r < nruns; r++) {
                        int a = run_start[r], b = run_end[r];
                        while (a > 0 && (((src[a].attrs | shadow_row[a].attrs) & CELL_TRAIL) ||
                                         ((src[a - 1].attrs | shadow_row[a - 1].attrs) & CELL_WIDE))) a--;
                        while (b < state->cols &&
                               (((src[b - 1].attrs | shadow_row[b - 1].attrs) & CELL_WIDE) ||
                                ((src[b].attrs | shadow_row[b].attrs) & CELL_TRAIL))) b++;
                        if (out > 0 && a <= run_end[out - 1]) {
                            if (b > run_end[out - 1]) run_end[out - 1] = b;
                        } else {
                            run_start[out] = a;
                            run_end[out] = b;
                            out++;
                        }
                    }
                    nruns = out;

                    // Italic glyphs lean past their cell, so any italic cell
                    // on the row forces a full-row repaint (which is exact).
                    for (int i = 0; i < state->cols; i++) {
                        if ((src[i].attrs | shadow_row[i].attrs) & CELL_ITALIC) { full_row = 1; break; }
                    }
                }
            }
        }

        if (full_row) {
            run_start[0] = 0;
            run_end[0] = state->cols;
            nruns = 1;
        }
        if (nruns == 0) continue;

        if (!full) {
            // Reset the repainted runs to the background first; otherwise
            // glyphs that disappeared would leave stale pixels behind, since
            // cell painting blends over the old pixels.
            int end_y = start_y + g_cell_height;
            if (end_y > g_height) end_y = g_height;
            for (int r = 0; r < nruns; r++) {
                int sx = g_config.padding_left + run_start[r] * g_cell_width;
                int ex = g_config.padding_left + run_end[r] * g_cell_width;
                if (ex > g_width) ex = g_width;
                if (sx >= ex) continue;
                for (int ry = start_y; ry < end_y; ry++) {
                    uint32_t *dst = g_framebuffer + (size_t)ry * g_width + sx;
                    if (bg_buffer) {
                        memcpy(dst, bg_buffer + (size_t)ry * g_width + sx,
                               (size_t)(ex - sx) * 4);
                    } else {
                        for (int i = sx; i < ex; i++) g_framebuffer[(size_t)ry * g_width + i] = clear_bg;
                    }
                }
            }
        }

        for (int r = 0; r < nruns; r++) {
            draw_row_span(state, y, run_start[r], run_end[r], src, clear_bg,
                          cursor_shown_now, sel_on, r1, c1, r2, c2);
        }
        if (sh) {
            if (src) {
                for (int r = 0; r < nruns; r++) {
                    memcpy(shadow_row + run_start[r], src + run_start[r],
                           (size_t)(run_end[r] - run_start[r]) * sizeof(Cell));
                }
            } else {
                row_fill_blank(shadow_row, state->cols, blank);
            }
        }
        if (start_y < y0) y0 = start_y;
        int end_y = start_y + g_cell_height;
        if (end_y > g_height) end_y = g_height;
        if (end_y > y1) y1 = end_y;
    }

    if (full) draw_kitty_images(state, g_framebuffer, 0, 1);

    // Visual bell flash: overlay the foreground color, fading out
    if (bell_on) {
        int64_t now_ms = bell_now_ms();
        if (now_ms < g_bell_flash_until && g_bell_flash_until > g_bell_flash_start) {
            float t = 1.0f - (float)(now_ms - g_bell_flash_start) /
                              (float)(g_bell_flash_until - g_bell_flash_start);
            uint32_t alpha = (uint32_t)(96.0f * t); // max ~38% overlay
            uint32_t fr = (g_config.fg_color >> 16) & 0xFF;
            uint32_t fgc = (g_config.fg_color >> 8) & 0xFF;
            uint32_t fb2 = g_config.fg_color & 0xFF;
            int total = g_width * g_height;
            for (int i = 0; i < total; i++) {
                uint32_t px = g_framebuffer[i];
                uint32_t pa = (px >> 24) & 0xFF;
                uint32_t r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
                r = div255(fr * alpha + r * (255 - alpha));
                g = div255(fgc * alpha + g * (255 - alpha));
                b = div255(fb2 * alpha + b * (255 - alpha));
                g_framebuffer[i] = (pa << 24) | (r << 16) | (g << 8) | b;
            }
            // The overlay tinted every pixel
            y0 = 0;
            y1 = g_height;
        }
    }

    if (sh) {
        sh->cached = 1;
        sh->cursor_x = state->cursor_x;
        sh->cursor_y = state->cursor_y;
        sh->cursor_on = cursor_shown_now;
        sh->cursor_shape = g_config.cursor_shape;
        sh->scroll_offset = state->scroll_offset;
        sh->sel_on = sel_on;
        sh->sel_r1 = r1; sh->sel_c1 = c1; sh->sel_r2 = r2; sh->sel_c2 = c2;
        sh->bell_on = bell_on;
        sh->had_images = had_images;
    }

    if (y1 <= y0) {
        g_damage_y0 = g_damage_y1 = 0;
        return 0;
    }
    g_damage_y0 = y0;
    g_damage_y1 = y1;
    return 1;
}
