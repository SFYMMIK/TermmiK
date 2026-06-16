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
#include "vt_parser.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>

void vt_resize(VTState *state, int new_rows, int new_cols) {
    if (new_rows <= 0 || new_cols <= 0) return;
    
    Cell *new_cells = my_malloc(new_rows * new_cols * sizeof(Cell));
    for (int y = 0; y < new_rows; y++) {
        for (int x = 0; x < new_cols; x++) {
            if (y < state->rows && x < state->cols) {
                new_cells[y * new_cols + x] = state->cells[y * state->cols + x];
            } else {
                new_cells[y * new_cols + x].char_code = ' ';
                new_cells[y * new_cols + x].fg_color = state->reverse ? state->current_bg : state->current_fg;
                new_cells[y * new_cols + x].bg_color = state->reverse ? state->current_fg : state->current_bg;
            }
        }
    }
    
    my_free(state->cells);
    state->cells = new_cells;
    state->rows = new_rows;
    state->cols = new_cols;
    
    if (state->alt_screen_active && state->alt_cells) {
        Cell *new_alt = my_malloc(new_rows * new_cols * sizeof(Cell));
        for (int y = 0; y < new_rows; y++) {
            for (int x = 0; x < new_cols; x++) {
                if (y < state->alt_rows && x < state->alt_cols) {
                    new_alt[y * new_cols + x] = state->alt_cells[y * state->alt_cols + x];
                } else {
                    new_alt[y * new_cols + x].char_code = ' ';
                    new_alt[y * new_cols + x].fg_color = state->reverse ? state->current_bg : state->current_fg;
                    new_alt[y * new_cols + x].bg_color = state->reverse ? state->current_fg : state->current_bg;
                }
            }
        }
        my_free(state->alt_cells);
        state->alt_cells = new_alt;
        state->alt_rows = new_rows;
        state->alt_cols = new_cols;
    }
    
    // Reset scroll region to full screen on resize
    state->scroll_top = 0;
    state->scroll_bottom = new_rows - 1;
    
    if (state->cursor_x >= new_cols) state->cursor_x = new_cols - 1;
    if (state->cursor_y >= new_rows) state->cursor_y = new_rows - 1;
}


// Basic state machine states
enum {
    STATE_NORMAL,
    STATE_ESCAPE,
    STATE_CSI,
    STATE_APC
};

void vt_init(VTState *state, int rows, int cols, int pty_fd) {
    memset(state, 0, sizeof(VTState));
    state->rows = rows;
    state->cols = cols;
    state->pty_fd = pty_fd;
    state->cells = my_malloc(rows * cols * sizeof(Cell));
    state->current_fg = g_config.fg_color;
    state->current_bg = g_config.bg_color;
    state->current_fg_idx = -1;
    state->auto_wrap = 1;
    state->cursor_visible = 1;
    state->alt_screen_active = 0;
    state->alt_cells = NULL;
    state->scroll_top = 0;
    state->scroll_bottom = rows - 1;
    state->app_cursor_keys = 0;
    state->bold = 0;
    state->dim = 0;
    state->italic = 0;
    state->underline = 0;
    state->reverse = 0;
    state->strikethrough = 0;
    for (int y = 0; y < state->rows; y++) {
        for (int x = 0; x < state->cols; x++) {
            state->cells[(y) * state->cols + (x)].char_code = ' ';
            state->cells[(y) * state->cols + (x)].fg_color = state->reverse ? state->current_bg : state->current_fg;
            state->cells[(y) * state->cols + (x)].bg_color = state->reverse ? state->current_fg : state->current_bg;
        }
    }
}

