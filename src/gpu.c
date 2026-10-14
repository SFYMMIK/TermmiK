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

/*
 * GPU renderer — EGL/GLES2.
 *
 * The cell grid is turned into textured quads every frame: one glyph atlas
 * for text, solid quads for cell backgrounds/decorations/cursor, and one
 * texture per kitty-protocol image. Kitty images are uploaded to the GPU on
 * first use and their CPU copy is released, keeping system RAM flat like
 * kitty does.
 *
 * If EGL or a context is unavailable the CPU renderer in render.c stays in
 * charge (gpu_init returns non-zero and nothing here is touched).
 */

#include "alloc.h"
#include "backend.h"
#include "config.h"
#include "gpu.h"
#include "render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#ifdef _HAS_WAYLAND
#include <wayland-client.h>
#include <wayland-egl.h>
#endif

extern int64_t bell_now_ms(void);

int g_use_gpu = 0;

static int egl_platform_of_backend(void);

// ---------------------------------------------------------------------------
// GL state
// ---------------------------------------------------------------------------

static int gpu_ready = 0;

static EGLDisplay egl_dpy = EGL_NO_DISPLAY;
static EGLContext egl_ctx = EGL_NO_CONTEXT;
static EGLSurface egl_surf = EGL_NO_SURFACE;
static EGLConfig  egl_cfg = NULL;

#ifdef _HAS_WAYLAND
static struct wl_egl_window *wl_eglwin = NULL;
#endif

static GLuint prog_cell = 0;  // atlas alpha mask (text, rects)
static GLuint prog_img = 0;   // RGBA image over
static GLuint prog_bg = 0;    // background image mixed with the base color
static GLint  u_cell_view = -1, u_img_view = -1, u_bg_view = -1;
static GLint  u_bg_base = -1, u_bg_t = -1, u_bg_op = -1;

static GLuint atlas_tex = 0;
static GLuint white_tex = 0;  // 1x1 opaque white for solid quads
static GLuint bg_tex = 0;
static int    bg_tex_w = 0, bg_tex_h = 0;

#define ATLAS_W 2048
#define ATLAS_H 2048
static int atlas_x = 4, atlas_y = 0, atlas_row_h = 2;

// ---------------------------------------------------------------------------
// Vertex batch
// ---------------------------------------------------------------------------

typedef struct {
    GLfloat x, y;
    GLfloat u, v;
    GLfloat r, g, b, a;
} Vtx;

static Vtx *vbuf = NULL;
static int vcount = 0, vcap = 0;
static GLuint vbo = 0;

static void push_vertex(float x, float y, float u, float v,
                        float r, float g, float b, float a) {
    if (vcount >= vcap) {
        int ncap = vcap ? vcap * 2 : 65536;
        Vtx *nv = realloc(vbuf, (size_t)ncap * sizeof(Vtx));
        if (!nv) return;
        vbuf = nv;
        vcap = ncap;
    }
    Vtx *p = &vbuf[vcount++];
    p->x = x; p->y = y; p->u = u; p->v = v;
    p->r = r; p->g = g; p->b = b; p->a = a;
}

// Axis-aligned quad. v0 is the top edge, v1 the bottom edge.
static void push_rect(float x, float y, float w, float h,
                      float u0, float v0, float u1, float v1,
                      float r, float g, float b, float a) {
    push_vertex(x,     y,     u0, v0, r, g, b, a);
    push_vertex(x + w, y,     u1, v0, r, g, b, a);
    push_vertex(x + w, y + h, u1, v1, r, g, b, a);
    push_vertex(x,     y,     u0, v0, r, g, b, a);
    push_vertex(x + w, y + h, u1, v1, r, g, b, a);
    push_vertex(x,     y + h, u0, v1, r, g, b, a);
}

