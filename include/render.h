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

#ifndef RENDER_H
#define RENDER_H


#include "vt_parser.h"

extern int g_cell_width;
extern int g_cell_height;
extern int g_baseline;

// A rasterized glyph (CPU bitmap + GPU atlas placement, filled lazily).
typedef struct {
    unsigned char *bitmap;
    int w, h, xoff, yoff;
    // GPU atlas cache — set by gpu.c after uploading
    float u0, v0, u1, v1;
    int in_atlas;
} Glyph;

int render_init(const char *font_pattern);

// Draw the frame into g_framebuffer. Only rows that actually changed since
// this buffer's previous frame are repainted. Returns:
//   1  pixels were written (g_damage_y0/y1 hold the affected pixel rows)
//   0  nothing changed, nothing was written
//  -1  no presentation buffer is available (drawing must be retried later)
int render_draw(VTState *state);

void render_cleanup(void);
void render_resize(int width, int height);
void render_invalidate_background(void); // rebuild after OSC 10/11 color changes

// Presentation buffer lifetime: forget cached frame state when a backend
// destroys its buffers (resize / renderer switch), so the next frame into a
// fresh buffer always repaints fully.
void render_forget_all_buffers(void);

// Re-rasterize every glyph at the current font size (runtime zoom) and
// re-apply a display scale factor (Wayland output scale / Xft.dpi).
void render_update_font(void);
void render_set_scale(float scale);

// The GPU renderer registers a callback that rebuilds its glyph atlas when
// the caches above are dropped.
void render_set_glyph_reset_callback(void (*cb)(void));

// Pixel rows [y0, y1) written by the last render_draw() (full width).
extern int g_damage_y0, g_damage_y1;

int64_t bell_now_ms(void);

// Shared glyph machinery (used by the CPU and GPU renderers)
Glyph *render_lookup_glyph(uint32_t cp, int wide);
unsigned char render_gamma(unsigned char v);
const unsigned char *render_bg_image(int *w, int *h);

#endif