// Scroll up within the scroll region (lines move up, new blank line at bottom of region)
static void scroll_region_up(VTState *state, int n) {
    int top = state->scroll_top;
    int bot = state->scroll_bottom;
    if (top < 0) top = 0;
    if (bot >= state->rows) bot = state->rows - 1;
    if (top >= bot) return;
    if (n <= 0) return;
    if (n > bot - top + 1) n = bot - top + 1;

    // If this is a full-screen scroll (no scroll region set), push to scrollback
    if (top == 0 && bot == state->rows - 1) {
        for (int s = 0; s < n; s++) {
            Cell *old_line = my_malloc(state->cols * sizeof(Cell));
            for (int x = 0; x < state->cols; x++) {
                old_line[x] = state->cells[0 * state->cols + x];
            }
            
            int max_sb = g_config.scrollback_lines;
            if (max_sb > MAX_SCROLLBACK) max_sb = MAX_SCROLLBACK;
            if (max_sb > 0) {
                if (state->scrollback_count >= max_sb) {
                    my_free(state->scrollback[state->scrollback_head].cells);
                } else {
                    state->scrollback_count++;
                }
                state->scrollback[state->scrollback_head].cells = old_line;
                state->scrollback[state->scrollback_head].cols = state->cols;
                state->scrollback_head = (state->scrollback_head + 1) % max_sb;
            } else {
                my_free(old_line);
            }
            
            // Shift lines up by 1
            for (int y = top; y < bot; y++) {
                for (int x = 0; x < state->cols; x++) {
                    state->cells[y * state->cols + x] = state->cells[(y + 1) * state->cols + x];
                }
            }
            // Clear the bottom line
            for (int x = 0; x < state->cols; x++) {
                state->cells[bot * state->cols + x].char_code = ' ';
                state->cells[bot * state->cols + x].fg_color = state->reverse ? state->current_bg : state->current_fg;
                state->cells[bot * state->cols + x].bg_color = state->reverse ? state->current_fg : state->current_bg;
            }
        }
    } else {
        // Scroll region — no scrollback, just shift lines within region
        for (int s = 0; s < n; s++) {
            for (int y = top; y < bot; y++) {
                for (int x = 0; x < state->cols; x++) {
                    state->cells[y * state->cols + x] = state->cells[(y + 1) * state->cols + x];
                }
            }
            for (int x = 0; x < state->cols; x++) {
                state->cells[bot * state->cols + x].char_code = ' ';
                state->cells[bot * state->cols + x].fg_color = state->reverse ? state->current_bg : state->current_fg;
                state->cells[bot * state->cols + x].bg_color = state->reverse ? state->current_fg : state->current_bg;
            }
        }
    }
}

// Scroll down within the scroll region (lines move down, new blank line at top of region)
static void scroll_region_down(VTState *state, int n) {
    int top = state->scroll_top;
    int bot = state->scroll_bottom;
    if (top < 0) top = 0;
    if (bot >= state->rows) bot = state->rows - 1;
    if (top >= bot) return;
    if (n <= 0) return;
    if (n > bot - top + 1) n = bot - top + 1;

    for (int s = 0; s < n; s++) {
        for (int y = bot; y > top; y--) {
            for (int x = 0; x < state->cols; x++) {
                state->cells[y * state->cols + x] = state->cells[(y - 1) * state->cols + x];
            }
        }
        for (int x = 0; x < state->cols; x++) {
            state->cells[top * state->cols + x].char_code = ' ';
            state->cells[top * state->cols + x].fg_color = state->reverse ? state->current_bg : state->current_fg;
            state->cells[top * state->cols + x].bg_color = state->reverse ? state->current_fg : state->current_bg;
        }
    }
}

// Legacy scroll_up for compatibility — scrolls within scroll region by 1
static void scroll_up(VTState *state) {
    scroll_region_up(state, 1);
}

static void put_char(VTState *state, uint32_t c) {
    if (c >= 'a' && c <= 'z') {
        int charset = (state->current_charset == 0) ? state->g0_charset : state->g1_charset;
        if (charset == 1) { // DEC Special Graphics
            uint32_t dec_map[] = {
                0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 
                0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB, 
                0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C, 0x2502, 
                0x2264, 0x2265
            };
            c = dec_map[c - 'a'];
        }
    }

    if (c == '\r') {
        state->cursor_x = 0;
    } else if (c == '\n') {
        if (state->cursor_y == state->scroll_bottom) {
            // At bottom of scroll region — scroll up within region
            scroll_up(state);
        } else if (state->cursor_y < state->rows - 1) {
            state->cursor_y++;
        }
    } else if (c == '\b') {
        if (state->cursor_x > 0) state->cursor_x--;
    } else if (c == '\t') {
        // Tab: advance to next tab stop (every 8 columns)
        int next_tab = (state->cursor_x / 8 + 1) * 8;
        if (next_tab >= state->cols) next_tab = state->cols - 1;
        state->cursor_x = next_tab;
    } else if (c == '\a') {
        // Bell — ignored silently (could add visual bell later)
    } else if (c >= 32) {
        if (state->cursor_x >= state->cols) {
            if (state->auto_wrap) {
                state->cursor_x = 0;
                if (state->cursor_y == state->scroll_bottom) {
                    scroll_up(state);
                } else if (state->cursor_y < state->rows - 1) {
                    state->cursor_y++;
                }
            } else {
                state->cursor_x = state->cols - 1;
            }
        }
        state->cells[(state->cursor_y) * state->cols + (state->cursor_x)].char_code = c;
        state->cells[(state->cursor_y) * state->cols + (state->cursor_x)].fg_color = state->reverse ? state->current_bg : state->current_fg;
        state->cells[(state->cursor_y) * state->cols + (state->cursor_x)].bg_color = state->reverse ? state->current_fg : state->current_bg;
        state->cursor_x++;
    }
}