// Quads whose top/bottom edges are shifted horizontally (italic shear).
static void push_rect_shear(float x, float y, float w, float h,
                            float s_top, float s_bot,
                            float u0, float v0, float u1, float v1,
                            float r, float g, float b, float a) {
    float xt0 = x + s_top, xt1 = x + w + s_top;
    float xb0 = x + s_bot, xb1 = x + w + s_bot;
    push_vertex(xt0, y,     u0, v0, r, g, b, a);
    push_vertex(xt1, y,     u1, v0, r, g, b, a);
    push_vertex(xb1, y + h, u1, v1, r, g, b, a);
    push_vertex(xt0, y,     u0, v0, r, g, b, a);
    push_vertex(xb1, y + h, u1, v1, r, g, b, a);
    push_vertex(xb0, y + h, u0, v1, r, g, b, a);
}

static void flush_batch(void) {
    if (vcount == 0) return;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    size_t bytes = (size_t)vcount * sizeof(Vtx);
    glBufferData(GL_ARRAY_BUFFER, bytes, vbuf, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)(2 * sizeof(GLfloat)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)(4 * sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLES, 0, vcount);
    vcount = 0;
}

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

static const char *VS_SRC =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "attribute vec4 a_color;\n"
    "uniform vec2 u_view;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "  vec2 ndc = vec2(a_pos.x / u_view.x * 2.0 - 1.0,\n"
    "                  1.0 - a_pos.y / u_view.y * 2.0);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "  v_color = a_color;\n"
    "}\n";

// Cell pass: alpha mask (glyph atlas or white) tinted by the vertex color.
// Output is premultiplied for GL_ONE / GL_ONE_MINUS_SRC_ALPHA blending.
static const char *FS_CELL =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "  float m = texture2D(u_tex, v_uv).a;\n"
    "  float a = v_color.a * m;\n"
    "  gl_FragColor = vec4(v_color.rgb * a, a);\n"
    "}\n";

// RGBA image over the framebuffer.
static const char *FS_IMG =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "  vec4 t = texture2D(u_tex, v_uv);\n"
    "  float a = t.a * v_color.a;\n"
    "  gl_FragColor = vec4(t.rgb * a, a);\n"
    "}\n";

// Background image: image mixed over the base color, whole window at u_op
// alpha (matches the CPU compositor: (1-t)*base + t*image*imageA, * opacity).
static const char *FS_BG =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec3 u_base;\n"
    "uniform float u_t;\n"
    "uniform float u_op;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "  vec4 t = texture2D(u_tex, v_uv);\n"
    "  float mixf = u_t * t.a;\n"
    "  vec3 rgb = mix(u_base, t.rgb, mixf);\n"
    "  gl_FragColor = vec4(rgb * u_op, u_op);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "TermmiK: GPU shader error: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint make_program(const char *fs, GLint *view_uniform) {
    GLuint vs = compile(GL_VERTEX_SHADER, VS_SRC);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    if (!vs || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_uv");
    glBindAttribLocation(p, 2, "a_color");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(f);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "TermmiK: GPU link error: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    *view_uniform = glGetUniformLocation(p, "u_view");
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "u_tex"), 0);
    return p;
}

// ---------------------------------------------------------------------------
// Atlas
// ---------------------------------------------------------------------------

static void atlas_init(void) {
    glGenTextures(1, &atlas_tex);
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, ATLAS_W, ATLAS_H, 0,
                 GL_ALPHA, GL_UNSIGNED_BYTE, NULL);

    glGenTextures(1, &white_tex);
    glBindTexture(GL_TEXTURE_2D, white_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    unsigned char white[4] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 1, 1, 0, GL_ALPHA,
                 GL_UNSIGNED_BYTE, white);
}

// Rebuild the glyph atlas from scratch. Registered with the CPU renderer so
// a runtime font zoom / scale change can drop every cached glyph safely.
static void reset_glyph_atlas(void) {
    if (!gpu_ready || !atlas_tex) return;
    glDeleteTextures(1, &atlas_tex);
    atlas_tex = 0;
    atlas_x = 4;
    atlas_y = 0;
    atlas_row_h = 2;
    glGenTextures(1, &atlas_tex);
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, ATLAS_W, ATLAS_H, 0,
                 GL_ALPHA, GL_UNSIGNED_BYTE, NULL);
}

