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
                new_cells[y * new_cols + x].fg_color = state->current_fg;
                new_cells[y * new_cols + x].bg_color = state->current_bg;
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
                    new_alt[y * new_cols + x].fg_color = state->current_fg;
                    new_alt[y * new_cols + x].bg_color = state->current_bg;
                }
            }
        }
        my_free(state->alt_cells);
        state->alt_cells = new_alt;
        state->alt_rows = new_rows;
        state->alt_cols = new_cols;
    }
    
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
    state->auto_wrap = 1;
    state->alt_screen_active = 0;
    state->alt_cells = NULL;
    for (int y = 0; y < state->rows; y++) {
        for (int x = 0; x < state->cols; x++) {
            state->cells[(y) * state->cols + (x)].char_code = ' ';
            state->cells[(y) * state->cols + (x)].fg_color = state->current_fg;
            state->cells[(y) * state->cols + (x)].bg_color = state->current_bg;
        }
    }
}

static void scroll_up(VTState *state) {
    // Push the top line to scrollback buffer
    Cell *old_line = my_malloc(state->cols * sizeof(Cell));
    for (int x = 0; x < state->cols; x++) {
        old_line[x] = state->cells[0 * state->cols + x];
    }
    
    int max_sb = g_config.scrollback_lines;
    if (max_sb > MAX_SCROLLBACK) max_sb = MAX_SCROLLBACK;
    if (max_sb <= 0) {
        my_free(old_line);
        return;
    }

    if (state->scrollback_count >= max_sb) {
        my_free(state->scrollback[state->scrollback_head].cells);
    } else {
        state->scrollback_count++;
    }
    state->scrollback[state->scrollback_head].cells = old_line;
    state->scrollback[state->scrollback_head].cols = state->cols;
    state->scrollback_head = (state->scrollback_head + 1) % max_sb;

    for (int y = 1; y < state->rows; y++) {
        for (int x = 0; x < state->cols; x++) {
            state->cells[(y-1) * state->cols + x] = state->cells[y * state->cols + x];
        }
    }
    for (int x = 0; x < state->cols; x++) {
        state->cells[(state->rows-1) * state->cols + x].char_code = ' ';
        state->cells[(state->rows-1) * state->cols + x].fg_color = state->current_fg;
        state->cells[(state->rows-1) * state->cols + x].bg_color = state->current_bg;
    }
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
        state->cursor_y++;
        if (state->cursor_y >= state->rows) {
            state->cursor_y = state->rows - 1;
            scroll_up(state);
        }
    } else if (c == '\b') {
        if (state->cursor_x > 0) state->cursor_x--;
    } else if (c >= 32) {
        if (state->cursor_x >= state->cols) {
            if (state->auto_wrap) {
                state->cursor_x = 0;
                state->cursor_y++;
                if (state->cursor_y >= state->rows) {
                    state->cursor_y = state->rows - 1;
                    scroll_up(state);
                }
            } else {
                state->cursor_x = state->cols - 1;
            }
        }
        state->cells[(state->cursor_y) * state->cols + (state->cursor_x)].char_code = c;
        state->cells[(state->cursor_y) * state->cols + (state->cursor_x)].fg_color = state->current_fg;
        state->cells[(state->cursor_y) * state->cols + (state->cursor_x)].bg_color = state->current_bg;
        state->cursor_x++;
    }
}