// SGR color and attribute handling
static void handle_csi_m(VTState *state) {
    if (state->num_params == 0) {
        // Reset all attributes
        state->current_fg = g_config.fg_color;
        state->current_bg = g_config.bg_color;
        state->current_fg_idx = -1;
        state->bold = 0;
        state->dim = 0;
        state->italic = 0;
        state->underline = 0;
        state->reverse = 0;
        state->strikethrough = 0;
        return;
    }
    for (int i = 0; i < state->num_params; i++) {
        int p = state->params[i];
        if (p == 0) {
            // Reset all
            state->current_fg = g_config.fg_color;
            state->current_bg = g_config.bg_color;
            state->current_fg_idx = -1;
            state->bold = 0;
            state->dim = 0;
            state->italic = 0;
            state->underline = 0;
            state->reverse = 0;
            state->strikethrough = 0;
        } else if (p == 1) {
            state->bold = 1;
            if (state->current_fg_idx >= 0 && state->current_fg_idx < 8) {
                state->current_fg = g_config.colors[state->current_fg_idx + 8];
            }
        } else if (p == 2) {
            state->dim = 1;
        } else if (p == 3) {
            state->italic = 1;
        } else if (p == 4) {
            state->underline = 1;
        } else if (p == 7) {
            state->reverse = 1;
        } else if (p == 9) {
            state->strikethrough = 1;
        } else if (p == 22) {
            state->bold = 0;
            state->dim = 0;
            if (state->current_fg_idx >= 0 && state->current_fg_idx < 8) {
                state->current_fg = g_config.colors[state->current_fg_idx];
            }
        } else if (p == 23) {
            state->italic = 0;
        } else if (p == 24) {
            state->underline = 0;
        } else if (p == 27) {
            state->reverse = 0;
        } else if (p == 29) {
            state->strikethrough = 0;
        } else if (p >= 30 && p <= 37) {
            int idx = p - 30;
            state->current_fg_idx = idx;
            // Bold shifts to bright colors (indices 8-15)
            if (state->bold) {
                state->current_fg = g_config.colors[idx + 8];
            } else {
                state->current_fg = g_config.colors[idx];
            }
        } else if (p >= 40 && p <= 47) {
            state->current_bg = g_config.colors[p - 40];
        } else if (p >= 90 && p <= 97) {
            state->current_fg = g_config.colors[p - 90 + 8];
        } else if (p >= 100 && p <= 107) {
            state->current_bg = g_config.colors[p - 100 + 8];
        } else if (p == 39) {
            state->current_fg = g_config.fg_color;
            state->current_fg_idx = -1;
        } else if (p == 49) {
            state->current_bg = g_config.bg_color;
        } else if (p == 38 || p == 48) {
            if (i + 2 < state->num_params && state->params[i+1] == 5) {
                // 256 color
                uint32_t c256 = state->params[i+2];
                uint32_t res = 0xFFFFFF;
                if (c256 < 16) {
                    res = g_config.colors[c256];
                } else if (c256 < 232) {
                    c256 -= 16;
                    int b = c256 % 6;
                    int g = (c256 / 6) % 6;
                    int r = (c256 / 36) % 6;
                    r = r ? r * 40 + 55 : 0;
                    g = g ? g * 40 + 55 : 0;
                    b = b ? b * 40 + 55 : 0;
                    res = (r << 16) | (g << 8) | b;
                } else {
                    int gray = 8 + (c256 - 232) * 10;
                    res = (gray << 16) | (gray << 8) | gray;
                }
                if (p == 38) {
                    state->current_fg = res;
                    state->current_fg_idx = -1;
                } else {
                    state->current_bg = res;
                }
                i += 2;
            } else if (i + 4 < state->num_params && state->params[i+1] == 2) {
                // TrueColor
                uint32_t r = state->params[i+2] & 0xFF;
                uint32_t g = state->params[i+3] & 0xFF;
                uint32_t b = state->params[i+4] & 0xFF;
                uint32_t res = (r << 16) | (g << 8) | b;
                if (p == 38) {
                    state->current_fg = res;
                    state->current_fg_idx = -1;
                } else {
                    state->current_bg = res;
                }
                i += 4;
            }
        }
    }
}

