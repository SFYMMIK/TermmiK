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
#include <fontconfig/fontconfig.h>
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
    if (bg_buffer) my_free(bg_buffer);
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

    if (!bg_src) {
        for (int i = 0; i < w * h; i++) bg_buffer[i] = plain;
        return;
    }

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

typedef struct {
    unsigned char *bitmap;
    int w, h, xoff, yoff;
} Glyph;

#define GLYPH_CACHE_SIZE 65536
static Glyph g_glyph_cache[GLYPH_CACHE_SIZE];

#define MAX_FONTS 64
static stbtt_fontinfo g_fonts[MAX_FONTS];
static float g_font_scales[MAX_FONTS];
static int g_num_fonts = 0;
static unsigned char gamma_table[256];


static void* map_font(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }
    void *ptr = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    return (ptr == MAP_FAILED) ? NULL : ptr;
}

int render_init(const char *font_pattern) {
    for (int i = 0; i < 256; i++) {
        // Gamma correction to thicken text (makes fonts look "fatter" like Kitty)
        float v = i / 255.0f;
        gamma_table[i] = (unsigned char)(powf(v, 1.0f / 1.5f) * 255.0f);
    }

    FcConfig *config = FcInitLoadConfigAndFonts();
    FcPattern *pat = FcNameParse((const FcChar8 *)font_pattern);
    FcConfigSubstitute(config, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    
    FcResult result;
    FcFontSet *fs = FcFontSort(config, pat, FcTrue, NULL, &result);
    if (fs) {
        for (int i = 0; i < fs->nfont && g_num_fonts < MAX_FONTS; i++) {
            FcChar8 *file;
            if (FcPatternGetString(fs->fonts[i], FC_FILE, 0, &file) == FcResultMatch) {
                unsigned char *ttf_buf = map_font((const char *)file);
                if (ttf_buf) {
                    int offset = stbtt_GetFontOffsetForIndex(ttf_buf, 0);
                    if (offset >= 0 && stbtt_InitFont(&g_fonts[g_num_fonts], ttf_buf, offset)) {
                        g_font_scales[g_num_fonts] = stbtt_ScaleForPixelHeight(&g_fonts[g_num_fonts], g_config.font_size * 1.333333f);
                        g_num_fonts++;
                    }
                }
            }
        }
        FcFontSetDestroy(fs);
    }
    FcPatternDestroy(pat);
    FcConfigDestroy(config);
    memset(g_glyph_cache, 0, sizeof(g_glyph_cache));

    if (g_num_fonts > 0) {
        int ascent, descent, lineGap;
        stbtt_GetFontVMetrics(&g_fonts[0], &ascent, &descent, &lineGap);
        g_cell_height = (int)ceilf((ascent - descent + lineGap) * g_font_scales[0]);
        if (g_cell_height < 1) g_cell_height = 18;
        g_baseline = (int)roundf(ascent * g_font_scales[0]);

        int advance, lsb;
        stbtt_GetCodepointHMetrics(&g_fonts[0], 'M', &advance, &lsb);
        g_cell_width = (int)roundf(advance * g_font_scales[0]);
        if (g_cell_width < 1) g_cell_width = 9;
    }

    // Kitty-style cell metric tuning
    g_cell_width += g_config.adjust_column_width;
    g_cell_height += g_config.adjust_line_height;
    g_baseline += g_config.adjust_baseline;
    if (g_cell_width < 1) g_cell_width = 1;
    if (g_cell_height < 1) g_cell_height = 1;
    if (g_baseline < 1) g_baseline = 1;

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
// Wide (CJK/emoji) glyph cache — open-addressing hash, allocated lazily so we
// don't pay for a second full-size bitmap cache like the narrow one.
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t cp;
    int used;
    Glyph g;
} WideSlot;

#define WIDE_CACHE_SIZE 8192 // power of two
static WideSlot *g_wide_cache = NULL;

static Glyph *wide_glyph_lookup(uint32_t cp) {
    if (!g_wide_cache) {
        g_wide_cache = calloc(WIDE_CACHE_SIZE, sizeof(WideSlot));
        if (!g_wide_cache) return NULL;
    }
    uint32_t h = (cp * 2654435761u) & (WIDE_CACHE_SIZE - 1);
    for (int probe = 0; probe < 64; probe++) {
        WideSlot *s = &g_wide_cache[(h + probe) & (WIDE_CACHE_SIZE - 1)];
        if (!s->used) {
            int w = 0, h2 = 0, xoff = 0, yoff = 0;
            unsigned char *bitmap = NULL;
            for (int i = 0; i < g_num_fonts; i++) {
                if (stbtt_FindGlyphIndex(&g_fonts[i], cp) != 0) {
                    bitmap = stbtt_GetCodepointBitmap(&g_fonts[i], 0, g_font_scales[i], cp, &w, &h2, &xoff, &yoff);
                    if (bitmap) break;
                }
            }
            if (!bitmap) {
                bitmap = malloc(1);
                if (bitmap) bitmap[0] = 0;
                w = 1; h2 = 1; xoff = 0; yoff = 0;
                if (!bitmap) return NULL;
            }
            s->cp = cp;
            s->used = 1;
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


// ---------------------------------------------------------------------------
// Animated cursor trail (kitty-style): when the logical cursor jumps, the
// block cursor glides from its previous position to the new one with an
// ease-out curve, leaving a fading smear. Purely cosmetic — render_draw
// detects the move, the main loop ticks frames at ~80 fps while active.
// ---------------------------------------------------------------------------
#define TRAIL_DURATION_MS 60

static int trail_active = 0;
static int64_t trail_start = 0, trail_end = 0;
static float trail_from_x = 0, trail_from_y = 0; // cell coords (float)
static float trail_to_x = 0, trail_to_y = 0;
static int last_cursor_x = -1, last_cursor_y = -1;

int cursor_trail_active(void) { return trail_active; }

static void fill_solid_rect(uint32_t *fb, int x0, int y0, int x1, int y1, uint32_t color) {
    for (int y = y0; y < y1; y++) {
        if (y < 0 || y >= g_height) continue;
        for (int x = x0; x < x1; x++) {
            if (x < 0 || x >= g_width) continue;
            fb[y * g_width + x] = color;
        }
    }
}

static void blend_rect(uint32_t *fb, int x0, int y0, int x1, int y1, uint32_t color, uint32_t alpha) {
    if (alpha == 0) return;
    uint32_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    for (int y = y0; y < y1; y++) {
        if (y < 0 || y >= g_height) continue;
        for (int x = x0; x < x1; x++) {
            if (x < 0 || x >= g_width) continue;
            uint32_t px = fb[y * g_width + x];
            uint32_t dr = (px >> 16) & 0xFF, dg = (px >> 8) & 0xFF, db = px & 0xFF;
            uint32_t da = (px >> 24) & 0xFF;
            uint32_t orr = div255(r * alpha + dr * (255 - alpha));
            uint32_t og = div255(g * alpha + dg * (255 - alpha));
            uint32_t ob = div255(b * alpha + db * (255 - alpha));
            uint32_t oa = alpha + div255(da * (255 - alpha));
            fb[y * g_width + x] = (oa << 24) | (orr << 16) | (og << 8) | ob;
        }
    }
}

// Blit a cached glyph at an arbitrary pixel position (used by the trail so
// the character rides the cursor)
static void blit_glyph_at(const Glyph *b, int base_x, int base_y, uint32_t fg) {
    int yoff = b->yoff + g_baseline;
    uint32_t fr = (fg >> 16) & 0xFF, fgc = (fg >> 8) & 0xFF, fb2 = fg & 0xFF;
    for (int cy = 0; cy < b->h; cy++) {
        for (int cx = 0; cx < b->w; cx++) {
            unsigned char a = gamma_table[b->bitmap[cy * b->w + cx]];
            if (!a) continue;
            int sx = base_x + b->xoff + cx;
            int sy = base_y + yoff + cy;
            if (sx < 0 || sx >= g_width || sy < 0 || sy >= g_height) continue;
            uint32_t dst = g_framebuffer[sy * g_width + sx];
            uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
            uint32_t da = (dst >> 24) & 0xFF;
            uint32_t orr = div255(fr * a + dr * (255 - a));
            uint32_t og = div255(fgc * a + dg * (255 - a));
            uint32_t ob = div255(fb2 * a + db * (255 - a));
            uint32_t oa = a + div255(da * (255 - a));
            g_framebuffer[sy * g_width + sx] = (oa << 24) | (orr << 16) | (og << 8) | ob;
        }
    }
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

void render_draw(VTState *state) {

    if (!g_framebuffer) return;

    static uint32_t *shadow_buffer = NULL;
    static int shadow_w = 0, shadow_h = 0;
    if (shadow_w != g_width || shadow_h != g_height || !shadow_buffer) {
        if (shadow_buffer) my_free(shadow_buffer);
        shadow_buffer = my_malloc(g_width * g_height * 4);
        if (!shadow_buffer) {
            shadow_w = 0;
            shadow_h = 0;
            return;
        }
        shadow_w = g_width;
        shadow_h = g_height;
        // Reset trail state on window resize
        trail_active = 0;
        last_cursor_x = -1;
        last_cursor_y = -1;
    }
    uint32_t *real_fb = g_framebuffer;
    uint32_t *g_framebuffer = shadow_buffer;

    // Cursor trail: detect a cursor jump and (re)start the glide animation
    int cursor_shown_now = state->cursor_visible &&
                           (!g_config.cursor_blink || g_cursor_blink_on);
    int trail_on = g_config.cursor_trail && g_config.cursor_shape == 0 &&
                   state->scroll_offset == 0 && cursor_shown_now;
    if (state->cursor_x != last_cursor_x || state->cursor_y != last_cursor_y) {
        if (trail_on && last_cursor_x >= 0) {
            float fx = last_cursor_x, fy = last_cursor_y;
            if (trail_active) {
                // Retarget from wherever the animation currently is
                float t = (bell_now_ms() - trail_start) / (float)(trail_end - trail_start);
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                float e = 1 - (1 - t) * (1 - t);
                fx = trail_from_x + (trail_to_x - trail_from_x) * e;
                fy = trail_from_y + (trail_to_y - trail_from_y) * e;
            }
            trail_from_x = fx;
            trail_from_y = fy;
            trail_to_x = state->cursor_x;
            trail_to_y = state->cursor_y;
            trail_start = bell_now_ms();
            trail_end = trail_start + TRAIL_DURATION_MS;
            trail_active = 1;
        }
        last_cursor_x = state->cursor_x;
        last_cursor_y = state->cursor_y;
    }
    int trail_now = trail_active;
    if (trail_active && bell_now_ms() >= trail_end) trail_active = 0;

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
    if (bg_dirty || bg_bw != g_width || bg_bh != g_height) {
        build_background(g_width, g_height);
    }
    if (bg_buffer) {
        memcpy(g_framebuffer, bg_buffer, g_width * g_height * 4);
    } else {
        for (int i = 0; i < g_width * g_height; i++) {
            g_framebuffer[i] = clear_bg;
        }
    }

    draw_kitty_images(state, g_framebuffer, 0, -1);

    for (int y = 0; y < state->rows; y++) {
        Cell *scroll_line = NULL;
        int logical_y = y - state->scroll_offset;
        
        if (logical_y < 0) {
            int max_sb = g_config.scrollback_lines;
            if (max_sb > MAX_SCROLLBACK) max_sb = MAX_SCROLLBACK;
            if (max_sb <= 0) max_sb = 1;

            int back = -logical_y;
            if (back <= state->scrollback_count) {
                int real_idx = (state->scrollback_head - back + max_sb) % max_sb;
                scroll_line = state->scrollback[real_idx].cells;
            }
        }
        
        for (int x = 0; x < state->cols; x++) {
            Cell c;
            if (scroll_line) {
                c = scroll_line[x];
            } else if (logical_y >= 0 && logical_y < state->rows) {
                c = state->cells[logical_y * state->cols + x];
            } else {
                c.char_code = ' '; c.fg_color = g_config.fg_color; c.bg_color = g_config.bg_color;
                c.wrapped = 0; c.attrs = 0;
            }
            
            uint32_t bg = c.bg_color;
            uint32_t fg = c.fg_color;
            uint8_t cattrs = c.attrs;

            // Double-width characters span two cells
            int cell_w = (cattrs & CELL_WIDE) ? 2 * g_cell_width : g_cell_width;

            int is_selected = 0;
            if (g_select_active) {
                int r1 = g_select_start_row, c1 = g_select_start_col;
                int r2 = g_select_end_row, c2 = g_select_end_col;
                if (r1 > r2 || (r1 == r2 && c1 > c2)) {
                    int tr = r1; r1 = r2; r2 = tr;
                    int tc = c1; c1 = c2; c2 = tc;
                }
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
            
            extern int g_cursor_blink_on;
            int cursor_shown = state->cursor_visible &&
                               (!g_config.cursor_blink || g_cursor_blink_on);
            int is_cursor = (cursor_shown && logical_y == state->cursor_y && x == state->cursor_x);
            if (trail_now) is_cursor = 0; // the animated block replaces it
            if (is_cursor && g_config.cursor_shape == 0) {
                bg = g_config.cursor_color;
                fg = g_config.cursor_text_color_set ? g_config.cursor_text_color : c.bg_color;
            }
            
            uint32_t bg_pixel = clear_bg;
            int start_x = g_config.padding_left + x * g_cell_width;
            int start_y = g_config.padding_top + y * g_cell_height;

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
            
            if (g_num_fonts > 0 && !(cattrs & CELL_TRAIL)) {
                Glyph *b = NULL;
                if (cattrs & CELL_WIDE) {
                    // Double-width glyph from the wide cache
                    if (c.char_code >= 32 && c.char_code != ' ') {
                        b = wide_glyph_lookup(c.char_code);
                    }
                } else if (c.char_code >= 32 && c.char_code < GLYPH_CACHE_SIZE) {
                    if (!g_glyph_cache[c.char_code].bitmap && c.char_code != ' ') {
                        int w = 0, h = 0, xoff = 0, yoff = 0;
                        unsigned char *bitmap = NULL;

                        if (is_procedural_glyph(c.char_code)) {
                            // Draw box/block glyphs ourselves so they fill the whole
                            // cell and connect seamlessly with their neighbors.
                            w = g_cell_width; h = g_cell_height;
                            bitmap = calloc(1, w * h);
                            if (bitmap) {
                                synth_glyph(c.char_code, bitmap, w, h);
                                xoff = 0;
                                yoff = -g_baseline;
                            } else {
                                w = 1; h = 1;
                            }
                        }

                        if (!bitmap) {
                            for (int i = 0; i < g_num_fonts; i++) {
                                if (stbtt_FindGlyphIndex(&g_fonts[i], c.char_code) != 0) {
                                    bitmap = stbtt_GetCodepointBitmap(&g_fonts[i], 0, g_font_scales[i], c.char_code, &w, &h, &xoff, &yoff);
                                    if (bitmap) break;
                                }
                            }
                        }
                        if (!bitmap) {
                            bitmap = malloc(1); bitmap[0] = 0; w = 1; h = 1; xoff = 0; yoff = 0;
                        }
                        g_glyph_cache[c.char_code].bitmap = bitmap;
                        g_glyph_cache[c.char_code].w = w; g_glyph_cache[c.char_code].h = h;
                        g_glyph_cache[c.char_code].xoff = xoff; g_glyph_cache[c.char_code].yoff = yoff;
                    }
                    b = &g_glyph_cache[c.char_code];
                } else if (c.char_code >= GLYPH_CACHE_SIZE) {
                    // Astral-plane narrow characters — hash cache covers them
                    if (c.char_code >= 32) b = wide_glyph_lookup(c.char_code);
                }
                
                if (b && b->bitmap && c.char_code != ' ') {
                    int passes = (cattrs & CELL_BOLD) ? 2 : 1;
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
                            for (int cx = 0; cx < max_cx; cx++) {
                                int pX = start_x + xoff + cx + shear + pass;
                                int pY = start_y + yoff + cy;
                                if (pX >= 0 && pX < g_width && pY >= 0 && pY < g_height) {
                                    unsigned char alpha = gamma_table[b->bitmap[cy * w + cx]];
                                    if (cattrs & CELL_DIM) alpha = (alpha * 2) / 3;
                                    if (alpha > 0) {
                                        uint32_t dst = g_framebuffer[pY * g_width + pX];
                                        uint32_t fg_r = (fg >> 16) & 0xFF, fg_g = (fg >> 8) & 0xFF, fg_b = fg & 0xFF;
                                        uint32_t bg_r = (dst >> 16) & 0xFF, bg_g = (dst >> 8) & 0xFF, bg_b = dst & 0xFF;
                                        uint32_t dst_a = (dst >> 24) & 0xFF;
                                        uint32_t r = div255(fg_r * alpha + bg_r * (255 - alpha));
                                        uint32_t g2 = div255(fg_g * alpha + bg_g * (255 - alpha));
                                        uint32_t b2 = div255(fg_b * alpha + bg_b * (255 - alpha));
                                        uint32_t new_a = alpha + div255(dst_a * (255 - alpha));
                                        g_framebuffer[pY * g_width + pX] = (new_a << 24) | (r << 16) | (g2 << 8) | b2;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Underline / strikethrough decorations (skip on trail halves)
            if (!(cattrs & CELL_TRAIL) && fg != bg && (cattrs & (CELL_UNDERLINE | CELL_STRIKE))) {
                int t = line_thickness(g_cell_height);
                if (cattrs & CELL_UNDERLINE) {
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
    
    // Animated cursor trail: glide block + fading smear + riding glyph
    if (trail_now) {
        float t = (bell_now_ms() - trail_start) / (float)(trail_end - trail_start);
        if (t < 0) t = 0;
        if (t >= 1) {
            trail_active = 0;
        } else {
            float e = 1 - (1 - t) * (1 - t); // ease-out quad
            float ax = trail_from_x + (trail_to_x - trail_from_x) * e;
            float ay = trail_from_y + (trail_to_y - trail_from_y) * e;
            int px = g_config.padding_left + (int)(ax * g_cell_width);
            int py = g_config.padding_top + (int)(ay * g_cell_height);
            int fx = g_config.padding_left + (int)(trail_from_x * g_cell_width);
            int fy = g_config.padding_top + (int)(trail_from_y * g_cell_height);

            // Fading smear from the origin cell to the current position
            int tail_alpha = (int)(110 * (1 - e));
            if (tail_alpha > 0) {
                int x0 = px < fx ? px : fx;
                int y0 = py < fy ? py : fy;
                int x1 = (px > fx ? px : fx) + g_cell_width;
                int y1 = (py > fy ? py : fy) + g_cell_height;
                blend_rect(g_framebuffer, x0, y0, x1, y1, g_config.cursor_color, tail_alpha);
            }

            // The cursor block itself
            fill_solid_rect(g_framebuffer, px, py, px + g_cell_width, py + g_cell_height,
                            g_config.cursor_color | 0xFF000000);

            // The destination character rides the cursor
            Cell cc = state->cells[state->cursor_y * state->cols + state->cursor_x];
            if (!(cc.attrs & (CELL_WIDE | CELL_TRAIL)) &&
                cc.char_code >= 32 && cc.char_code < GLYPH_CACHE_SIZE) {
                Glyph *b = &g_glyph_cache[cc.char_code];
                if (b->bitmap && b->w > 1) {
                    uint32_t textfg = g_config.cursor_text_color_set
                                          ? g_config.cursor_text_color
                                          : cc.bg_color;
                    blit_glyph_at(b, px, py, textfg);
                }
            }
        }
    }

    draw_kitty_images(state, g_framebuffer, 0, 1);

    // Visual bell flash: overlay the foreground color, fading out
    extern int64_t g_bell_flash_until;
    extern int64_t g_bell_flash_start;
    if (g_bell_flash_until) {
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
        }
    }
    
    memcpy(real_fb, shadow_buffer, g_width * g_height * 4);
}
