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

#ifndef VT_PARSER_H
#define VT_PARSER_H

#include <stdint.h>

#define MAX_SCROLLBACK 10000

typedef struct {
    uint32_t char_code;
    uint32_t fg_color;
    uint32_t bg_color;
} Cell;

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
    
    // Cursor visibility (?25h/l)
    int cursor_visible;
    
    // Application cursor keys (?1h/l)
    int app_cursor_keys;
    
    // Mouse tracking
    int mouse_tracking_mode;
    int mouse_sgr_mode;
    
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
    int params[16];
    int num_params;
    int csi_private;  // tracks '?' prefix in CSI sequences
    
    int pty_fd;
} VTState;

void vt_init(VTState *state, int rows, int cols, int pty_fd);
void vt_resize(VTState *state, int new_rows, int new_cols);
void vt_process(VTState *state, const char *buf, int len);

#endif