// Upload a glyph into the atlas on first use; sets UVs on the Glyph.
static int atlas_put(Glyph *g) {
    if (!g || !g->bitmap || g->w <= 0 || g->h <= 0) return 0;
    if (g->in_atlas) return 1;
    int w = g->w, h = g->h;
    if (w > ATLAS_W - 2 || h > ATLAS_H - 2) return 0;

    if (atlas_x + w + 1 > ATLAS_W) {
        atlas_x = 0;
        atlas_y += atlas_row_h + 1;
        atlas_row_h = 0;
    }
    if (atlas_y + h + 1 > ATLAS_H) return 0; // atlas full — drop the glyph

    unsigned char *tmp = malloc((size_t)w * h);
    if (!tmp) return 0;
    for (int i = 0; i < w * h; i++) tmp[i] = render_gamma(g->bitmap[i]);

    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, atlas_x, atlas_y, w, h,
                    GL_ALPHA, GL_UNSIGNED_BYTE, tmp);
    free(tmp);

    g->u0 = (atlas_x + 0.25f) / ATLAS_W;
    g->v0 = (atlas_y + 0.25f) / ATLAS_H;
    g->u1 = (atlas_x + w - 0.25f) / ATLAS_W;
    g->v1 = (atlas_y + h - 0.25f) / ATLAS_H;
    g->in_atlas = 1;

    atlas_x += w + 1;
    if (h > atlas_row_h) atlas_row_h = h;
    return 1;
}

// ---------------------------------------------------------------------------
// Kitty images
// ---------------------------------------------------------------------------

static int image_texture(KittyImage *im) {
    if (im->gl_ok) return 1;
    if (!im->pixels || im->w <= 0 || im->h <= 0) return 0;

    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, im->w, im->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, im->pixels);

    im->gl_tex = t;
    im->gl_ok = 1;
    // Pixels now live on the GPU (and every future frame draws from there) —
    // releasing them keeps image cost flat in system RAM, like kitty.
    my_free(im->pixels);
    im->pixels = NULL;
    return 1;
}

static void draw_images(VTState *state, int z_limit, int dir) {
    if (!state->kitty_placements) return;

    int count = 0;
    for (KittyPlacement *p = state->kitty_placements; p; p = p->next) count++;
    if (count == 0) return;
    KittyPlacement **arr = malloc((size_t)count * sizeof(*arr));
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
        if (!img || !image_texture(img)) continue;

        int start_x = g_config.padding_left + p->cell_x * g_cell_width;
        int start_y = g_config.padding_top + (p->cell_y + state->scroll_offset) * g_cell_height;
        int target_w = p->cols * g_cell_width;
        int target_h = p->rows * g_cell_height;
        if (target_w <= 0 || target_h <= 0) continue;
        if (start_y + target_h <= 0 || start_y >= g_height) continue;
        if (start_x + target_w <= 0 || start_x >= g_width) continue;

        int sw = p->src_w > 0 ? p->src_w : img->w;
        int sh = p->src_h > 0 ? p->src_h : img->h;
        float u0 = (p->src_x + 0.5f) / img->w;
        float u1 = (p->src_x + sw - 0.5f) / img->w;
        float v0 = (p->src_y + 0.5f) / img->h;
        float v1 = (p->src_y + sh - 0.5f) / img->h;

        glUseProgram(prog_img);
        glUniform2f(u_img_view, (float)g_width, (float)g_height);
        glBindTexture(GL_TEXTURE_2D, img->gl_tex);
        push_rect((float)start_x, (float)start_y, (float)target_w, (float)target_h,
                  u0, v0, u1, v1, 1.0f, 1.0f, 1.0f, 1.0f);
        flush_batch();
    }
    free(arr);
}

