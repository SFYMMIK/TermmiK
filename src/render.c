/*
 * TermmiK
 * Copyright (C) 2026 Szymon Grajner (SfymmiK)
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
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

int g_cell_width = 9;
int g_cell_height = 18;
int g_baseline = 14;

typedef struct {
    unsigned char *bitmap;
    int w, h, xoff, yoff;
    int advance;
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
                    if (stbtt_InitFont(&g_fonts[g_num_fonts], ttf_buf, stbtt_GetFontOffsetForIndex(ttf_buf, 0))) {
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

    return 0;
}

extern int g_select_active;
extern int g_select_start_row;
extern int g_select_start_col;
extern int g_select_end_row;
extern int g_select_end_col;

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
    }
    uint32_t *real_fb = g_framebuffer;
    uint32_t *g_framebuffer = shadow_buffer;

    uint32_t alpha = (uint32_t)(g_config.opacity * 255.0f);
    if (alpha > 255) alpha = 255;
    uint32_t bg_r = (g_config.bg_color >> 16) & 0xFF;
    uint32_t bg_g = (g_config.bg_color >> 8) & 0xFF;
    uint32_t bg_b = g_config.bg_color & 0xFF;
    bg_r = (bg_r * alpha) / 255;
    bg_g = (bg_g * alpha) / 255;
    bg_b = (bg_b * alpha) / 255;
    uint32_t clear_bg = (alpha << 24) | (bg_r << 16) | (bg_g << 8) | bg_b;

    for (int i = 0; i < g_width * g_height; i++) {
        g_framebuffer[i] = clear_bg;
    }

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
            }
            
            uint32_t bg = c.bg_color;
            uint32_t fg = c.fg_color;

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
                bg = c.fg_color;
                fg = c.bg_color;
            }
            
            int is_cursor = (state->cursor_visible && logical_y == state->cursor_y && x == state->cursor_x);
            if (is_cursor && g_config.cursor_shape == 0) {
                bg = g_config.cursor_color;
                fg = c.bg_color;
            }
            
            uint32_t bg_pixel = clear_bg;
            if (bg != g_config.bg_color) {
                bg_pixel = bg | 0xFF000000;
            }

            int start_x = g_config.padding_x + x * g_cell_width;
            int start_y = g_config.padding_y + y * g_cell_height;

            for (int cy = 0; cy < g_cell_height; cy++) {
                int screen_y = start_y + cy;
                if (screen_y >= g_height) break;
                for (int cx = 0; cx < g_cell_width; cx++) {
                    int screen_x = start_x + cx;
                    if (screen_x >= g_width) break;
                    g_framebuffer[screen_y * g_width + screen_x] = bg_pixel;
                }
            }
            
            if (g_num_fonts > 0 && c.char_code >= 32 && c.char_code < GLYPH_CACHE_SIZE) {
                if (!g_glyph_cache[c.char_code].bitmap && c.char_code != ' ') {
                    int w = 0, h = 0, xoff = 0, yoff = 0;
                    unsigned char *bitmap = NULL;
                    
                    for (int i = 0; i < g_num_fonts; i++) {
                        if (stbtt_FindGlyphIndex(&g_fonts[i], c.char_code) != 0) {
                            bitmap = stbtt_GetCodepointBitmap(&g_fonts[i], 0, g_font_scales[i], c.char_code, &w, &h, &xoff, &yoff);
                            if (bitmap) break;
                        }
                    }
                    if (!bitmap && c.char_code >= 0x2500 && c.char_code <= 0x257F) {
                        w = g_cell_width; h = g_cell_height; xoff = 0; yoff = -(g_cell_height * 14 / 18);
                        bitmap = calloc(1, w * h);
                        int mid_x = w / 2;
                        int mid_y = h / 2;
                        if (c.char_code == 0x2500) { for (int i=0; i<w; i++) bitmap[mid_y * w + i] = 255; }
                        else if (c.char_code == 0x2502) { for (int i=0; i<h; i++) bitmap[i * w + mid_x] = 255; }
                        else if (c.char_code == 0x250C) { for (int i=mid_x; i<w; i++) bitmap[mid_y * w + i] = 255; for (int i=mid_y; i<h; i++) bitmap[i * w + mid_x] = 255; }
                        else if (c.char_code == 0x2510) { for (int i=0; i<=mid_x; i++) bitmap[mid_y * w + i] = 255; for (int i=mid_y; i<h; i++) bitmap[i * w + mid_x] = 255; }
                        else if (c.char_code == 0x2514) { for (int i=mid_x; i<w; i++) bitmap[mid_y * w + i] = 255; for (int i=0; i<=mid_y; i++) bitmap[i * w + mid_x] = 255; }
                        else if (c.char_code == 0x2518) { for (int i=0; i<=mid_x; i++) bitmap[mid_y * w + i] = 255; for (int i=0; i<=mid_y; i++) bitmap[i * w + mid_x] = 255; }
                        else if (c.char_code == 0x251C) { for (int i=mid_x; i<w; i++) bitmap[mid_y * w + i] = 255; for (int i=0; i<h; i++) bitmap[i * w + mid_x] = 255; }
                        else if (c.char_code == 0x2524) { for (int i=0; i<=mid_x; i++) bitmap[mid_y * w + i] = 255; for (int i=0; i<h; i++) bitmap[i * w + mid_x] = 255; }
                        else if (c.char_code == 0x252C) { for (int i=0; i<w; i++) bitmap[mid_y * w + i] = 255; for (int i=mid_y; i<h; i++) bitmap[i * w + mid_x] = 255; }
                        else if (c.char_code == 0x2534) { for (int i=0; i<w; i++) bitmap[mid_y * w + i] = 255; for (int i=0; i<=mid_y; i++) bitmap[i * w + mid_x] = 255; }
                        else if (c.char_code == 0x253C) { for (int i=0; i<w; i++) bitmap[mid_y * w + i] = 255; for (int i=0; i<h; i++) bitmap[i * w + mid_x] = 255; }
                        else if (c.char_code == 0x2592) { for (int i=0; i<w*h; i+=2) bitmap[i] = 128; }
                    } else if (!bitmap) {
                        bitmap = malloc(1); bitmap[0] = 0; w = 1; h = 1; xoff = 0; yoff = 0;
                    }
                    g_glyph_cache[c.char_code].bitmap = bitmap;
                    g_glyph_cache[c.char_code].w = w; g_glyph_cache[c.char_code].h = h;
                    g_glyph_cache[c.char_code].xoff = xoff; g_glyph_cache[c.char_code].yoff = yoff;
                }
                
                if (c.char_code != ' ') {
                    Glyph *b = &g_glyph_cache[c.char_code];
                    int w = b->w; int h = b->h; int xoff = b->xoff; int yoff = b->yoff + g_baseline;
                    for (int cy = 0; cy < h; cy++) {
                        for (int cx = 0; cx < w; cx++) {
                            int pX = start_x + xoff + cx; int pY = start_y + yoff + cy;
                            if (pX >= 0 && pX < g_width && pY >= 0 && pY < g_height) {
                                unsigned char alpha = gamma_table[b->bitmap[cy * w + cx]];
                                if (alpha > 0) {
                                    uint32_t dst = g_framebuffer[pY * g_width + pX];
                                    uint32_t fg_r = (fg >> 16) & 0xFF, fg_g = (fg >> 8) & 0xFF, fg_b = fg & 0xFF;
                                    uint32_t bg_r = (dst >> 16) & 0xFF, bg_g = (dst >> 8) & 0xFF, bg_b = dst & 0xFF;
                                    uint32_t dst_a = (dst >> 24) & 0xFF;
                                    uint32_t r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
                                    uint32_t g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
                                    uint32_t b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;
                                    uint32_t new_a = alpha + (dst_a * (255 - alpha)) / 255;
                                    g_framebuffer[pY * g_width + pX] = (new_a << 24) | (r << 16) | (g << 8) | b;
                                }
                            }
                        }
                    }
                }
            }

            if (is_cursor) {
                uint32_t c_color = g_config.cursor_color | 0xFF000000;
                if (g_config.cursor_shape == 1) {
                    int cy = start_y + g_cell_height - 2;
                    for (int cx = 0; cx < g_cell_width; cx++) {
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
    
    memcpy(real_fb, shadow_buffer, g_width * g_height * 4);
}
