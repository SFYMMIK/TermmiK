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

#ifndef VT_PARSER_H
#define VT_PARSER_H

#include <stdint.h>

#define MAX_SCROLLBACK 10000

// Cell attribute bits (SGR) — rendered by render.c
#define CELL_BOLD      (1u << 0)
#define CELL_DIM       (1u << 1)
#define CELL_ITALIC    (1u << 2)
#define CELL_UNDERLINE (1u << 3)
#define CELL_STRIKE    (1u << 4)
#define CELL_WIDE      (1u << 5)  // first half of a double-width character
#define CELL_TRAIL     (1u << 6)  // second (spacing) half of a wide character

typedef struct {
    uint32_t char_code;
    uint32_t fg_color;
    uint32_t bg_color;
    uint8_t wrapped;
    uint8_t attrs;
} Cell; // 16 bytes — keep it tight: scrollback holds rows*cols of these

typedef struct KittyImage {
    int id;
    int w, h;
    uint32_t *pixels;
    struct KittyImage *next;
} KittyImage;

typedef struct KittyPlacement {
    int image_id;
    int id;
    int cell_x;
    int cell_y;
    int cols, rows;
    int src_x, src_y, src_w, src_h;
    int z_index;
    struct KittyPlacement *next;
} KittyPlacement;

typedef struct {
    int action;
    int format;
    int t;
    int s;
    int v;
    int o;
    char compression;
    int m;
    int id;
    int placement_id;
    int z_index;
    int cols;
    int rows;
    char *chunk_buf;
    int chunk_len;
    int chunk_cap;
    char *payload_buf;
    int payload_len;
    int payload_cap;
    int is_more;
    int kitty_started; // set to 1 after 'G' byte seen in APC, so we accumulate subsequent bytes
} KittyImageState;

typedef struct {
    Cell *cells;
    int rows;
    int cols;

    struct {
        Cell *cells;
        int cols;
    } scrollback[MAX_SCROLLBACK];
    int scrollback_head;
    int scrollback_count;
    int scroll_offset;

    int utf8_state;
    uint32_t utf8_codepoint;
    
    int g0_charset;
    int g1_charset;
    int current_charset;
    
    int auto_wrap;
    int origin_mode;   // DECOM (?6) — CUP/VPA relative to scroll region
    int alt_screen_active;
    Cell *alt_cells;
    int alt_rows;
    int alt_cols;
    int save_x;
    int save_y;
    
    // Scroll region (DECSTBM)
    int scroll_top;
    int scroll_bottom;
    
    // Cursor save/restore (DECSC/DECRC)
    int saved_cursor_x;
    int saved_cursor_y;
    uint32_t saved_fg;
    uint32_t saved_bg;
    uint8_t saved_attrs;

    // Custom tab stops (ESC H sets, CSI g clears, CSI I / CSI Z move)
    unsigned char *tabstops;
    
    // Cursor visibility (?25h/l)
    int cursor_visible;
    
    // Application cursor keys (?1h/l)
    int app_cursor_keys;
    
    // Mouse tracking
    int mouse_tracking_mode;
    int mouse_sgr_mode;
    int alt_scroll; // ?1007 — wheel scroll sends arrows in alt screen

    // Bracketed paste (?2004) and focus reporting (?1004)
    int bracketed_paste;
    int focus_reporting;

    // OSC string accumulator (OSC 0/2 title, 10/11 color queries, 52 clipboard)
    char *osc_buf;
    int osc_len;
    int osc_cap;
    
    // SGR text attributes
    int bold;
    int dim;
    int italic;
    int underline;
    int reverse;
    int strikethrough;
    
    int cursor_x;
    int cursor_y;
    uint32_t current_fg;
    uint32_t current_bg;
    int current_fg_idx;
    
    // Parser state
    int state;
    int params[32];
    uint8_t param_is_sub[32]; // param arrived after ':' (sub-parameter)
    int num_params;
    int params_overflow; // more than 32 params seen — extra digits dropped
    int csi_private;  // tracks '?' prefix (1) or '>','<','=','other intermediates' (2)
    int csi_inter;    // intermediate byte of the current CSI (e.g. '!', '$', ' ', '>')

    // Last printed cell — used by REP (CSI b)
    Cell last_cell;
    int have_last_cell;
    
    int pty_fd;
    
    // Kitty Graphics
    KittyImageState kitty_img;
    KittyImage *kitty_images;
    KittyPlacement *kitty_placements;
} VTState;

void vt_init(VTState *state, int rows, int cols, int pty_fd);
void vt_resize(VTState *state, int new_rows, int new_cols);
void vt_process(VTState *state, const char *buf, int len);
int  vt_char_width(uint32_t cp); // 0 = combining/skipped, 1 = narrow, 2 = wide

#endif