// ---------------------------------------------------------------------------
// Background
// ---------------------------------------------------------------------------

static void ensure_bg_texture(void) {
    int w = 0, h = 0;
    const unsigned char *src = render_bg_image(&w, &h);
    if (!src || w <= 0 || h <= 0) return;
    if (bg_tex && bg_tex_w == w && bg_tex_h == h) return;
    if (bg_tex) glDeleteTextures(1, &bg_tex);
    glGenTextures(1, &bg_tex);
    glBindTexture(GL_TEXTURE_2D, bg_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, src);
    bg_tex_w = w;
    bg_tex_h = h;
}

static void draw_background_image(void) {
    ensure_bg_texture();
    if (!bg_tex) return;

    int w = bg_tex_w, h = bg_tex_h;
    float t = g_config.background_image_opacity;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float op = g_config.opacity;
    if (op < 0) op = 0;
    if (op > 1) op = 1;
    float base[3] = {
        ((g_config.bg_color >> 16) & 0xFF) / 255.0f,
        ((g_config.bg_color >> 8) & 0xFF) / 255.0f,
        (g_config.bg_color & 0xFF) / 255.0f,
    };

    glUseProgram(prog_bg);
    glUniform2f(u_bg_view, (float)g_width, (float)g_height);
    glUniform3f(u_bg_base, base[0], base[1], base[2]);
    glUniform1f(u_bg_t, t);
    glUniform1f(u_bg_op, op);
    glBindTexture(GL_TEXTURE_2D, bg_tex);
    glDisable(GL_BLEND); // replaces the cleared pixels

    if (g_config.background_image_mode == 1) {
        // center, native size
        float ox = (g_width - w) / 2.0f, oy = (g_height - h) / 2.0f;
        push_rect(ox, oy, (float)w, (float)h, 0.0f, 0.0f, 1.0f, 1.0f,
                  1, 1, 1, 1);
    } else if (g_config.background_image_mode == 2) {
        // tile, native size, anchored at the top-left like the CPU renderer
        int tiles = 0;
        for (int ty = 0; ty * h < g_height && tiles < 4096; ty++) {
            for (int tx = 0; tx * w < g_width && tiles < 4096; tx++, tiles++) {
                push_rect((float)(tx * w), (float)(ty * h), (float)w, (float)h,
                          0.0f, 0.0f, 1.0f, 1.0f, 1, 1, 1, 1);
            }
        }
    } else {
        // stretch
        push_rect(0, 0, (float)g_width, (float)g_height,
                  0.0f, 0.0f, 1.0f, 1.0f, 1, 1, 1, 1);
    }
    flush_batch();
}

// ---------------------------------------------------------------------------
// Cells
// ---------------------------------------------------------------------------

extern int g_select_active;
extern int g_select_start_row;
extern int g_select_start_col;
extern int g_select_end_row;
extern int g_select_end_col;
extern int g_cursor_blink_on;