// Minimal implementation of SGR color
static void handle_csi_m(VTState *state) {
    if (state->num_params == 0) {
        state->current_fg = g_config.fg_color;
        state->current_bg = g_config.bg_color;
        return;
    }
    for (int i = 0; i < state->num_params; i++) {
        int p = state->params[i];
        if (p == 0) {
            state->current_fg = g_config.fg_color;
            state->current_bg = g_config.bg_color;
        } else if (p >= 30 && p <= 37) {
            state->current_fg = g_config.colors[p - 30];
        } else if (p >= 40 && p <= 47) {
            state->current_bg = g_config.colors[p - 40];
        } else if (p >= 90 && p <= 97) {
            state->current_fg = g_config.colors[p - 90 + 8];
        } else if (p >= 100 && p <= 107) {
            state->current_bg = g_config.colors[p - 100 + 8];
        } else if (p == 39) {
            state->current_fg = g_config.fg_color;
        } else if (p == 49) {
            state->current_bg = g_config.bg_color;
        } else if (p == 38 || p == 48) {
            if (i + 2 < state->num_params && state->params[i+1] == 5) {
                // 256 color
                uint32_t c256 = state->params[i+2];
                uint32_t res = 0xFFFFFF;
                if (c256 < 8) {
                    uint32_t colors[] = {0x000000, 0xCC0000, 0x4E9A06, 0xC4A000, 0x3465A4, 0x75507B, 0x06989A, 0xD3D7CF};
                    res = colors[c256];
                } else if (c256 < 16) {
                    uint32_t colors[] = {0x555555, 0xEF2929, 0x8AE234, 0xFCE94F, 0x729FCF, 0xAD7FA8, 0x34E2E2, 0xEEEEEE};
                    res = colors[c256 - 8];
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
                if (p == 38) state->current_fg = res;
                else state->current_bg = res;
                i += 2;
            } else if (i + 4 < state->num_params && state->params[i+1] == 2) {
                // TrueColor
                uint32_t r = state->params[i+2] & 0xFF;
                uint32_t g = state->params[i+3] & 0xFF;
                uint32_t b = state->params[i+4] & 0xFF;
                uint32_t res = (r << 16) | (g << 8) | b;
                if (p == 38) state->current_fg = res;
                else state->current_bg = res;
                i += 4;
            }
        }
    }
}

static void handle_csi(VTState *state, char c) {
    if (c == 'm') {
        handle_csi_m(state);
    } else if (c == 'H' || c == 'f') {
        int row = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        int col = (state->num_params > 1 && state->params[1] > 0) ? state->params[1] : 1;
        if (row > state->rows) row = state->rows;
        if (col > state->cols) col = state->cols;
        state->cursor_y = row - 1;
        state->cursor_x = col - 1;
    } else if (c == 'J') {
        int mode = (state->num_params > 0) ? state->params[0] : 0;
        if (mode == 0) {
            for (int x = state->cursor_x; x < state->cols; x++) {
                state->cells[state->cursor_y * state->cols + x].char_code = ' ';
                state->cells[state->cursor_y * state->cols + x].bg_color = state->current_bg;
            }
            for (int y = state->cursor_y + 1; y < state->rows; y++) {
                for (int x = 0; x < state->cols; x++) {
                    state->cells[y * state->cols + x].char_code = ' ';
                    state->cells[y * state->cols + x].bg_color = state->current_bg;
                }
            }
        } else if (mode == 1) {
            for (int y = 0; y < state->cursor_y; y++) {
                for (int x = 0; x < state->cols; x++) {
                    state->cells[y * state->cols + x].char_code = ' ';
                    state->cells[y * state->cols + x].bg_color = state->current_bg;
                }
            }
            for (int x = 0; x <= state->cursor_x && x < state->cols; x++) {
                state->cells[state->cursor_y * state->cols + x].char_code = ' ';
                state->cells[state->cursor_y * state->cols + x].bg_color = state->current_bg;
            }
        } else if (mode == 2) {
            for (int y = 0; y < state->rows; y++) {
                for (int x = 0; x < state->cols; x++) {
                    state->cells[(y) * state->cols + (x)].char_code = ' ';
                    state->cells[(y) * state->cols + (x)].bg_color = state->current_bg;
                }
            }
            state->cursor_x = 0;
            state->cursor_y = 0;
        }
    } else if (c == 'G' || c == '`') {
        int col = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        state->cursor_x = col - 1;
        if (state->cursor_x >= state->cols) state->cursor_x = state->cols - 1;
    } else if (c == 'd') {
        int row = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        state->cursor_y = row - 1;
        if (state->cursor_y >= state->rows) state->cursor_y = state->rows - 1;
    } else if (c == 'n') {
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
        int is_set = (c == 'h');
        for (int i = 0; i < state->num_params; i++) {
            if (state->params[i] == 7) {
                state->auto_wrap = is_set;
            } else if (state->params[i] == 1049 || state->params[i] == 1047) {
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
                            state->cells[y * state->cols + x].bg_color = state->current_bg;
                        }
                    }
                    state->cursor_x = 0;
                    state->cursor_y = 0;
                } else if (!is_set && state->alt_screen_active) {
                    state->alt_screen_active = 0;
                    if (state->alt_rows == state->rows && state->alt_cols == state->cols) {
                        memcpy(state->cells, state->alt_cells, state->rows * state->cols * sizeof(Cell));
                    } else {
                        // Safe copy if dimensions changed, though vt_resize handles this now
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
            }
        }
    } else if (c == 'A') {
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        state->cursor_y -= n;
        if (state->cursor_y < 0) state->cursor_y = 0;
    } else if (c == 'B') {
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        state->cursor_y += n;
        if (state->cursor_y >= state->rows) state->cursor_y = state->rows - 1;
    } else if (c == 'C') {
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        state->cursor_x += n;
        if (state->cursor_x >= state->cols) state->cursor_x = state->cols - 1;
    } else if (c == 'D') {
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        state->cursor_x -= n;
        if (state->cursor_x < 0) state->cursor_x = 0;
    } else if (c == 'K') {
        int mode = (state->num_params > 0) ? state->params[0] : 0;
        int start_x = 0, end_x = state->cols;
        if (mode == 0) { start_x = state->cursor_x; } // cursor to end
        else if (mode == 1) { end_x = state->cursor_x + 1; } // start to cursor
        for (int x = start_x; x < end_x; x++) {
            state->cells[(state->cursor_y) * state->cols + (x)].char_code = ' ';
            state->cells[(state->cursor_y) * state->cols + (x)].bg_color = state->current_bg;
        }
    } else if (c == 'c') {
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
                memset(state->params, 0, sizeof(state->params));
            } else if (c == '_') {
                state->state = STATE_APC;
            } else if (c == '(') {
                state->state = 7;
            } else if (c == ')') {
                state->state = 8;
            } else if (c == ']' || c == 'P' || c == '^') {
                state->state = 4; // STATE_IGNORE_STRING
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
                // Ignore '?' modifier
            } else if (c >= '0' && c <= '9') {
                if (state->num_params == 0) state->num_params = 1;
                state->params[state->num_params - 1] = state->params[state->num_params - 1] * 10 + (c - '0');
            } else if (c == ';') {
                if (state->num_params < 16) {
                    state->num_params++;
                }
            } else if (c >= 0x40 && c <= 0x7E) {
                handle_csi(state, c);
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