static void handle_csi(VTState *state, char c, int is_private) {
    if (c == 'm') {
        handle_csi_m(state);
    } else if (c == 'H' || c == 'f') {
        // CUP / HVP — Cursor Position
        int row = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        int col = (state->num_params > 1 && state->params[1] > 0) ? state->params[1] : 1;
        if (row > state->rows) row = state->rows;
        if (col > state->cols) col = state->cols;
        state->cursor_y = row - 1;
        state->cursor_x = col - 1;
    } else if (c == 'J') {
        // ED — Erase in Display
        int mode = (state->num_params > 0) ? state->params[0] : 0;
        if (mode == 0) {
            for (int x = state->cursor_x; x < state->cols; x++) {
                state->cells[state->cursor_y * state->cols + x].char_code = ' ';
                state->cells[state->cursor_y * state->cols + x].fg_color = state->reverse ? state->current_bg : state->current_fg;
                state->cells[state->cursor_y * state->cols + x].bg_color = state->reverse ? state->current_fg : state->current_bg;
            }
            for (int y = state->cursor_y + 1; y < state->rows; y++) {
                for (int x = 0; x < state->cols; x++) {
                    state->cells[y * state->cols + x].char_code = ' ';
                    state->cells[y * state->cols + x].fg_color = state->reverse ? state->current_bg : state->current_fg;
                    state->cells[y * state->cols + x].bg_color = state->reverse ? state->current_fg : state->current_bg;
                }
            }
        } else if (mode == 1) {
            for (int y = 0; y < state->cursor_y; y++) {
                for (int x = 0; x < state->cols; x++) {
                    state->cells[y * state->cols + x].char_code = ' ';
                    state->cells[y * state->cols + x].fg_color = state->reverse ? state->current_bg : state->current_fg;
                    state->cells[y * state->cols + x].bg_color = state->reverse ? state->current_fg : state->current_bg;
                }
            }
            for (int x = 0; x <= state->cursor_x && x < state->cols; x++) {
                state->cells[state->cursor_y * state->cols + x].char_code = ' ';
                state->cells[state->cursor_y * state->cols + x].fg_color = state->reverse ? state->current_bg : state->current_fg;
                state->cells[state->cursor_y * state->cols + x].bg_color = state->reverse ? state->current_fg : state->current_bg;
            }
        } else if (mode == 2) {
            for (int y = 0; y < state->rows; y++) {
                for (int x = 0; x < state->cols; x++) {
                    state->cells[(y) * state->cols + (x)].char_code = ' ';
                    state->cells[(y) * state->cols + (x)].fg_color = state->reverse ? state->current_bg : state->current_fg;
                    state->cells[(y) * state->cols + (x)].bg_color = state->reverse ? state->current_fg : state->current_bg;
                }
            }
        } else if (mode == 3) {
            // Erase scrollback
            for (int i = 0; i < MAX_SCROLLBACK; i++) {
                if (state->scrollback[i].cells) {
                    my_free(state->scrollback[i].cells);
                    state->scrollback[i].cells = NULL;
                }
            }
            state->scrollback_count = 0;
            state->scrollback_head = 0;
            state->scroll_offset = 0;
        }
    } else if (c == 'G' || c == '`') {
        // CHA — Cursor Horizontal Absolute
        int col = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        state->cursor_x = col - 1;
        if (state->cursor_x >= state->cols) state->cursor_x = state->cols - 1;
    } else if (c == 'd') {
        // VPA — Vertical Line Position Absolute
        int row = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        state->cursor_y = row - 1;
        if (state->cursor_y >= state->rows) state->cursor_y = state->rows - 1;
    } else if (c == 'n') {
        // DSR — Device Status Report
        int mode = (state->num_params > 0) ? state->params[0] : 0;
        if (mode == 6) {
            char buf[32];
            char row_str[10], col_str[10];
            int r = state->cursor_y + 1, c_x = state->cursor_x + 1;
            int r_idx = 0, c_idx = 0;
            if (r == 0) row_str[r_idx++] = '0';
            while (r > 0) { row_str[r_idx++] = '0' + (r % 10); r /= 10; }
            if (c_x == 0) col_str[c_idx++] = '0';
            while (c_x > 0) { col_str[c_idx++] = '0' + (c_x % 10); c_x /= 10; }
            
            int len = 0;
            buf[len++] = '\033'; buf[len++] = '[';
            for (int i = r_idx - 1; i >= 0; i--) buf[len++] = row_str[i];
            buf[len++] = ';';
            for (int i = c_idx - 1; i >= 0; i--) buf[len++] = col_str[i];
            buf[len++] = 'R';
            
            if (state->pty_fd != -1) {
                extern ssize_t pty_write(int fd, const char *buf, size_t count);
                pty_write(state->pty_fd, buf, len);
            }
        }
    } else if (c == 'h' || c == 'l') {
        // SM/RM — Set/Reset Mode
        int is_set = (c == 'h');
        for (int i = 0; i < state->num_params; i++) {
            if (is_private) {
                // DEC private modes (CSI ? ... h/l)
                switch (state->params[i]) {
                    case 1:
                        // DECCKM — Application Cursor Keys
                        state->app_cursor_keys = is_set;
                        break;
                    case 7:
                        // DECAWM — Auto-wrap mode
                        state->auto_wrap = is_set;
                        break;
                    case 25:
                        // DECTCEM — Cursor visibility
                        state->cursor_visible = is_set;
                        break;
                    case 1000:
                    case 1002:
                    case 1003:
                        state->mouse_tracking_mode = is_set ? state->params[i] : 0;
                        break;
                    case 1006:
                        state->mouse_sgr_mode = is_set;
                        break;
                    case 1049:
                    case 1047:
                        // Alt screen buffer
                        if (is_set && !state->alt_screen_active) {
                            state->alt_screen_active = 1;
                            state->alt_rows = state->rows;
                            state->alt_cols = state->cols;
                            state->alt_cells = my_malloc(state->rows * state->cols * sizeof(Cell));
                            memcpy(state->alt_cells, state->cells, state->rows * state->cols * sizeof(Cell));
                            state->save_x = state->cursor_x;
                            state->save_y = state->cursor_y;
                            for (int y = 0; y < state->rows; y++) {
                                for (int x = 0; x < state->cols; x++) {
                                    state->cells[y * state->cols + x].char_code = ' ';
                                    state->cells[y * state->cols + x].fg_color = state->reverse ? state->current_bg : state->current_fg;
                                    state->cells[y * state->cols + x].bg_color = state->reverse ? state->current_fg : state->current_bg;
                                }
                            }
                            state->cursor_x = 0;
                            state->cursor_y = 0;
                        } else if (!is_set && state->alt_screen_active) {
                            state->alt_screen_active = 0;
                            if (state->alt_rows == state->rows && state->alt_cols == state->cols) {
                                memcpy(state->cells, state->alt_cells, state->rows * state->cols * sizeof(Cell));
                            } else {
                                for (int y = 0; y < state->rows; y++) {
                                    for (int x = 0; x < state->cols; x++) {
                                        if (y < state->alt_rows && x < state->alt_cols) {
                                            state->cells[y * state->cols + x] = state->alt_cells[y * state->alt_cols + x];
                                        }
                                    }
                                }
                            }
                            my_free(state->alt_cells);
                            state->alt_cells = NULL;
                            state->cursor_x = state->save_x;
                            state->cursor_y = state->save_y;
                            if (state->cursor_x >= state->cols) state->cursor_x = state->cols - 1;
                            if (state->cursor_y >= state->rows) state->cursor_y = state->rows - 1;
                        }
                        break;
                    case 1048:
                        // Save/restore cursor (as part of alt screen)
                        if (is_set) {
                            state->saved_cursor_x = state->cursor_x;
                            state->saved_cursor_y = state->cursor_y;
                            state->saved_fg = state->current_fg;
                            state->saved_bg = state->current_bg;
                        } else {
                            state->cursor_x = state->saved_cursor_x;
                            state->cursor_y = state->saved_cursor_y;
                            state->current_fg = state->saved_fg;
                            state->current_bg = state->saved_bg;
                            if (state->cursor_x >= state->cols) state->cursor_x = state->cols - 1;
                            if (state->cursor_y >= state->rows) state->cursor_y = state->rows - 1;
                        }
                        break;
                }
            } else {
                // Standard (non-private) modes
                if (state->params[i] == 7) {
                    state->auto_wrap = is_set;
                }
            }
        }
    } else if (c == 'A') {
        // CUU — Cursor Up
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        state->cursor_y -= n;
        if (state->cursor_y < 0) state->cursor_y = 0;
    } else if (c == 'B') {
        // CUD — Cursor Down
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        state->cursor_y += n;
        if (state->cursor_y >= state->rows) state->cursor_y = state->rows - 1;
    } else if (c == 'C') {
        // CUF — Cursor Forward (Right)
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        state->cursor_x += n;
        if (state->cursor_x >= state->cols) state->cursor_x = state->cols - 1;
    } else if (c == 'D') {
        // CUB — Cursor Back (Left)
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        state->cursor_x -= n;
        if (state->cursor_x < 0) state->cursor_x = 0;
    } else if (c == 'E') {
        // CNL — Cursor Next Line
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        state->cursor_y += n;
        if (state->cursor_y >= state->rows) state->cursor_y = state->rows - 1;
        state->cursor_x = 0;
    } else if (c == 'F') {
        // CPL — Cursor Previous Line
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        state->cursor_y -= n;
        if (state->cursor_y < 0) state->cursor_y = 0;
        state->cursor_x = 0;
    } else if (c == 'K') {
        // EL — Erase in Line
        int mode = (state->num_params > 0) ? state->params[0] : 0;
        int start_x = 0, end_x = state->cols;
        if (mode == 0) { start_x = state->cursor_x; } // cursor to end
        else if (mode == 1) { end_x = state->cursor_x + 1; } // start to cursor
        // mode == 2: entire line (start_x=0, end_x=cols)
        for (int x = start_x; x < end_x; x++) {
            state->cells[(state->cursor_y) * state->cols + (x)].char_code = ' ';
            state->cells[(state->cursor_y) * state->cols + (x)].fg_color = state->reverse ? state->current_bg : state->current_fg;
            state->cells[(state->cursor_y) * state->cols + (x)].bg_color = state->reverse ? state->current_fg : state->current_bg;
        }
    } else if (c == 'L') {
        // IL — Insert Lines: insert N blank lines at cursor row, pushing existing lines down
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        int top = state->cursor_y;
        int bot = state->scroll_bottom;
        if (top < state->scroll_top) top = state->scroll_top;
        if (n > bot - top + 1) n = bot - top + 1;
        // Shift lines down from bottom of region
        for (int y = bot; y >= top + n; y--) {
            for (int x = 0; x < state->cols; x++) {
                state->cells[y * state->cols + x] = state->cells[(y - n) * state->cols + x];
            }
        }
        // Clear inserted lines
        for (int y = top; y < top + n && y <= bot; y++) {
            for (int x = 0; x < state->cols; x++) {
                state->cells[y * state->cols + x].char_code = ' ';
                state->cells[y * state->cols + x].fg_color = state->reverse ? state->current_bg : state->current_fg;
                state->cells[y * state->cols + x].bg_color = state->reverse ? state->current_fg : state->current_bg;
            }
        }
    } else if (c == 'M') {
        // DL — Delete Lines: delete N lines at cursor row, pulling lines up
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        int top = state->cursor_y;
        int bot = state->scroll_bottom;
        if (top < state->scroll_top) top = state->scroll_top;
        if (n > bot - top + 1) n = bot - top + 1;
        // Shift lines up
        for (int y = top; y <= bot - n; y++) {
            for (int x = 0; x < state->cols; x++) {
                state->cells[y * state->cols + x] = state->cells[(y + n) * state->cols + x];
            }
        }
        // Clear vacated lines at bottom of region
        for (int y = bot - n + 1; y <= bot; y++) {
            for (int x = 0; x < state->cols; x++) {
                state->cells[y * state->cols + x].char_code = ' ';
                state->cells[y * state->cols + x].fg_color = state->reverse ? state->current_bg : state->current_fg;
                state->cells[y * state->cols + x].bg_color = state->reverse ? state->current_fg : state->current_bg;
            }
        }
    } else if (c == 'P') {
        // DCH — Delete Characters: delete N chars at cursor, shift rest left
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        int row = state->cursor_y;
        int col = state->cursor_x;
        if (n > state->cols - col) n = state->cols - col;
        // Shift characters left
        for (int x = col; x < state->cols - n; x++) {
            state->cells[row * state->cols + x] = state->cells[row * state->cols + x + n];
        }
        // Clear vacated characters at end of line
        for (int x = state->cols - n; x < state->cols; x++) {
            state->cells[row * state->cols + x].char_code = ' ';
            state->cells[row * state->cols + x].fg_color = state->reverse ? state->current_bg : state->current_fg;
            state->cells[row * state->cols + x].bg_color = state->reverse ? state->current_fg : state->current_bg;
        }
    } else if (c == '@') {
        // ICH — Insert Characters: insert N blank chars at cursor, shift rest right
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        int row = state->cursor_y;
        int col = state->cursor_x;
        if (n > state->cols - col) n = state->cols - col;
        // Shift characters right
        for (int x = state->cols - 1; x >= col + n; x--) {
            state->cells[row * state->cols + x] = state->cells[row * state->cols + x - n];
        }
        // Clear inserted characters
        for (int x = col; x < col + n && x < state->cols; x++) {
            state->cells[row * state->cols + x].char_code = ' ';
            state->cells[row * state->cols + x].fg_color = state->reverse ? state->current_bg : state->current_fg;
            state->cells[row * state->cols + x].bg_color = state->reverse ? state->current_fg : state->current_bg;
        }
    } else if (c == 'X') {
        // ECH — Erase Characters: erase N chars at cursor (without moving cursor)
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        int row = state->cursor_y;
        for (int x = state->cursor_x; x < state->cursor_x + n && x < state->cols; x++) {
            state->cells[row * state->cols + x].char_code = ' ';
            state->cells[row * state->cols + x].fg_color = state->reverse ? state->current_bg : state->current_fg;
            state->cells[row * state->cols + x].bg_color = state->reverse ? state->current_fg : state->current_bg;
        }
    } else if (c == 'S') {
        // SU — Scroll Up: scroll up N lines within scroll region
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        scroll_region_up(state, n);
    } else if (c == 'T') {
        // SD — Scroll Down: scroll down N lines within scroll region
        if (!is_private) {
            int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
            scroll_region_down(state, n);
        }
    } else if (c == 'r') {
        // DECSTBM — Set Scrolling Region
        if (!is_private) {
            int top = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
            int bot = (state->num_params > 1 && state->params[1] > 0) ? state->params[1] : state->rows;
            if (top < 1) top = 1;
            if (bot > state->rows) bot = state->rows;
            if (top < bot) {
                state->scroll_top = top - 1;
                state->scroll_bottom = bot - 1;
            } else {
                // Invalid range, reset to full screen
                state->scroll_top = 0;
                state->scroll_bottom = state->rows - 1;
            }
            // DECSTBM also moves cursor to home position
            state->cursor_x = 0;
            state->cursor_y = 0;
        }
    } else if (c == 's') {
        // SCP — Save Cursor Position (ANSI)
        if (!is_private) {
            state->saved_cursor_x = state->cursor_x;
            state->saved_cursor_y = state->cursor_y;
            state->saved_fg = state->current_fg;
            state->saved_bg = state->current_bg;
        }
    } else if (c == 'u') {
        // RCP — Restore Cursor Position (ANSI)
        if (!is_private) {
            state->cursor_x = state->saved_cursor_x;
            state->cursor_y = state->saved_cursor_y;
            state->current_fg = state->saved_fg;
            state->current_bg = state->saved_bg;
            if (state->cursor_x >= state->cols) state->cursor_x = state->cols - 1;
            if (state->cursor_y >= state->rows) state->cursor_y = state->rows - 1;
        }
    } else if (c == 'c') {
        // DA — Device Attributes
        extern int g_pty_fd;
        extern ssize_t pty_write(int fd, const char *buf, size_t count);
        if (g_pty_fd != -1) {
            const char *resp = "\033[?6c";
            pty_write(g_pty_fd, resp, strlen(resp));
        }
    }
    state->state = STATE_NORMAL;
}