static void build_cells(VTState *state) {
    // Tri-state like the CPU renderer: 0 = off, 1 = blinking, 2 = steady;
    // both visible modes respect the application's ?25 hide request.
    int cursor_shown_now;
    if (g_config.cursor_blink == 0) cursor_shown_now = 0;
    else if (g_config.cursor_blink == 2) cursor_shown_now = state->cursor_visible;
    else cursor_shown_now = state->cursor_visible && g_cursor_blink_on;

    glUseProgram(prog_cell);
    glUniform2f(u_cell_view, (float)g_width, (float)g_height);
    glBindTexture(GL_TEXTURE_2D, atlas_tex);

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
                bg = g_config.selection_bg_set ? g_config.selection_background : c.fg_color;
                fg = g_config.selection_fg_set ? g_config.selection_foreground : c.bg_color;
            }

            int is_cursor = (cursor_shown_now && logical_y == state->cursor_y && x == state->cursor_x);
            if (is_cursor && g_config.cursor_shape == 0) {
                bg = g_config.cursor_color;
                fg = g_config.cursor_text_color_set ? g_config.cursor_text_color : c.bg_color;
            }

            float start_x = (float)(g_config.padding_left + x * g_cell_width);
            float start_y = (float)(g_config.padding_top + y * g_cell_height);

            if (bg != g_config.bg_color) {
                push_rect(start_x, start_y, (float)cell_w, (float)g_cell_height,
                          0, 0, 0, 0,
                          ((bg >> 16) & 0xFF) / 255.0f,
                          ((bg >> 8) & 0xFF) / 255.0f,
                          (bg & 0xFF) / 255.0f, 1.0f);
            }

            if (!(cattrs & CELL_TRAIL) && c.char_code != ' ') {
                Glyph *b = render_lookup_glyph(c.char_code, (cattrs & CELL_WIDE) ? 1 : 0);
                if (b && atlas_put(b)) {
                    int gw = b->w, gh = b->h;
                    float gx = start_x + b->xoff;
                    float gy = start_y + b->yoff + g_baseline;
                    float u0 = b->u0, u1 = b->u1, v0 = b->v0, v1 = b->v1;

                    if ((cattrs & CELL_WIDE) && b->xoff + gw > cell_w) {
                        int lim = cell_w - b->xoff;
                        if (lim < 0) lim = 0;
                        if (lim < gw && gw > 0) {
                            float frac = (float)lim / (float)gw;
                            u1 = u0 + (u1 - u0) * frac;
                            gw = lim;
                        }
                    }

                    if (gw > 0 && gh > 0) {
                        float fr = ((fg >> 16) & 0xFF) / 255.0f;
                        float fgc = ((fg >> 8) & 0xFF) / 255.0f;
                        float fb = (fg & 0xFF) / 255.0f;
                        float a = (cattrs & CELL_DIM) ? (2.0f / 3.0f) : 1.0f;
                        int passes = (cattrs & CELL_BOLD) ? 2 : 1;
                        for (int pass = 0; pass < passes; pass++) {
                            if (cattrs & CELL_ITALIC) {
                                float st = (float)((g_baseline - b->yoff) / 3);
                                float sb = (float)((g_baseline - (b->yoff + gh - 1)) / 3);
                                push_rect_shear(gx + pass, gy, (float)gw, (float)gh,
                                                st, sb, u0, v0, u1, v1, fr, fgc, fb, a);
                            } else {
                                push_rect(gx + pass, gy, (float)gw, (float)gh,
                                          u0, v0, u1, v1, fr, fgc, fb, a);
                            }
                        }
                    }
                }
            }

            // OSC 8 hyperlinks render underlined, like the CPU renderer.
            int link_underline = (c.link != 0 && c.char_code != ' ');
            if (!(cattrs & CELL_TRAIL) && fg != bg &&
                ((cattrs & (CELL_UNDERLINE | CELL_STRIKE)) || link_underline)) {
                int t = (g_cell_height + 8) / 12;
                if (t < 1) t = 1;
                float r = ((fg >> 16) & 0xFF) / 255.0f;
                float g2 = ((fg >> 8) & 0xFF) / 255.0f;
                float b2 = (fg & 0xFF) / 255.0f;
                if ((cattrs & CELL_UNDERLINE) || link_underline) {
                    int uy = g_baseline + 1;
                    if (uy + t > g_cell_height) uy = g_cell_height - t;
                    push_rect(start_x, start_y + uy, (float)cell_w, (float)t,
                              0, 0, 0, 0, r, g2, b2, 1.0f);
                }
                if (cattrs & CELL_STRIKE) {
                    int sy = (g_baseline * 2) / 3;
                    push_rect(start_x, start_y + sy, (float)cell_w, (float)t,
                              0, 0, 0, 0, r, g2, b2, 1.0f);
                }
            }

            if (is_cursor) {
                float cr = ((g_config.cursor_color >> 16) & 0xFF) / 255.0f;
                float cg = ((g_config.cursor_color >> 8) & 0xFF) / 255.0f;
                float cb = (g_config.cursor_color & 0xFF) / 255.0f;
                if (g_config.cursor_shape == 1) {
                    push_rect(start_x, start_y + g_cell_height - 2, (float)cell_w, 2,
                              0, 0, 0, 0, cr, cg, cb, 1.0f);
                } else if (g_config.cursor_shape == 2) {
                    push_rect(start_x, start_y, 2, (float)g_cell_height,
                              0, 0, 0, 0, cr, cg, cb, 1.0f);
                }
            }
        }
    }

    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    flush_batch();
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void gpu_draw(VTState *state) {
    if (!gpu_ready) return;

    float op = g_config.opacity;
    if (op < 0) op = 0;
    if (op > 1) op = 1;
    float br = ((g_config.bg_color >> 16) & 0xFF) / 255.0f;
    float bgc = ((g_config.bg_color >> 8) & 0xFF) / 255.0f;
    float bb = (g_config.bg_color & 0xFF) / 255.0f;

    glViewport(0, 0, g_width, g_height);
    glDisable(GL_BLEND);
    glClearColor(br * op, bgc * op, bb * op, op);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, g_width, g_height);

    draw_background_image();

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    draw_images(state, 0, -1);
    build_cells(state);
    draw_images(state, 0, 1);

    // Visual bell flash: foreground color fading out (max ~38% overlay)
    extern int64_t g_bell_flash_until;
    extern int64_t g_bell_flash_start;
    if (g_bell_flash_until) {
        int64_t now_ms = bell_now_ms();
        if (now_ms < g_bell_flash_until && g_bell_flash_until > g_bell_flash_start) {
            float t = 1.0f - (float)(now_ms - g_bell_flash_start) /
                              (float)(g_bell_flash_until - g_bell_flash_start);
            float a = (96.0f * t) / 255.0f;
            glUseProgram(prog_cell);
            glUniform2f(u_cell_view, (float)g_width, (float)g_height);
            glBindTexture(GL_TEXTURE_2D, white_tex);
            push_rect(0, 0, (float)g_width, (float)g_height,
                      0, 0, 0, 0,
                      ((g_config.fg_color >> 16) & 0xFF) / 255.0f,
                      ((g_config.fg_color >> 8) & 0xFF) / 255.0f,
                      (g_config.fg_color & 0xFF) / 255.0f, a);
            flush_batch();
            glBindTexture(GL_TEXTURE_2D, atlas_tex);
        }
    }
}