// Unused APC buffer removed

void vt_process(VTState *state, const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char c = buf[i];
        
        if (state->state == STATE_NORMAL) {
            if (c == 0x1B) {
                state->state = STATE_ESCAPE;
                state->utf8_state = 0;
            } else if (c == 0x0E) {
                state->current_charset = 1;
                state->utf8_state = 0;
            } else if (c == 0x0F) {
                state->current_charset = 0;
                state->utf8_state = 0;
            } else if (state->utf8_state > 0) {
                state->utf8_codepoint = (state->utf8_codepoint << 6) | (c & 0x3F);
                state->utf8_state--;
                if (state->utf8_state == 0) {
                    put_char(state, state->utf8_codepoint);
                }
            } else if (c < 128) {
                put_char(state, c);
            } else if ((c & 0xE0) == 0xC0) {
                state->utf8_state = 1;
                state->utf8_codepoint = c & 0x1F;
            } else if ((c & 0xF0) == 0xE0) {
                state->utf8_state = 2;
                state->utf8_codepoint = c & 0x0F;
            } else if ((c & 0xF8) == 0xF0) {
                state->utf8_state = 3;
                state->utf8_codepoint = c & 0x07;
            }
        } else if (state->state == STATE_ESCAPE) {
            if (c == '[') {
                state->state = STATE_CSI;
                state->num_params = 0;
                state->csi_private = 0;
                memset(state->params, 0, sizeof(state->params));
            } else if (c == '_') {
                state->state = STATE_APC;
            } else if (c == '(') {
                state->state = 7;
            } else if (c == ')') {
                state->state = 8;
            } else if (c == ']' || c == 'P' || c == '^') {
                state->state = 4; // STATE_IGNORE_STRING
            } else if (c == '7') {
                // DECSC — Save Cursor
                state->saved_cursor_x = state->cursor_x;
                state->saved_cursor_y = state->cursor_y;
                state->saved_fg = state->current_fg;
                state->saved_bg = state->current_bg;
                state->state = STATE_NORMAL;
            } else if (c == '8') {
                // DECRC — Restore Cursor
                state->cursor_x = state->saved_cursor_x;
                state->cursor_y = state->saved_cursor_y;
                state->current_fg = state->saved_fg;
                state->current_bg = state->saved_bg;
                if (state->cursor_x >= state->cols) state->cursor_x = state->cols - 1;
                if (state->cursor_y >= state->rows) state->cursor_y = state->rows - 1;
                state->state = STATE_NORMAL;
            } else if (c == 'D') {
                // IND — Index: move cursor down one line, scroll if at bottom of scroll region
                if (state->cursor_y == state->scroll_bottom) {
                    scroll_region_up(state, 1);
                } else if (state->cursor_y < state->rows - 1) {
                    state->cursor_y++;
                }
                state->state = STATE_NORMAL;
            } else if (c == 'M') {
                // RI — Reverse Index: move cursor up one line, scroll down if at top of scroll region
                if (state->cursor_y == state->scroll_top) {
                    scroll_region_down(state, 1);
                } else if (state->cursor_y > 0) {
                    state->cursor_y--;
                }
                state->state = STATE_NORMAL;
            } else if (c == 'E') {
                // NEL — Next Line: CR + LF, respects scroll region
                state->cursor_x = 0;
                if (state->cursor_y == state->scroll_bottom) {
                    scroll_region_up(state, 1);
                } else if (state->cursor_y < state->rows - 1) {
                    state->cursor_y++;
                }
                state->state = STATE_NORMAL;
            } else if (c == 'c') {
                // RIS — Full Reset
                int rows = state->rows;
                int cols = state->cols;
                int pty_fd = state->pty_fd;
                // Free alt screen if active
                if (state->alt_cells) {
                    my_free(state->alt_cells);
                }
                // Free current cells before vt_init allocates new ones
                my_free(state->cells);
                vt_init(state, rows, cols, pty_fd);
                state->state = STATE_NORMAL;
            } else {
                state->state = STATE_NORMAL;
            }
        } else if (state->state == 4) { // STATE_IGNORE_STRING
            if (c == 0x07) { // BEL
                state->state = STATE_NORMAL;
            } else if (c == 0x1B) { // ESC
                state->state = 5; // STATE_IGNORE_ESC
            }
        } else if (state->state == 5) { // STATE_IGNORE_ESC
            if (c == '\\') {
                state->state = STATE_NORMAL;
            } else {
                state->state = 4; // Back to ignore string
            }
        } else if (state->state == 7) {
            if (c == '0') state->g0_charset = 1;
            else if (c == 'B') state->g0_charset = 0;
            state->state = STATE_NORMAL;
        } else if (state->state == 8) {
            if (c == '0') state->g1_charset = 1;
            else if (c == 'B') state->g1_charset = 0;
            state->state = STATE_NORMAL;
        } else if (state->state == STATE_CSI) {
            if (c == '?') {
                state->csi_private = 1;
            } else if (c == '>' || c == '!' || c == '=') {
                // Other CSI modifiers — track but don't specifically handle
                state->csi_private = 2;
            } else if (c >= '0' && c <= '9') {
                if (state->num_params == 0) state->num_params = 1;
                state->params[state->num_params - 1] = state->params[state->num_params - 1] * 10 + (c - '0');
            } else if (c == ';' || c == ':') {
                if (state->num_params < 16) {
                    state->num_params++;
                }
            } else if (c >= 0x40 && c <= 0x7E) {
                handle_csi(state, c, state->csi_private == 1);
            }
        } else if (state->state == STATE_APC) {
            if (c == 0x07 || c == 0x5C) { // ST or BEL
                state->state = STATE_NORMAL;
            } else if (c == 0x1B) { // ESC
                state->state = 6; // STATE_APC_ESC
            }
        } else if (state->state == 6) { // STATE_APC_ESC
            if (c == '\\') {
                state->state = STATE_NORMAL;
            } else {
                state->state = STATE_APC;
            }
        }
    }
}