void gpu_present(void) {
    if (!gpu_ready) return;
    // Present unconditionally. Gating on compositor frame callbacks deadlocks
    // against compositors that don't deliver callbacks to background windows.
    eglSwapBuffers(egl_dpy, egl_surf);
}

// Kept as a hook for the main loop (queues are only used while a frame
// callback is outstanding; nothing to pump in the unconditional path).
void gpu_pump(void) {
}

void gpu_resize(int width, int height) {
    if (!gpu_ready) return;
#ifdef _HAS_WAYLAND
    if (wl_eglwin && g_backend && g_backend->get_native_platform &&
        g_backend->get_native_platform() == 1) {
        wl_egl_window_resize(wl_eglwin, width, height, 0, 0);
    }
#endif
    glViewport(0, 0, width, height);
}

// ---------------------------------------------------------------------------
// Init / teardown
// ---------------------------------------------------------------------------

static int egl_platform_of_backend(void) {
    if (g_backend && g_backend->get_native_platform) return g_backend->get_native_platform();
    return 0;
}

int gpu_init(void) {
    if (!g_backend || !g_backend->get_native_display || !g_backend->get_native_window) {
        return -1;
    }
    int platform = egl_platform_of_backend();
    void *native_dpy = g_backend->get_native_display();
    void *native_win = g_backend->get_native_window();
    if (!native_dpy || !native_win) return -1;

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

    if (get_platform_display && platform == 1) {
        egl_dpy = get_platform_display(0x31D8 /* EGL_PLATFORM_WAYLAND_KHR */, native_dpy, NULL);
    } else if (get_platform_display && platform == 2) {
        egl_dpy = get_platform_display(0x31D5 /* EGL_PLATFORM_X11_KHR */, native_dpy, NULL);
    }
    if (egl_dpy == EGL_NO_DISPLAY) {
        egl_dpy = eglGetDisplay((EGLNativeDisplayType)native_dpy);
    }
    if (egl_dpy == EGL_NO_DISPLAY) return -1;

    EGLint major = 0, minor = 0;
    if (!eglInitialize(egl_dpy, &major, &minor)) {
        egl_dpy = EGL_NO_DISPLAY;
        return -1;
    }

    EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };
    EGLint ncfg = 0;
    if (!eglChooseConfig(egl_dpy, cfg_attrs, &egl_cfg, 1, &ncfg) || ncfg < 1) return -1;

    EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return -1;
    egl_ctx = eglCreateContext(egl_dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (egl_ctx == EGL_NO_CONTEXT) return -1;

#ifdef _HAS_WAYLAND
    if (platform == 1) {
        wl_eglwin = wl_egl_window_create((struct wl_surface *)native_win, g_width, g_height);
        if (!wl_eglwin) return -1;
        egl_surf = eglCreateWindowSurface(egl_dpy, egl_cfg, (EGLNativeWindowType)wl_eglwin, NULL);
    }
#endif
#ifdef _HAS_X11
    if (platform == 2) {
        egl_surf = eglCreateWindowSurface(egl_dpy, egl_cfg,
                                          (EGLNativeWindowType)(uintptr_t)native_win, NULL);
    }
#endif
    if (egl_surf == EGL_NO_SURFACE) return -1;

    if (!eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx)) return -1;
    // Presentation is compositor-synchronized on Wayland; don't block the
    // parser on vblank when coalescing a big burst.
    eglSwapInterval(egl_dpy, 0);

    prog_cell = make_program(FS_CELL, &u_cell_view);
    prog_img = make_program(FS_IMG, &u_img_view);
    prog_bg = make_program(FS_BG, &u_bg_view);
    if (!prog_cell || !prog_img || !prog_bg) return -1;

    glUseProgram(prog_bg);
    u_bg_base = glGetUniformLocation(prog_bg, "u_base");
    u_bg_t = glGetUniformLocation(prog_bg, "u_t");
    u_bg_op = glGetUniformLocation(prog_bg, "u_op");

    glGenBuffers(1, &vbo);
    atlas_init();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // The EGL swapchain replaces the SHM one — let the backend release it.
    if (g_backend->gl_begin) g_backend->gl_begin();

    gpu_ready = 1;
    // Let runtime font changes rebuild our atlas.
    render_set_glyph_reset_callback(reset_glyph_atlas);
    return 0;
}

void gpu_cleanup(void) {
    if (!gpu_ready) return;
    gpu_ready = 0;
    render_set_glyph_reset_callback(NULL);
    if (atlas_tex) glDeleteTextures(1, &atlas_tex);
    if (white_tex) glDeleteTextures(1, &white_tex);
    if (bg_tex) glDeleteTextures(1, &bg_tex);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (prog_cell) glDeleteProgram(prog_cell);
    if (prog_img) glDeleteProgram(prog_img);
    if (prog_bg) glDeleteProgram(prog_bg);
    eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_surf != EGL_NO_SURFACE) eglDestroySurface(egl_dpy, egl_surf);
    if (egl_ctx != EGL_NO_CONTEXT) eglDestroyContext(egl_dpy, egl_ctx);
#ifdef _HAS_WAYLAND
    if (wl_eglwin) { wl_egl_window_destroy(wl_eglwin); wl_eglwin = NULL; }
#endif
    if (egl_dpy != EGL_NO_DISPLAY) eglTerminate(egl_dpy);
    if (vbuf) { free(vbuf); vbuf = NULL; vcap = vcount = 0; }
}
