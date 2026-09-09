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

#include "alloc.h"
#include "vt_parser.h"
#include "config.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

// Debug logging to /tmp/kitty_cmd.log, enabled only with TERMMIK_DEBUG=1
static void kitty_log(const char *fmt, ...) {
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("TERMMIK_DEBUG") ? 1 : 0;
    if (!dbg) return;
    FILE *f = fopen("/tmp/kitty_cmd.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

void vt_resize(VTState *state, int new_rows, int new_cols) {
    if (new_rows <= 0 || new_cols <= 0) return;
    if (new_rows == state->rows && new_cols == state->cols) return;

    Cell empty_cell;
    empty_cell.char_code = ' ';
    empty_cell.fg_color = state->reverse ? state->current_bg : state->current_fg;
    empty_cell.bg_color = state->reverse ? state->current_fg : state->current_bg;
    empty_cell.wrapped = 0;
    empty_cell.attrs = 0;

    typedef struct {
        Cell *cells;
        int len;
        int cap;
    } LogLine;
    
    LogLine *lines = NULL;
    int num_lines = 0;
    int lines_cap = 0;
    
    void add_logline() {
        if (num_lines >= lines_cap) {
            lines_cap = lines_cap == 0 ? 64 : lines_cap * 2;
            lines = my_realloc(lines, lines_cap * sizeof(LogLine));
        }
        lines[num_lines].cells = NULL;
        lines[num_lines].len = 0;
        lines[num_lines].cap = 0;
        num_lines++;
    }
    
    void append_cell(Cell c) {
        if (num_lines == 0) add_logline();
        LogLine *ll = &lines[num_lines - 1];
        if (ll->len >= ll->cap) {
            ll->cap = ll->cap == 0 ? 128 : ll->cap * 2;
            ll->cells = my_realloc(ll->cells, ll->cap * sizeof(Cell));
        }
        ll->cells[ll->len++] = c;
    }

    Cell *primary_cells = state->alt_screen_active ? state->alt_cells : state->cells;
    int primary_rows = state->alt_screen_active ? state->alt_rows : state->rows;
    int primary_cols = state->alt_screen_active ? state->alt_cols : state->cols;
    int primary_cur_x = state->alt_screen_active ? state->save_x : state->cursor_x;
    int primary_cur_y = state->alt_screen_active ? state->save_y : state->cursor_y;
    
    int new_primary_cur_x = 0;
    int new_primary_cur_y = 0;
    
    for (int i = 0; i < state->scrollback_count; i++) {
        int idx = (state->scrollback_head - state->scrollback_count + i + MAX_SCROLLBACK) % MAX_SCROLLBACK;
        int sb_cols = state->scrollback[idx].cols;
        Cell *sb_cells = state->scrollback[idx].cells;
        
        int is_wrapped = sb_cells[sb_cols - 1].wrapped;
        
        int copy_len = sb_cols;
        if (!is_wrapped) {
            while (copy_len > 0 && sb_cells[copy_len - 1].char_code == ' ' && sb_cells[copy_len - 1].bg_color == g_config.bg_color) copy_len--;
        }
        
        for (int c = 0; c < copy_len; c++) append_cell(sb_cells[c]);
        if (!is_wrapped) add_logline();
    }
    
    for (int y = 0; y < primary_rows; y++) {
        int is_wrapped = primary_cells[y * primary_cols + primary_cols - 1].wrapped;
        
        int copy_len = primary_cols;
        if (!is_wrapped && y != primary_cur_y) {
            while (copy_len > 0 && primary_cells[y * primary_cols + copy_len - 1].char_code == ' ' && primary_cells[y * primary_cols + copy_len - 1].bg_color == g_config.bg_color) copy_len--;
        }
        
        for (int c = 0; c < copy_len; c++) {
            if (y == primary_cur_y && c == primary_cur_x) {
                new_primary_cur_x = num_lines > 0 ? lines[num_lines - 1].len : 0;
            }
            append_cell(primary_cells[y * primary_cols + c]);
        }
        if (y == primary_cur_y && primary_cur_x >= copy_len) {
            new_primary_cur_x = num_lines > 0 ? lines[num_lines - 1].len : 0;
        }
        
        if (y == primary_cur_y) {
            new_primary_cur_y = num_lines > 0 ? num_lines - 1 : 0;
        }

        if (!is_wrapped) add_logline();
    }
    
    typedef struct { Cell *cells; int cols; } PhysLine;
    PhysLine *phys_lines = NULL;
    int num_phys = 0;
    int phys_cap = 0;
    
    int final_cur_x = 0;
    int final_cur_y = 0;
    
    for (int i = 0; i < num_lines; i++) {
        LogLine *ll = &lines[i];
        if (ll->len == 0) {
            if (num_phys >= phys_cap) {
                phys_cap = phys_cap == 0 ? 64 : phys_cap * 2;
                phys_lines = my_realloc(phys_lines, phys_cap * sizeof(PhysLine));
            }
            Cell *row = my_malloc(new_cols * sizeof(Cell));
            if (!row) goto alloc_fail;
            for(int k=0; k<new_cols; k++) row[k] = empty_cell;
            phys_lines[num_phys].cells = row;
            phys_lines[num_phys].cols = new_cols;
            
            if (i == new_primary_cur_y) {
                final_cur_y = num_phys;
                final_cur_x = 0;
            }
            num_phys++;
            continue;
        }
        
        int offset = 0;
        while (offset < ll->len) {
            int chunk = ll->len - offset;
            if (chunk > new_cols) chunk = new_cols;
            
            if (num_phys >= phys_cap) {
                phys_cap = phys_cap == 0 ? 64 : phys_cap * 2;
                phys_lines = my_realloc(phys_lines, phys_cap * sizeof(PhysLine));
            }
            
            Cell *row = my_malloc(new_cols * sizeof(Cell));
            if (!row) goto alloc_fail;
            for(int k=0; k<new_cols; k++) {
                if (k < chunk) {
                    row[k] = ll->cells[offset + k];
                    row[k].wrapped = 0;
                } else {
                    row[k] = empty_cell;
                }
            }
            if (offset + chunk < ll->len) {
                row[new_cols - 1].wrapped = 1;
            }
            
            phys_lines[num_phys].cells = row;
            phys_lines[num_phys].cols = new_cols;
            
            if (i == new_primary_cur_y) {
                if (new_primary_cur_x >= offset && new_primary_cur_x < offset + chunk) {
                    final_cur_y = num_phys;
                    final_cur_x = new_primary_cur_x - offset;
                } else if (new_primary_cur_x == offset + chunk && chunk < new_cols) {
                    final_cur_y = num_phys;
                    final_cur_x = chunk;
                }
            }
            
            offset += chunk;
            num_phys++;
        }
    }
    
    for(int i=0; i<num_lines; i++) {
        if(lines[i].cells) my_free(lines[i].cells);
    }
    if (lines) { my_free(lines); lines = NULL; }

    int new_primary_start = num_phys - new_rows;
    if (new_primary_start < 0) new_primary_start = 0;
    
    if (final_cur_y < new_primary_start) {
        new_primary_start = final_cur_y;
    } else if (final_cur_y >= new_primary_start + new_rows) {
        new_primary_start = final_cur_y - new_rows + 1;
    }
    
    for (int i = 0; i < state->scrollback_count; i++) {
        int idx = (state->scrollback_head - state->scrollback_count + i + MAX_SCROLLBACK) % MAX_SCROLLBACK;
        my_free(state->scrollback[idx].cells);
    }
    state->scrollback_count = 0;
    state->scrollback_head = 0;
    
    // Preserve tab stops across the resize
    unsigned char *old_tabs = state->tabstops;
    int old_cols_for_tabs = state->cols;
    
    int max_sb = g_config.scrollback_lines;
    if (max_sb > MAX_SCROLLBACK) max_sb = MAX_SCROLLBACK;
    
    for (int i = 0; i < new_primary_start; i++) {
        if (max_sb > 0) {
            if (state->scrollback_count >= max_sb) {
                my_free(state->scrollback[state->scrollback_head].cells);
                state->scrollback_count--;
            }
            state->scrollback[state->scrollback_head].cells = phys_lines[i].cells;
            state->scrollback[state->scrollback_head].cols = phys_lines[i].cols;
            state->scrollback_head = (state->scrollback_head + 1) % max_sb;
            state->scrollback_count++;
        } else {
            my_free(phys_lines[i].cells);
        }
    }
    
    Cell *new_primary = my_malloc(new_rows * new_cols * sizeof(Cell));
    if (!new_primary) goto alloc_fail;
    for (int y = 0; y < new_rows; y++) {
        int py = new_primary_start + y;
        for (int x = 0; x < new_cols; x++) {
            if (py < num_phys) {
                new_primary[y * new_cols + x] = phys_lines[py].cells[x];
            } else {
                new_primary[y * new_cols + x] = empty_cell;
            }
        }
        if (py < num_phys) { my_free(phys_lines[py].cells); phys_lines[py].cells = NULL; }
    }
    
    if (phys_lines) { my_free(phys_lines); phys_lines = NULL; }
    
    int act_cur_y = final_cur_y - new_primary_start;
    if (act_cur_y < 0) act_cur_y = 0;
    if (act_cur_y >= new_rows) act_cur_y = new_rows - 1;
    
    Cell *new_alt = NULL;
    if (state->alt_screen_active || state->alt_cells) {
        Cell *old_alt = state->alt_screen_active ? state->cells : state->alt_cells;
        int old_alt_rows = state->alt_screen_active ? state->rows : state->alt_rows;
        int old_alt_cols = state->alt_screen_active ? state->cols : state->alt_cols;
        
        new_alt = my_malloc(new_rows * new_cols * sizeof(Cell));
        if (!new_alt) {
            my_free(new_primary);
            goto alloc_fail;
        }
        for (int y = 0; y < new_rows; y++) {
            for (int x = 0; x < new_cols; x++) {
                if (y < old_alt_rows && x < old_alt_cols) {
                    new_alt[y * new_cols + x] = old_alt[y * old_alt_cols + x];
                } else {
                    new_alt[y * new_cols + x] = empty_cell;
                }
            }
        }
    }
    
    if (state->alt_screen_active) {
        my_free(state->cells);
        if (state->alt_cells) my_free(state->alt_cells);
        
        state->cells = new_alt;
        state->alt_cells = new_primary;
        
        state->save_x = final_cur_x;
        state->save_y = act_cur_y;
        
        if (state->cursor_x >= new_cols) state->cursor_x = new_cols - 1;
        if (state->cursor_y >= new_rows) state->cursor_y = new_rows - 1;
    } else {
        my_free(state->cells);
        if (state->alt_cells) my_free(state->alt_cells);
        
        state->cells = new_primary;
        state->alt_cells = new_alt;
        
        state->cursor_x = final_cur_x;
        state->cursor_y = act_cur_y;
        
        if (state->save_x >= new_cols) state->save_x = new_cols - 1;
        if (state->save_y >= new_rows) state->save_y = new_rows - 1;
    }
    
    state->rows = new_rows;
    state->cols = new_cols;
    state->alt_rows = new_rows;
    state->alt_cols = new_cols;
    
    // Keep tab stops that still fit; new columns default to every 8
    unsigned char *new_tabs = my_malloc(new_cols);
    if (new_tabs) {
        for (int x = 0; x < new_cols; x++) {
            if (x < old_cols_for_tabs) new_tabs[x] = old_tabs[x];
            else new_tabs[x] = (x % 8) == 0;
        }
        my_free(old_tabs);
        state->tabstops = new_tabs;
    } else {
        state->tabstops = NULL;
    }
    
    state->scroll_top = 0;
    state->scroll_bottom = new_rows - 1;
    return;
    
alloc_fail:
    for(int i=0; i<num_lines; i++) {
        if(lines && lines[i].cells) my_free(lines[i].cells);
    }
    if (lines) my_free(lines);
    for(int i=0; i<num_phys; i++) {
        if(phys_lines && phys_lines[i].cells) my_free(phys_lines[i].cells);
    }
    if (phys_lines) my_free(phys_lines);
}



// Basic state machine states
enum {
    STATE_NORMAL,
    STATE_ESCAPE,
    STATE_CSI,
    STATE_APC
};

// A blank cell in the current background (with BCE semantics — reverse is
// baked into the colors, attributes are always cleared).
static Cell blank_cell(VTState *state) {
    Cell c;
    c.char_code = ' ';
    c.fg_color = state->reverse ? state->current_bg : state->current_fg;
    c.bg_color = state->reverse ? state->current_fg : state->current_bg;
    c.wrapped = 0;
    c.attrs = 0;
    return c;
}

// Clear one cell (erase semantics: blank with current bg, no attributes)
static void clear_cell(VTState *state, int y, int x) {
    state->cells[y * state->cols + x] = blank_cell(state);
}

// SGR attribute mask for newly written characters
static uint8_t current_attrs(VTState *state) {
    uint8_t a = 0;
    if (state->bold) a |= CELL_BOLD;
    if (state->dim) a |= CELL_DIM;
    if (state->italic) a |= CELL_ITALIC;
    if (state->underline) a |= CELL_UNDERLINE;
    if (state->strikethrough) a |= CELL_STRIKE;
    return a;
}

void vt_init(VTState *state, int rows, int cols, int pty_fd) {
    // Takes ownership of a possibly-dirty struct (RIS re-entry): callers must
    // have freed prior allocations (see the RIS branch in vt_process) or pass
    // zero-initialized memory.
    memset(state, 0, sizeof(VTState));
    state->rows = rows;
    state->cols = cols;
    state->pty_fd = pty_fd;
    state->cells = my_malloc(rows * cols * sizeof(Cell));
    if (!state->cells) {
        state->rows = 0;
        state->cols = 0;
        return;
    }
    // Tab stops: one byte per column, default every 8 columns
    state->tabstops = my_malloc(cols);
    if (state->tabstops) {
        for (int x = 0; x < cols; x++) state->tabstops[x] = (x % 8) == 0;
    }
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
            state->cells[(y) * state->cols + (x)].bg_color = state->reverse ? state->current_fg : state->current_bg; state->cells[(y) * state->cols + (x)].wrapped = 0;
            state->cells[(y) * state->cols + (x)].attrs = 0;
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

    // If this is a full-screen scroll (no scroll region set), push to scrollback.
    // Never while the alternate screen is active: the alt buffer must not
    // pollute the primary scrollback (vim/less/tmux scrolling).
    if (!state->alt_screen_active && top == 0 && bot == state->rows - 1) {
        for (int s = 0; s < n; s++) {
            Cell *old_line = my_malloc(state->cols * sizeof(Cell));
            if (!old_line) goto skip_scrollback;
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
            skip_scrollback:
            
            // Shift lines up by 1
            for (int y = top; y < bot; y++) {
                for (int x = 0; x < state->cols; x++) {
                    state->cells[y * state->cols + x] = state->cells[(y + 1) * state->cols + x];
                }
            }
            // Clear the bottom line
            for (int x = 0; x < state->cols; x++) {
                clear_cell(state, bot, x);
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
                clear_cell(state, bot, x);
            }
        }
    }
    // Adjust image placements so they track with scrolling text
    for (KittyPlacement *p = state->kitty_placements; p; p = p->next) {
        if (top == 0 && bot == state->rows - 1) {
            // Full screen scroll (with scrollback): all lines from -infinity to bot move up
            if (p->cell_y <= bot) {
                p->cell_y -= n;
            }
        } else {
            // Restricted scroll region: only lines strictly within [top, bot] move
            if (p->cell_y >= top && p->cell_y <= bot) {
                p->cell_y -= n;
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
            clear_cell(state, top, x);
        }
    }
    
    // Adjust kitty image placements so they track with scrolling text
    for (KittyPlacement *p = state->kitty_placements; p; p = p->next) {
        if (p->cell_y >= top && p->cell_y <= bot) {
            p->cell_y += n;
        }
    }
}

// Legacy scroll_up for compatibility — scrolls within scroll region by 1
static void scroll_up(VTState *state) {
    scroll_region_up(state, 1);
}

// ---------------------------------------------------------------------------
// Character width model (compact wcwidth)
// 0  = zero-width / combining — skipped so they don't paint stray cells
// 1  = narrow
// 2  = wide (East Asian Wide/Fullwidth, emoji) — occupies two cells
// ---------------------------------------------------------------------------
typedef struct { uint32_t lo, hi; } CpRange;

static const CpRange wide_ranges[] = {
    {0x1100, 0x115F}, {0x2329, 0x232A}, {0x2E80, 0x303E}, {0x3041, 0x33FF},
    {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xA960, 0xA97F},
    {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F},
    {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6},
    {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A}, {0x1F200, 0x1F320}, {0x1F32D, 0x1F335},
    {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA},
    {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4},
    {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC},
    {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567},
    {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4},
    {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC},
    {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6DF},
    {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB},
    {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F9FF}, {0x1FA70, 0x1FAFF},
    {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

static const CpRange zero_ranges[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0711, 0x0711}, {0x0730, 0x074A},
    {0x07A6, 0x07B0}, {0x07EB, 0x07F3}, {0x0816, 0x0819}, {0x081B, 0x0823},
    {0x0825, 0x0827}, {0x0829, 0x082D}, {0x0859, 0x085B}, {0x08E3, 0x0903},
    {0x093A, 0x093A}, {0x093C, 0x093C}, {0x0941, 0x0948}, {0x094D, 0x094D},
    {0x0951, 0x0957}, {0x0962, 0x0963}, {0x0981, 0x0981}, {0x09BC, 0x09BC},
    {0x09C1, 0x09C4}, {0x09CD, 0x09CD}, {0x09E2, 0x09E3}, {0x0A01, 0x0A02},
    {0x0A3C, 0x0A3C}, {0x0A41, 0x0A42}, {0x0A47, 0x0A48}, {0x0A4B, 0x0A4D},
    {0x0A70, 0x0A71}, {0x0A81, 0x0A82}, {0x0ABC, 0x0ABC}, {0x0AC1, 0x0AC5},
    {0x0AC7, 0x0AC8}, {0x0ACD, 0x0ACD}, {0x0B01, 0x0B01}, {0x0B3C, 0x0B3C},
    {0x0B3F, 0x0B3F}, {0x0B41, 0x0B44}, {0x0B4D, 0x0B4D}, {0x0B82, 0x0B82},
    {0x0BC0, 0x0BC0}, {0x0BCD, 0x0BCD}, {0x0C00, 0x0C00}, {0x0C3E, 0x0C40},
    {0x0C46, 0x0C48}, {0x0C4A, 0x0C4D}, {0x0C81, 0x0C81}, {0x0CBC, 0x0CBC},
    {0x0CBF, 0x0CBF}, {0x0CC6, 0x0CC6}, {0x0CCC, 0x0CCD}, {0x0D01, 0x0D01},
    {0x0D41, 0x0D44}, {0x0D4D, 0x0D4D}, {0x0DCA, 0x0DCA}, {0x0DD2, 0x0DD4},
    {0x0DD6, 0x0DD6}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E},
    {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EB9}, {0x0F35, 0x0F35}, {0x0F37, 0x0F37},
    {0x0F39, 0x0F39}, {0x0F71, 0x0F7E}, {0x0F80, 0x0F84}, {0x0F86, 0x0F87},
    {0x18A9, 0x18A9}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F},
    {0x202A, 0x202E}, {0x2060, 0x2064}, {0x206A, 0x206F}, {0x20D0, 0x20F0},
    {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF},
    {0x1F3FB, 0x1F3FF}, {0xE0100, 0xE01EF},
};

static int cp_in_ranges(uint32_t cp, const CpRange *ranges, int n) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < ranges[mid].lo) hi = mid - 1;
        else if (cp > ranges[mid].hi) lo = mid + 1;
        else return 1;
    }
    return 0;
}

int vt_char_width(uint32_t cp) {
    if (cp < 0x0300) return 1; // ASCII, Latin-1, Greek/Cyrillic area (minus combining handled below)
    if (cp_in_ranges(cp, zero_ranges, (int)(sizeof(zero_ranges) / sizeof(zero_ranges[0]))))
        return 0;
    if (cp_in_ranges(cp, wide_ranges, (int)(sizeof(wide_ranges) / sizeof(wide_ranges[0]))))
        return 2;
    return 1;
}

// Write a printable character at the cursor, handling deferred auto-wrap and
// double-width cells. Records the cell for REP (CSI b).
static void write_cells(VTState *state, uint32_t c, int width) {
    if (state->cursor_x >= state->cols || state->cursor_x + width > state->cols) {
        if (state->auto_wrap) {
            state->cells[(state->cursor_y) * state->cols + (state->cols - 1)].wrapped = 1;
            state->cursor_x = 0;
            if (state->cursor_y == state->scroll_bottom) {
                scroll_up(state);
            } else if (state->cursor_y < state->rows - 1) {
                state->cursor_y++;
            }
        } else {
            state->cursor_x = state->cols - width;
            if (state->cursor_x < 0) state->cursor_x = 0;
        }
    }
    Cell *lead = &state->cells[(state->cursor_y) * state->cols + (state->cursor_x)];
    lead->char_code = c;
    lead->fg_color = state->reverse ? state->current_bg : state->current_fg;
    lead->bg_color = state->reverse ? state->current_fg : state->current_bg;
    lead->wrapped = 0;
    lead->attrs = current_attrs(state) | (width == 2 ? CELL_WIDE : 0);
    if (width == 2 && state->cursor_x + 1 < state->cols) {
        Cell *trail = lead + 1;
        trail->char_code = 0;
        trail->fg_color = lead->fg_color;
        trail->bg_color = lead->bg_color;
        trail->wrapped = 0;
        trail->attrs = CELL_TRAIL;
    }
    state->last_cell = *lead;
    state->have_last_cell = 1;
    state->cursor_x += width;
}

static void put_char(VTState *state, uint32_t c) {
    if (c >= '`' && c <= '~') {
        int charset = (state->current_charset == 0) ? state->g0_charset : state->g1_charset;
        if (charset == 1) { // DEC Special Graphics
            static const uint32_t dec_map[32] = {
                0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, // ` a b c d e f g
                0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, // h i j k l m n o
                0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C, // p q r s t u v w
                0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7, 0x00F7  // x y z { | } ~
            };
            c = dec_map[c - '`'];
        }
    }

    switch (c) {
        case '\r':
            state->cursor_x = 0;
            return;
        case '\n':
        case '\v': // VT
        case '\f': // FF — treated as line feeds
            if (state->cursor_y == state->scroll_bottom) {
                scroll_up(state);
            } else if (state->cursor_y < state->rows - 1) {
                state->cursor_y++;
            }
            return;
        case '\b':
            if (state->cursor_x > 0) {
                // Step back over the trailing half of a wide character
                if (state->cursor_x - 1 < state->cols &&
                    (state->cells[(state->cursor_y) * state->cols + (state->cursor_x - 1)].attrs & CELL_TRAIL))
                    state->cursor_x--;
                state->cursor_x--;
            }
            return;
        case '\t': {
            // Advance to the next tab stop
            int next = state->cursor_x + 1;
            while (next < state->cols && !(state->tabstops && state->tabstops[next])) next++;
            state->cursor_x = (next < state->cols) ? next : state->cols - 1;
            return;
        }
        case '\a':
            // Bell — flash the screen when visual_bell_duration is configured
            {
                extern void term_bell(void);
                term_bell();
            }
            return;
        case 0x7F:
            return; // DEL — ignored
        default:
            break;
    }

    if (c < 32) return; // other C0 controls are ignored
    if (c == 0x7F) return;

    int width = vt_char_width(c);
    if (width == 0) return; // combining marks: skipped to avoid stray glyphs
    write_cells(state, c, width);
}

// Expand an xterm 256-color index to an RGB value (cube + grayscale)
static uint32_t sgr_expand_256(uint32_t c256) {
    if (c256 < 16) return g_config.colors[c256];
    if (c256 < 232) {
        c256 -= 16;
        int b = c256 % 6;
        int g = (c256 / 6) % 6;
        int r = (c256 / 36) % 6;
        r = r ? r * 40 + 55 : 0;
        g = g ? g * 40 + 55 : 0;
        b = b ? b * 40 + 55 : 0;
        return (r << 16) | (g << 8) | b;
    }
    int gray = 8 + (c256 - 232) * 10;
    return (gray << 16) | (gray << 8) | gray;
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
        // Skip sub-parameters (from ':') that were not consumed by their parent
        if (state->param_is_sub[i]) continue;
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
            if (g_config.bold_brightens_text &&
                state->current_fg_idx >= 0 && state->current_fg_idx < 8) {
                state->current_fg = g_config.colors[state->current_fg_idx + 8];
            }
        } else if (p == 2) {
            state->dim = 1;
        } else if (p == 3) {
            state->italic = 1;
        } else if (p == 4) {
            // Underline; colon form 4:0=off 4:1..5=on (single/double/curly/...)
            if (i + 1 < state->num_params && state->param_is_sub[i + 1]) {
                state->underline = (state->params[i + 1] != 0);
                i++;
            } else {
                state->underline = 1;
            }
        } else if (p == 7) {
            state->reverse = 1;
        } else if (p == 9) {
            state->strikethrough = 1;
        } else if (p == 21) {
            // Double underline — rendered as regular underline
            state->underline = 1;
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
        } else if (p == 39) {
            state->current_fg = g_config.fg_color;
            state->current_fg_idx = -1;
        } else if (p == 49) {
            state->current_bg = g_config.bg_color;
        } else if (p == 58) {
            // Underline color: consume (and ignore) exactly like 38/48
            if (i + 1 < state->num_params && state->param_is_sub[i + 1]) {
                if (state->params[i + 1] == 5 && i + 2 < state->num_params) i += 2;
                else if (state->params[i + 1] == 2) {
                    int n_sub = 0;
                    while (i + 2 + n_sub < state->num_params && state->param_is_sub[i + 2 + n_sub]) n_sub++;
                    i += 1 + n_sub;
                }
            } else if (i + 2 < state->num_params && state->params[i + 1] == 5) {
                i += 2;
            } else if (i + 4 < state->num_params && state->params[i + 1] == 2) {
                i += 4;
            }
        } else if (p == 59) {
            // Reset underline color — nothing to do (not rendered)
        } else if (p >= 30 && p <= 37) {
            int idx = p - 30;
            state->current_fg_idx = idx;
            // Bold shifts to bright colors (indices 8-15)
            if (state->bold && g_config.bold_brightens_text) {
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
        } else if (p == 38 || p == 48) {
            int is_fg = (p == 38);
            if (i + 1 < state->num_params && state->param_is_sub[i + 1]) {
                // Colon form: 38:5:N or 38:2:R:G:B / 38:2:CS:R:G:B / 38:2::R:G:B
                if (state->params[i + 1] == 5 && i + 2 < state->num_params && state->param_is_sub[i + 2]) {
                    state->params[i + 2] = state->params[i + 2] & 0xFF;
                    // fall through to shared 256-color conversion below via semicolon form emulation
                    uint32_t col = state->params[i + 2];
                    uint32_t res = sgr_expand_256(col);
                    if (is_fg) { state->current_fg = res; state->current_fg_idx = -1; }
                    else { state->current_bg = res; }
                    i += 2;
                } else if (state->params[i + 1] == 2) {
                    int n_sub = 0;
                    while (i + 2 + n_sub < state->num_params && state->param_is_sub[i + 2 + n_sub]) n_sub++;
                    uint32_t r, g, b;
                    if (n_sub >= 4) {
                        // 38:2:CS:R:G:B or 38:2::R:G:B (empty colorspace -> 0)
                        r = state->params[i + 3] & 0xFF;
                        g = state->params[i + 4] & 0xFF;
                        b = state->params[i + 5] & 0xFF;
                        i += 1 + n_sub;
                    } else {
                        // 38:2:R:G:B
                        r = state->params[i + 2] & 0xFF;
                        g = (n_sub >= 2) ? state->params[i + 3] & 0xFF : 0;
                        b = (n_sub >= 3) ? state->params[i + 4] & 0xFF : 0;
                        i += 1 + n_sub;
                    }
                    uint32_t res = (r << 16) | (g << 8) | b;
                    if (is_fg) { state->current_fg = res; state->current_fg_idx = -1; }
                    else { state->current_bg = res; }
                }
            } else if (i + 2 < state->num_params && state->params[i+1] == 5) {
                // 256 color
                uint32_t res = sgr_expand_256(state->params[i+2] & 0xFF);
                if (is_fg) {
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
                if (is_fg) {
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

// private_mode: 0 = no prefix, 1 = '?' (DEC private), 2 = '>'/'='/other prefix or intermediate
static void handle_csi(VTState *state, char c, int private_mode) {
    if (c == 'm') {
        handle_csi_m(state);
    } else if (c == 'H' || c == 'f') {
        // CUP / HVP — Cursor Position
        int row = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        int col = (state->num_params > 1 && state->params[1] > 0) ? state->params[1] : 1;
        if (state->origin_mode) {
            // DECOM: positions are relative to the scroll region
            row += state->scroll_top;
            if (row > state->scroll_bottom + 1) row = state->scroll_bottom + 1;
        }
        if (row > state->rows) row = state->rows;
        if (col > state->cols) col = state->cols;
        state->cursor_y = row - 1;
        state->cursor_x = col - 1;
    } else if (c == 'J') {
        // ED — Erase in Display
        int mode = (state->num_params > 0) ? state->params[0] : 0;
        if (mode == 0) {
            for (int x = state->cursor_x; x < state->cols; x++) {
                clear_cell(state, state->cursor_y, x);
            }
            for (int y = state->cursor_y + 1; y < state->rows; y++) {
                for (int x = 0; x < state->cols; x++) {
                    clear_cell(state, y, x);
                }
            }
        } else if (mode == 1) {
            for (int y = 0; y < state->cursor_y; y++) {
                for (int x = 0; x < state->cols; x++) {
                    clear_cell(state, y, x);
                }
            }
            for (int x = 0; x <= state->cursor_x && x < state->cols; x++) {
                clear_cell(state, state->cursor_y, x);
            }
        } else if (mode == 2) {
            for (int y = 0; y < state->rows; y++) {
                for (int x = 0; x < state->cols; x++) {
                    clear_cell(state, y, x);
                }
            }
            while (state->kitty_placements) {
                KittyPlacement *p = state->kitty_placements;
                state->kitty_placements = p->next;
                my_free(p);
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
        if (state->origin_mode) {
            row += state->scroll_top;
            if (row > state->scroll_bottom + 1) row = state->scroll_bottom + 1;
        }
        state->cursor_y = row - 1;
        if (state->cursor_y >= state->rows) state->cursor_y = state->rows - 1;
    } else if (c == 'n') {
        // DSR — Device Status Report
        int mode = (state->num_params > 0) ? state->params[0] : 0;
        if (state->pty_fd != -1) {
            extern ssize_t pty_write(int fd, const char *buf, size_t count);
            char buf[32];
            int len = 0;
            // Clamp the reported column to the screen width: after filling the
            // last column the cursor sits at `cols` (wrap-pending), but apps
            // expect 1..cols.
            int cx = (state->cursor_x >= state->cols) ? state->cols - 1 : state->cursor_x;
            if (mode == 6 && private_mode == 1) {
                // DECXCPR — extended cursor position report
                len = snprintf(buf, sizeof(buf), "\033[?%d;%dR", state->cursor_y + 1, cx + 1);
            } else if (mode == 6) {
                // CPR — cursor position report
                len = snprintf(buf, sizeof(buf), "\033[%d;%dR", state->cursor_y + 1, cx + 1);
            } else if (mode == 5) {
                // DSR-OK — terminal is ready
                len = snprintf(buf, sizeof(buf), "\033[0n");
            }
            if (len > 0) pty_write(state->pty_fd, buf, len);
        }
    } else if (c == 'h' || c == 'l') {
        // SM/RM — Set/Reset Mode
        int is_set = (c == 'h');
        for (int i = 0; i < state->num_params; i++) {
            if (private_mode == 1) {
                // DEC private modes (CSI ? ... h/l)
                switch (state->params[i]) {
                    case 1:
                        // DECCKM — Application Cursor Keys
                        state->app_cursor_keys = is_set;
                        break;
                    case 6:
                        // DECOM — Origin mode: CUP/VPA relative to scroll region
                        state->origin_mode = is_set;
                        state->cursor_x = 0;
                        state->cursor_y = is_set ? state->scroll_top : 0;
                        break;
                    case 7:
                        // DECAWM — Auto-wrap mode
                        state->auto_wrap = is_set;
                        break;
                    case 12:
                        // Cursor blink — accepted, not rendered
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
                    case 1005:
                    case 1015:
                        // UTF-8 / rxvt mouse encodings — not supported, ignored
                        break;
                    case 1006:
                        state->mouse_sgr_mode = is_set;
                        break;
                    case 1004:
                        // Focus reporting
                        state->focus_reporting = is_set;
                        break;
                    case 1007:
                        // Alternate scroll — wheel sends arrows while in alt screen
                        state->alt_scroll = is_set;
                        break;
                    case 2004:
                        // Bracketed paste mode
                        state->bracketed_paste = is_set;
                        break;
                    case 1049:
                    case 1047:
                    case 47:
                        // Alt screen buffer
                        if (is_set && !state->alt_screen_active) {
                            state->alt_screen_active = 1;
                            state->alt_rows = state->rows;
                            state->alt_cols = state->cols;
                            state->alt_cells = my_malloc(state->rows * state->cols * sizeof(Cell));
                            if (!state->alt_cells) {
                                state->alt_screen_active = 0;
                                break;
                            }
                            memcpy(state->alt_cells, state->cells, state->rows * state->cols * sizeof(Cell));
                            state->save_x = state->cursor_x;
                            state->save_y = state->cursor_y;
                            for (int y = 0; y < state->rows; y++) {
                                for (int x = 0; x < state->cols; x++) {
                                    state->cells[y * state->cols + x] = blank_cell(state);
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
                            state->saved_attrs = current_attrs(state);
                        } else {
                            state->cursor_x = state->saved_cursor_x;
                            state->cursor_y = state->saved_cursor_y;
                            state->current_fg = state->saved_fg;
                            state->current_bg = state->saved_bg;
                            // Re-derive the individual SGR flags from the saved mask
                            state->bold = !!(state->saved_attrs & CELL_BOLD);
                            state->dim = !!(state->saved_attrs & CELL_DIM);
                            state->italic = !!(state->saved_attrs & CELL_ITALIC);
                            state->underline = !!(state->saved_attrs & CELL_UNDERLINE);
                            state->strikethrough = !!(state->saved_attrs & CELL_STRIKE);
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
            state->cells[(state->cursor_y) * state->cols + (x)].bg_color = state->reverse ? state->current_fg : state->current_bg; state->cells[(state->cursor_y) * state->cols + (x)].wrapped = 0;
        }
    } else if (c == 'L') {
        // IL — Insert Lines: insert N blank lines at cursor row, pushing existing lines down
        // No effect when the cursor is outside the scroll region (VT510 behavior)
        if (state->cursor_y >= state->scroll_top && state->cursor_y <= state->scroll_bottom) {
            int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
            int top = state->cursor_y;
            int bot = state->scroll_bottom;
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
                    clear_cell(state, y, x);
                }
            }
        }
    } else if (c == 'M') {
        // DL — Delete Lines: delete N lines at cursor row, pulling lines up
        // No effect when the cursor is outside the scroll region
        if (state->cursor_y >= state->scroll_top && state->cursor_y <= state->scroll_bottom) {
            int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
            int top = state->cursor_y;
            int bot = state->scroll_bottom;
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
                    clear_cell(state, y, x);
                }
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
            clear_cell(state, row, x);
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
            clear_cell(state, row, x);
        }
    } else if (c == 'X') {
        // ECH — Erase Characters: erase N chars at cursor (without moving cursor)
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        int row = state->cursor_y;
        for (int x = state->cursor_x; x < state->cursor_x + n && x < state->cols; x++) {
            clear_cell(state, row, x);
        }
    } else if (c == 'b') {
        // REP — Repeat the preceding graphic character N times (ECMA-48)
        // ncurses apps emit this; xterm-256color terminfo advertises `rep`.
        if (private_mode == 0 && state->have_last_cell) {
            int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
            int width = (state->last_cell.attrs & CELL_WIDE) ? 2 : 1;
            for (int i = 0; i < n; i++) {
                write_cells(state, state->last_cell.char_code, width);
            }
        }
    } else if (c == 'I') {
        // CHT — Cursor Horizontal Tab: advance N tab stops
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        for (int i = 0; i < n; i++) {
            int next = state->cursor_x + 1;
            while (next < state->cols && !(state->tabstops && state->tabstops[next])) next++;
            if (next < state->cols) state->cursor_x = next;
            else { state->cursor_x = state->cols - 1; break; }
        }
    } else if (c == 'Z') {
        // CBT — Cursor Backwards Tab: move back N tab stops
        int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
        for (int i = 0; i < n; i++) {
            int prev = state->cursor_x - 1;
            while (prev > 0 && !(state->tabstops && state->tabstops[prev])) prev--;
            state->cursor_x = (prev > 0) ? prev : 0;
        }
    } else if (c == 'g') {
        // TBC — Tab Clear
        int mode = (state->num_params > 0) ? state->params[0] : 0;
        if (state->tabstops) {
            if (mode == 0) {
                if (state->cursor_x < state->cols) state->tabstops[state->cursor_x] = 0;
            } else if (mode == 3) {
                memset(state->tabstops, 0, state->cols);
            }
        }
    } else if (c == 'S') {
        // SU — Scroll Up: scroll up N lines within scroll region
        if (private_mode == 0) {
            int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
            scroll_region_up(state, n);
        }
    } else if (c == 'T') {
        // SD — Scroll Down: scroll down N lines within scroll region
        if (private_mode == 0) {
            int n = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
            scroll_region_down(state, n);
        }
    } else if (c == 'r') {
        // DECSTBM — Set Scrolling Region
        if (private_mode == 0) {
            int top = (state->num_params > 0 && state->params[0] > 0) ? state->params[0] : 1;
            int bot = (state->num_params > 1 && state->params[1] > 0) ? state->params[1] : state->rows;
            if (top < 1) top = 1;
            if (bot > state->rows) bot = state->rows;
            if (top <= bot) {
                state->scroll_top = top - 1;
                state->scroll_bottom = bot - 1;
            } else {
                // Invalid range, reset to full screen
                state->scroll_top = 0;
                state->scroll_bottom = state->rows - 1;
            }
            // DECSTBM homes the cursor (to the origin when DECOM is on)
            state->cursor_x = 0;
            state->cursor_y = state->origin_mode ? state->scroll_top : 0;
        }
    } else if (c == 's') {
        // SCP — Save Cursor Position (ANSI)
        if (private_mode == 0) {
            state->saved_cursor_x = state->cursor_x;
            state->saved_cursor_y = state->cursor_y;
            state->saved_fg = state->current_fg;
            state->saved_bg = state->current_bg;
            state->saved_attrs = current_attrs(state);
        }
    } else if (c == 'u') {
        // RCP — Restore Cursor Position (ANSI)
        // Note: private_mode != 0 means this is CSI > u / CSI ? u (kitty keyboard
        // protocol push/pop/query) — must NOT touch the cursor.
        if (private_mode == 0) {
            state->cursor_x = state->saved_cursor_x;
            state->cursor_y = state->saved_cursor_y;
            state->current_fg = state->saved_fg;
            state->current_bg = state->saved_bg;
            state->bold = !!(state->saved_attrs & CELL_BOLD);
            state->dim = !!(state->saved_attrs & CELL_DIM);
            state->italic = !!(state->saved_attrs & CELL_ITALIC);
            state->underline = !!(state->saved_attrs & CELL_UNDERLINE);
            state->strikethrough = !!(state->saved_attrs & CELL_STRIKE);
            if (state->cursor_x >= state->cols) state->cursor_x = state->cols - 1;
            if (state->cursor_y >= state->rows) state->cursor_y = state->rows - 1;
        } else if (private_mode == 1 && state->pty_fd != -1) {
            // Kitty keyboard protocol query (CSI ? u): report zero flags —
            // "queried, no kitty keyboard features active".
            extern ssize_t pty_write(int fd, const char *buf, size_t count);
            const char *resp = "\033[?0u";
            pty_write(state->pty_fd, resp, 5);
        }
    } else if (c == 'c') {
        // DA — Device Attributes
        extern int g_pty_fd;
        extern ssize_t pty_write(int fd, const char *buf, size_t count);
        if (g_pty_fd != -1) {
            if (private_mode == 2) {
                // DA2 — report as a modern xterm build so feature gating in
                // vim/tmux picks up SGR mouse & OSC 52 support.
                const char *resp = "\033[>1;279;0c";
                pty_write(g_pty_fd, resp, strlen(resp));
            } else {
                const char *resp = "\033[?62;c";
                pty_write(g_pty_fd, resp, strlen(resp));
            }
        }
    } else if (c == 'p') {
        if (state->csi_inter == '!') {
            // DECSTR — Soft Terminal Reset
            state->current_fg = g_config.fg_color;
            state->current_bg = g_config.bg_color;
            state->current_fg_idx = -1;
            state->bold = 0;
            state->dim = 0;
            state->italic = 0;
            state->underline = 0;
            state->reverse = 0;
            state->strikethrough = 0;
            state->auto_wrap = 1;
            state->origin_mode = 0;
            state->cursor_visible = 1;
            state->app_cursor_keys = 0;
            state->scroll_top = 0;
            state->scroll_bottom = state->rows - 1;
            state->g0_charset = 0;
            state->g1_charset = 0;
            state->current_charset = 0;
            state->cursor_x = 0;
            state->cursor_y = 0;
        } else if (private_mode == 1 && state->csi_inter == '$' && state->pty_fd != -1) {
            // DECRQM — Request Mode (DEC private): CSI ? Pd $ p -> CSI ? Pd ; Ps $ y
            extern ssize_t pty_write(int fd, const char *buf, size_t count);
            int pd = (state->num_params > 0) ? state->params[0] : 0;
            int ps = 0; // 0 = not recognized
            switch (pd) {
                case 1:    ps = state->app_cursor_keys ? 1 : 2; break;
                case 6:    ps = state->origin_mode ? 1 : 2; break;
                case 7:    ps = state->auto_wrap ? 1 : 2; break;
                case 25:   ps = state->cursor_visible ? 1 : 2; break;
                case 47: case 1047: case 1049:
                    ps = state->alt_screen_active ? 1 : 2; break;
                case 1000: ps = (state->mouse_tracking_mode == 1000) ? 1 : 2; break;
                case 1002: ps = (state->mouse_tracking_mode == 1002) ? 1 : 2; break;
                case 1003: ps = (state->mouse_tracking_mode == 1003) ? 1 : 2; break;
                case 1004: ps = state->focus_reporting ? 1 : 2; break;
                case 1006: ps = state->mouse_sgr_mode ? 1 : 2; break;
                case 1007: ps = state->alt_scroll ? 1 : 2; break;
                case 2004: ps = state->bracketed_paste ? 1 : 2; break;
                default:   ps = 0; break;
            }
            char resp[32];
            int len = snprintf(resp, sizeof(resp), "\033[?%d;%d$y", pd, ps);
            if (len > 0) pty_write(state->pty_fd, resp, len);
        }
    } else if (c == 'q') {
        if (state->csi_inter == ' ') {
            // DECSCUSR — Set Cursor Style: 0/1/2 block, 3/4 underline, 5/6 bar
            int style = (state->num_params > 0) ? state->params[0] : 0;
            if (style <= 2) g_config.cursor_shape = 0;
            else if (style <= 4) g_config.cursor_shape = 1;
            else if (style <= 6) g_config.cursor_shape = 2;
        } else if (private_mode == 2 && state->csi_inter == '>' && state->pty_fd != -1) {
            // XTVERSION — report terminal name/version
            int q = (state->num_params > 0) ? state->params[0] : -1;
            if (q == 0) {
                extern ssize_t pty_write(int fd, const char *buf, size_t count);
                const char *resp = "\033P>|TermmiK\033\\";
                pty_write(state->pty_fd, resp, strlen(resp));
            }
        }
    } else if (c == 't') {
        // Window Manipulation
        extern int g_pty_fd;
        extern ssize_t pty_write(int fd, const char *buf, size_t count);
        extern int g_cell_width;
        extern int g_cell_height;
        if (g_pty_fd != -1 && state->num_params > 0) {
            char resp[64];
            if (state->params[0] == 14) {
                snprintf(resp, sizeof(resp), "\033[4;%d;%dt", state->rows * g_cell_height, state->cols * g_cell_width);
                pty_write(g_pty_fd, resp, strlen(resp));
            } else if (state->params[0] == 16) {
                snprintf(resp, sizeof(resp), "\033[6;%d;%dt", g_cell_height, g_cell_width);
                pty_write(g_pty_fd, resp, strlen(resp));
            } else if (state->params[0] == 18) {
                snprintf(resp, sizeof(resp), "\033[8;%d;%dt", state->rows, state->cols);
                pty_write(g_pty_fd, resp, strlen(resp));
            } else if (state->params[0] == 8 && state->num_params >= 3) {
                // XTWINOPS resize request: CSI 8 ; height ; width t
                extern void term_resize(int width, int height);
                int req_rows = state->params[1] > 0 ? state->params[1] : state->rows;
                int req_cols = state->params[2] > 0 ? state->params[2] : state->cols;
                if (req_rows > 1 && req_cols > 1) {
                    term_resize(req_cols * g_cell_width + g_config.padding_left + g_config.padding_right,
                                req_rows * g_cell_height + g_config.padding_top + g_config.padding_bottom);
                }
            }
        }
    }
    state->state = STATE_NORMAL;
}

// Unused APC buffer removed


static const int b64_table[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

static void decode_base64(const char *in, int in_len, unsigned char **out, int *out_len) {
    if (!in || in_len <= 0) { *out = NULL; *out_len = 0; return; }
    int pad = 0;
    if (in_len > 0 && in[in_len-1] == '=') pad++;
    if (in_len > 1 && in[in_len-2] == '=') pad++;
    int req_len = (in_len * 3) / 4 - pad;
    *out = my_malloc(req_len);
    *out_len = req_len;
    if (!*out) { *out_len = 0; return; }
    
    int j = 0;
    for (int i = 0; i < in_len; i += 4) {
        int v0 = b64_table[(unsigned char)in[i]];
        int v1 = (i+1 < in_len) ? b64_table[(unsigned char)in[i+1]] : -1;
        int v2 = (i+2 < in_len) ? b64_table[(unsigned char)in[i+2]] : -1;
        int v3 = (i+3 < in_len) ? b64_table[(unsigned char)in[i+3]] : -1;
        if (v0 == -1 || v1 == -1) break;
        (*out)[j++] = (v0 << 2) | ((v1 >> 4) & 3);
        if (v2 != -1) (*out)[j++] = ((v1 & 15) << 4) | ((v2 >> 2) & 15);
        if (v3 != -1) (*out)[j++] = ((v2 & 3) << 6) | v3;
    }
}

static void parse_kitty_image_command(VTState *state) {
    if (state->kitty_img.chunk_len == 0) return;
    char *cmd = state->kitty_img.chunk_buf;
    int len = state->kitty_img.chunk_len;
    
    // Log every received kitty command for debugging
    kitty_log("KITTY CMD [%d bytes]: %.*s\n", len, len > 200 ? 200 : len, cmd);
    
    // Format: a=T,f=100,...;base64  OR  a=q,i=1  (no semicolon for queries)
    char *semi = memchr(cmd, ';', len);
    int header_len = semi ? (int)(semi - cmd) : len;
    char *payload = semi ? semi + 1 : cmd + len;
    int payload_len = semi ? len - header_len - 1 : 0;
    
    char header[256];
    if (header_len >= sizeof(header)) header_len = sizeof(header) - 1;
    memcpy(header, cmd, header_len);
    header[header_len] = '\0';
    
    // Parse key-value pairs
    char *kv = strtok(header, ",");
    while (kv) {
        char *eq = strchr(kv, '=');
        if (eq) {
            *eq = '\0';
            char *val = eq + 1;
            if (strcmp(kv, "a") == 0) state->kitty_img.action = val[0];
            else if (strcmp(kv, "f") == 0) state->kitty_img.format = atoi(val);
            else if (strcmp(kv, "i") == 0) state->kitty_img.id = atoi(val);
            else if (strcmp(kv, "p") == 0) state->kitty_img.placement_id = atoi(val);
            else if (strcmp(kv, "z") == 0) state->kitty_img.z_index = atoi(val);
            else if (strcmp(kv, "c") == 0) state->kitty_img.cols = atoi(val);
            else if (strcmp(kv, "r") == 0) state->kitty_img.rows = atoi(val);
            else if (strcmp(kv, "m") == 0) state->kitty_img.is_more = atoi(val);
            else if (strcmp(kv, "t") == 0) state->kitty_img.t = val[0];
            else if (strcmp(kv, "q") == 0) state->kitty_img.o = atoi(val);
            else if (strcmp(kv, "o") == 0) state->kitty_img.compression = val[0];
            else if (strcmp(kv, "s") == 0) state->kitty_img.s = atoi(val);
            else if (strcmp(kv, "v") == 0) state->kitty_img.v = atoi(val);
        }
        kv = strtok(NULL, ",");
    }
    
    // Accumulate payload
    if (payload_len > 0) {
        if (state->kitty_img.payload_len + payload_len >= state->kitty_img.payload_cap) {
            state->kitty_img.payload_cap = state->kitty_img.payload_len + payload_len + 1024;
            state->kitty_img.payload_buf = my_realloc(state->kitty_img.payload_buf, state->kitty_img.payload_cap);
        }
        memcpy(state->kitty_img.payload_buf + state->kitty_img.payload_len, payload, payload_len);
        state->kitty_img.payload_len += payload_len;
    }
    
    // If not more (m=0), process the accumulated payload
    if (state->kitty_img.is_more == 0) {
        // Handle action
        if (state->kitty_img.action == 'q') {
            extern int g_pty_fd;
            extern ssize_t pty_write(int fd, const char *buf, size_t count);
            if (g_pty_fd != -1) {
                char resp[128];
                snprintf(resp, sizeof(resp), "\033_Gi=%d;OK\033\\", state->kitty_img.id);
                pty_write(g_pty_fd, resp, strlen(resp));
            }
        } else if (state->kitty_img.action == 'T' || state->kitty_img.action == 'f') {
            unsigned char *decoded = NULL;
            int decoded_len = 0;
            if (state->kitty_img.payload_len > 0) {
                decode_base64(state->kitty_img.payload_buf, state->kitty_img.payload_len, &decoded, &decoded_len);
            }
            
            KittyImage *img = NULL;
            
            // If action is T, we create/decode an image
            if (state->kitty_img.action == 'T') {
                int x, y, n;
                uint32_t *pixels = NULL;
                int w = 0, h = 0;
                
                int transfer = state->kitty_img.t ? state->kitty_img.t : 'd';
                
                if (transfer == 'f' || transfer == 't') {
                    // Payload is a file path — load directly from disk
                    if (decoded && decoded_len > 0) {
                        char path[512];
                        int plen = decoded_len < (int)sizeof(path)-1 ? decoded_len : (int)sizeof(path)-1;
                        memcpy(path, decoded, plen);
                        path[plen] = '\0';
                        
                        kitty_log("LOADING FILE: %s (fmt=%d)\n", path, state->kitty_img.format);
                        
                        unsigned char *data = stbi_load(path, &x, &y, &n, 4);
                        if (data) {
                            w = x; h = y;
                            pixels = my_malloc(w * h * 4);
                            if (pixels) {
                                for (int i = 0; i < w*h; i++) {
                                    uint32_t r = data[i*4+0];
                                    uint32_t g = data[i*4+1];
                                    uint32_t b = data[i*4+2];
                                    uint32_t a = data[i*4+3];
                                    pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
                                }
                            }
                            stbi_image_free(data);
                        } else {
                            kitty_log("stbi_load FAILED: %s\n", stbi_failure_reason());
                        }
                    }
                } else {
                    // Direct data: decoded bytes ARE the image data
                    if (decoded && decoded_len > 0) {
                        // Decompress zlib data if o=z
                        unsigned char *decompressed = NULL;
                        int decompressed_len = 0;
                        if (state->kitty_img.compression == 'z' || state->kitty_img.compression == 122) { // 'z' is 122
                            // We need to implement zlib decompression here
                            // Let's call a zlib decompress function if available.
                            // TermmiK may not link zlib, but wait, stb_image doesn't decompress raw zlib buffers easily unless we use stbi_zlib_decode_malloc.
                            extern char* stbi_zlib_decode_malloc(const char *buffer, int len, int *outlen);
                            decompressed = (unsigned char*)stbi_zlib_decode_malloc((const char*)decoded, decoded_len, &decompressed_len);
                            if (decompressed) {
                                my_free(decoded);
                                decoded = decompressed;
                                decoded_len = decompressed_len;
                            }
                        }
                        
                        if (state->kitty_img.format == 100 || state->kitty_img.format == 0) {
                            unsigned char *data = stbi_load_from_memory(decoded, decoded_len, &x, &y, &n, 4);
                            if (data) {
                                w = x; h = y;
                                pixels = my_malloc(w * h * 4);
                                if (pixels) {
                                    for (int i=0; i<w*h; i++) {
                                        uint32_t r = data[i*4+0];
                                        uint32_t g = data[i*4+1];
                                        uint32_t b = data[i*4+2];
                                        uint32_t a = data[i*4+3];
                                        pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
                                    }
                                }
                                stbi_image_free(data);
                            }
                        } else if (state->kitty_img.format == 24 || state->kitty_img.format == 32) {
                            // Raw RGB/RGBA pixels
                            int channels = (state->kitty_img.format == 32) ? 4 : 3;
                            if (state->kitty_img.s > 0 && state->kitty_img.v > 0) {
                                w = state->kitty_img.s;
                                h = state->kitty_img.v;
                                pixels = my_malloc(w * h * 4);
                                if (pixels) {
                                    for (int i = 0; i < w*h; i++) {
                                        int src = i * channels;
                                        if (src + channels - 1 < decoded_len) {
                                            uint32_t r = decoded[src+0];
                                            uint32_t g = decoded[src+1];
                                            uint32_t b = decoded[src+2];
                                            uint32_t a = (channels == 4) ? decoded[src+3] : 255;
                                            pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                
                if (pixels) {
                    img = my_malloc(sizeof(KittyImage));
                    img->id = state->kitty_img.id;
                    if (img->id == 0) img->id = 1;
                    img->w = w;
                    img->h = h;
                    img->pixels = pixels;
                    img->next = state->kitty_images;
                    state->kitty_images = img;
                    
                    kitty_log("IMAGE LOADED: id=%d, %dx%d\n", img->id, w, h);
                } else {
                    kitty_log("IMAGE LOAD FAILED: action=%c t=%c f=%d payload_len=%d\n",
                        state->kitty_img.action, state->kitty_img.t ? state->kitty_img.t : 'd',
                        state->kitty_img.format, state->kitty_img.payload_len);
                }
            }
            
            // Find the image if action is 'p' or just transmitted
            if (!img) {
                for (KittyImage *i = state->kitty_images; i; i = i->next) {
                    if (i->id == state->kitty_img.id) { img = i; break; }
                }
            }
            
            if (img) {
                KittyPlacement *p = my_malloc(sizeof(KittyPlacement));
                p->image_id = img->id;
                p->id = state->kitty_img.placement_id ? state->kitty_img.placement_id : img->id;
                p->cell_x = state->cursor_x;
                p->cell_y = state->cursor_y;
                
                // Determine display cols/rows from image pixel dimensions
                extern int g_cell_width;
                extern int g_cell_height;
                int pcols = state->kitty_img.cols;
                int prows = state->kitty_img.rows;
                // If not specified, derive from image pixel size
                if (pcols <= 0 && g_cell_width > 0)
                    pcols = (img->w + g_cell_width - 1) / g_cell_width;
                if (prows <= 0 && g_cell_height > 0)
                    prows = (img->h + g_cell_height - 1) / g_cell_height;
                if (pcols <= 0) pcols = 1;
                if (prows <= 0) prows = 1;
                
                p->cols = pcols;
                p->rows = prows;
                p->z_index = state->kitty_img.z_index;
                p->src_x = 0; p->src_y = 0; p->src_w = img->w; p->src_h = img->h;
                p->next = state->kitty_placements;
                state->kitty_placements = p;
                
                // Advance cursor — kitty protocol expects the cursor to advance
                // by the number of rows if c and r are not specified.
                state->cursor_x = 0;
                for (int i = 0; i < prows; i++) {
                    if (state->cursor_y == state->scroll_bottom) {
                        scroll_region_up(state, 1);
                    } else if (state->cursor_y < state->rows - 1) {
                        state->cursor_y++;
                    }
                }
                
                kitty_log("PLACEMENT: cell=(%d,%d) cols=%d rows=%d img=%dx%d\n",
                    p->cell_x, p->cell_y, pcols, prows, img->w, img->h);
            }
            
            if (decoded) my_free(decoded);
        } else if (state->kitty_img.action == 'd') {
            // Delete image or placement
            // Simple clear for now (we could just remove everything to simplify)
            state->kitty_images = NULL;
            state->kitty_placements = NULL;
        }
        
        // Reset state since m=0 signifies end of transfer
        state->kitty_img.action = 0;
        state->kitty_img.format = 0;
        state->kitty_img.cols = 0;
        state->kitty_img.rows = 0;
        state->kitty_img.z_index = 0;
        state->kitty_img.compression = 0;
        state->kitty_img.payload_len = 0;
        // Do NOT free payload_buf, reuse capacity
    }

}

// Parse an X11-style color spec: "rgb:RR/GG/BB", "rgb:RRRR/GGGG/BBBB",
// "#RRGGBB", "rgba:RRRR/GGGG/BBBB/AAAA", or plain hex digits.
// Returns 1 on success.
static int hex_digit(char c, unsigned *out) {
    if (c >= '0' && c <= '9') { *out = (unsigned)(c - '0'); return 1; }
    if (c >= 'a' && c <= 'f') { *out = (unsigned)(c - 'a' + 10); return 1; }
    if (c >= 'A' && c <= 'F') { *out = (unsigned)(c - 'A' + 10); return 1; }
    return 0;
}

static unsigned scale8(unsigned v, int bits) {
    if (bits <= 0 || bits >= 8) return v & 0xFF;
    unsigned max = (1u << bits) - 1;
    return (unsigned)(((float)v / (float)max) * 255.0f + 0.5f) & 0xFF;
}

static int parse_x_color(const char *s, uint32_t *out) {
    if (!s) return 0;
    while (*s == ' ') s++;
    unsigned ch[3];
    int bits[3] = {8, 8, 8};
    if (*s == '#') {
        // #RGB #RRGGBB #RRRGGGBBB #RRRRGGGGBBBB
        s++;
        unsigned d[12];
        int n = 0;
        while (n < 12 && hex_digit(s[n], &d[n])) n++;
        if (n != 3 && n != 6 && n != 9 && n != 12) return 0;
        int per = n / 3;
        for (int k = 0; k < 3; k++) {
            unsigned v = 0;
            for (int j = 0; j < per; j++) v = (v << 4) | d[k * per + j];
            ch[k] = v;
            bits[k] = per * 4;
        }
    } else if ((s[0] | 32) == 'r' && (s[1] | 32) == 'g' && (s[2] | 32) == 'b') {
        s += 3;
        if (*s == 'a' || *s == 'A') s++;
        if (*s != ':') return 0;
        s++;
        for (int k = 0; k < 3; k++) {
            unsigned v = 0, dd = 0;
            int n = 0;
            while (hex_digit(*s, &dd)) { v = v * 16 + dd; n++; s++; }
            if (n < 1 || n > 4) return 0;
            ch[k] = v;
            bits[k] = n * 4;
            if (k < 2) {
                if (*s != '/') return 0;
                s++;
            }
        }
        // Trailing /alpha (rgba) is ignored
    } else {
        // Plain RRGGBB hex
        unsigned v = 0, dd = 0;
        int n = 0;
        while (hex_digit(s[n], &dd)) { v = v * 16 + dd; n++; }
        if (n != 6) return 0;
        ch[0] = (v >> 16) & 0xFF;
        ch[1] = (v >> 8) & 0xFF;
        ch[2] = v & 0xFF;
    }
    *out = (scale8(ch[0], bits[0]) << 16) | (scale8(ch[1], bits[1]) << 8) | scale8(ch[2], bits[2]);
    return 1;
}

// Handle a completed OSC string (terminated by BEL or ST)
static void handle_osc(VTState *state) {
    if (state->osc_len <= 0 || !state->osc_buf) return;
    state->osc_buf[state->osc_len] = '\0';

    // Parse the leading number (OSC code)
    int code = 0;
    int i = 0;
    while (i < state->osc_len && state->osc_buf[i] >= '0' && state->osc_buf[i] <= '9') {
        code = code * 10 + (state->osc_buf[i] - '0');
        i++;
    }
    char *arg = (i < state->osc_len && state->osc_buf[i] == ';') ? state->osc_buf + i + 1 : NULL;

    switch (code) {
        case 0:
        case 1:
        case 2: {
            // Window title (OSC 0 = icon+title, 1 = icon, 2 = title)
            if (arg) {
                extern void term_set_title(const char *title);
                term_set_title(arg);
            }
            break;
        }
        case 4: {
            // OSC 4 — query/set palette entries: 4;idx;spec;idx;spec...
            if (!arg) break;
            char *save = NULL;
            char *tok = strtok_r(arg, ";", &save);
            while (tok) {
                char *next = strtok_r(NULL, ";", &save);
                int idx = atoi(tok);
                if (next) {
                    if (next[0] == '?' && state->pty_fd != -1) {
                        // Query: reply with the current color
                        extern ssize_t pty_write(int fd, const char *buf, size_t count);
                        uint32_t col = (idx >= 0 && idx < 16) ? g_config.colors[idx] : 0;
                        char resp[64];
                        int len = snprintf(resp, sizeof(resp), "\033]4;%d;rgb:%04x/%04x/%04x\033\\",
                                           idx, ((col >> 16) & 0xFF) * 0x101,
                                           ((col >> 8) & 0xFF) * 0x101, (col & 0xFF) * 0x101);
                        if (len > 0) pty_write(state->pty_fd, resp, len);
                    } else {
                        uint32_t col;
                        if (idx >= 0 && idx < 16 && parse_x_color(next, &col)) {
                            g_config.colors[idx] = col;
                        }
                    }
                }
                tok = next;
            }
            break;
        }
        case 10:
        case 11: {
            // Foreground / background color — query or set
            if (arg && arg[0] == '?') {
                if (state->pty_fd != -1) {
                    extern ssize_t pty_write(int fd, const char *buf, size_t count);
                    uint32_t col = (code == 10) ? g_config.fg_color : g_config.bg_color;
                    char resp[64];
                    int len = snprintf(resp, sizeof(resp), "\033]%d;rgb:%04x/%04x/%04x\033\\", code,
                                       ((col >> 16) & 0xFF) * 0x101,
                                       ((col >> 8) & 0xFF) * 0x101,
                                       (col & 0xFF) * 0x101);
                    pty_write(state->pty_fd, resp, len);
                }
            } else if (arg) {
                uint32_t col;
                if (parse_x_color(arg, &col)) {
                    if (code == 10) {
                        g_config.fg_color = col;
                        state->current_fg = col;
                    } else {
                        g_config.bg_color = col;
                        state->current_bg = col;
                        // Re-blend the background image with the new color
                        extern void render_invalidate_background(void);
                        render_invalidate_background();
                    }
                }
            }
            break;
        }
        case 104: {
            // OSC 104 — reset palette entries (all, or specific ones)
            extern void config_reset_palette(int idx);
            if (!arg) {
                for (int k = 0; k < 16; k++) config_reset_palette(k);
            } else {
                char *tok = strtok(arg, ";");
                while (tok) {
                    int idx = atoi(tok);
                    if (idx >= 0 && idx < 16) config_reset_palette(idx);
                    tok = strtok(NULL, ";");
                }
            }
            break;
        }
        case 110:
        case 111: {
            // Reset default fg/bg to config values
            extern void config_reset_default_colors(void);
            config_reset_default_colors();
            state->current_fg = g_config.fg_color;
            state->current_bg = g_config.bg_color;
            extern void render_invalidate_background(void);
            render_invalidate_background();
            break;
        }
        case 52: {
            // Clipboard set: OSC 52;<selection>;<base64 payload>
            // Used by TUI programs (nvim, tmux, opencode...) to copy to the system clipboard.
            if (!arg) break;
            char *data = strchr(arg, ';');
            if (!data) break;
            data++;
            if (*data == '\0' || *data == '?') break; // clipboard read requests are refused
            unsigned char *decoded = NULL;
            int decoded_len = 0;
            decode_base64(data, (int)strlen(data), &decoded, &decoded_len);
            if (decoded && decoded_len > 0) {
                extern void term_set_clipboard(const char *text);
                term_set_clipboard((char *)decoded);
            }
            if (decoded) my_free(decoded);
            break;
        }
        default:
            // OSC 7 (cwd), 8 (hyperlinks), 133 (shell integration) etc. are
            // consumed and ignored.
            break;
    }
}

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
                if ((c & 0xC0) == 0x80) {
                    state->utf8_codepoint = (state->utf8_codepoint << 6) | (c & 0x3F);
                    state->utf8_state--;
                    if (state->utf8_state == 0) {
                        // Validate: reject overlongs, surrogates and out-of-range
                        uint32_t cp = state->utf8_codepoint;
                        int ok = (cp >= 0x80 && cp <= 0x10FFFF &&
                                  !(cp >= 0xD800 && cp <= 0xDFFF));
                        put_char(state, ok ? cp : 0xFFFD);
                    }
                } else {
                    // Invalid continuation — emit replacement and resync on this byte
                    put_char(state, 0xFFFD);
                    state->utf8_state = 0;
                    i--;
                }
            } else if (c < 128) {
                put_char(state, c);
            } else if (c >= 0xC2 && c <= 0xDF) {
                state->utf8_state = 1;
                state->utf8_codepoint = c & 0x1F;
            } else if ((c & 0xF0) == 0xE0) {
                state->utf8_state = 2;
                state->utf8_codepoint = c & 0x0F;
            } else if (c >= 0xF0 && c <= 0xF4) {
                state->utf8_state = 3;
                state->utf8_codepoint = c & 0x07;
            } else {
                // Stray 0x80-0xBF, 0xF5-0xFF — replacement character
                put_char(state, 0xFFFD);
            }
        } else if (state->state == STATE_ESCAPE) {
            if (c == '[') {
                state->state = STATE_CSI;
                state->num_params = 0;
                state->csi_private = 0;
                state->csi_inter = 0;
                state->params_overflow = 0;
                memset(state->params, 0, sizeof(state->params));
                memset(state->param_is_sub, 0, sizeof(state->param_is_sub));
            } else if (c == '_') {
                state->state = STATE_APC;
            } else if (c == '(') {
                state->state = 7;
            } else if (c == ')') {
                state->state = 8;
            } else if (c == '#') {
                state->state = 11; // ESC # ... (DECALN etc.)
            } else if (c == ']') {
                state->state = 9; // STATE_OSC
                state->osc_len = 0;
            } else if (c == 'P' || c == '^' || c == 'X') {
                // DCS / PM / SOS — swallowed until ST or BEL
                state->state = 4; // STATE_IGNORE_STRING
            } else if (c == 'H') {
                // HTS — set a tab stop at the cursor column
                if (state->tabstops && state->cursor_x < state->cols) {
                    state->tabstops[state->cursor_x] = 1;
                }
                state->state = STATE_NORMAL;
            } else if (c == 0x1B) {
                // ESC ESC — restart the escape sequence (xterm behavior)
                state->state = STATE_ESCAPE;
            } else if (c == '7') {
                // DECSC — Save Cursor
                state->saved_cursor_x = state->cursor_x;
                state->saved_cursor_y = state->cursor_y;
                state->saved_fg = state->current_fg;
                state->saved_bg = state->current_bg;
                state->saved_attrs = current_attrs(state);
                state->state = STATE_NORMAL;
            } else if (c == '8') {
                // DECRC — Restore Cursor
                state->cursor_x = state->saved_cursor_x;
                state->cursor_y = state->saved_cursor_y;
                state->current_fg = state->saved_fg;
                state->current_bg = state->saved_bg;
                state->bold = !!(state->saved_attrs & CELL_BOLD);
                state->dim = !!(state->saved_attrs & CELL_DIM);
                state->italic = !!(state->saved_attrs & CELL_ITALIC);
                state->underline = !!(state->saved_attrs & CELL_UNDERLINE);
                state->strikethrough = !!(state->saved_attrs & CELL_STRIKE);
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

                // Free scrollback and tab stops
                for (int i = 0; i < MAX_SCROLLBACK; i++) {
                    if (state->scrollback[i].cells) my_free(state->scrollback[i].cells);
                }
                if (state->tabstops) my_free(state->tabstops);

                // Free kitty protocol images and placements
                while (state->kitty_placements) {
                    KittyPlacement *p = state->kitty_placements;
                    state->kitty_placements = p->next;
                    my_free(p);
                }
                while (state->kitty_images) {
                    KittyImage *img = state->kitty_images;
                    state->kitty_images = img->next;
                    if (img->pixels) my_free(img->pixels);
                    my_free(img);
                }
                if (state->kitty_img.chunk_buf) {
                    my_free(state->kitty_img.chunk_buf);
                    state->kitty_img.chunk_buf = NULL;
                }
                state->kitty_img.chunk_cap = 0;
                state->kitty_img.chunk_len = 0;

                if (state->osc_buf) {
                    my_free(state->osc_buf);
                    state->osc_buf = NULL;
                }
                state->osc_cap = 0;
                state->osc_len = 0;

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
        } else if (state->state == 11) { // ESC # ...
            if (c == '8') {
                // DECALN — screen alignment test: fill with 'E', reset regions
                for (int y = 0; y < state->rows; y++) {
                    for (int x = 0; x < state->cols; x++) {
                        Cell *cell = &state->cells[y * state->cols + x];
                        cell->char_code = 'E';
                        cell->fg_color = g_config.fg_color;
                        cell->bg_color = g_config.bg_color;
                        cell->wrapped = 0;
                        cell->attrs = 0;
                    }
                }
                state->scroll_top = 0;
                state->scroll_bottom = state->rows - 1;
                state->origin_mode = 0;
                state->cursor_x = 0;
                state->cursor_y = 0;
            }
            state->state = STATE_NORMAL;
        } else if (state->state == STATE_CSI) {
            if (c >= '0' && c <= '9') {
                if (state->params_overflow) continue;
                if (state->num_params == 0) state->num_params = 1;
                state->params[state->num_params - 1] = state->params[state->num_params - 1] * 10 + (c - '0');
            } else if (c == ';') {
                if (state->num_params < 32) {
                    state->num_params++;
                } else {
                    state->params_overflow = 1;
                }
            } else if (c == ':') {
                // Sub-parameter (used by SGR 4:x, 38:2:..., 38:5:...)
                if (state->num_params < 32) {
                    state->num_params++;
                    state->param_is_sub[state->num_params - 1] = 1;
                } else {
                    state->params_overflow = 1;
                }
            } else if (c == '?') {
                state->csi_private = 1;
            } else if (c == '<' || c == '=' || c == '>') {
                // Private parameter prefixes (e.g. kitty keyboard CSI > u / CSI < u)
                state->csi_private = 2;
                state->csi_inter = c;
            } else if (c >= 0x20 && c <= 0x2F) {
                // Intermediate bytes (e.g. '!' of DECSTR, '$' of DECRQM, ' ' of DECSCUSR)
                // Must NOT clobber the '?' private marker (DECRQM is CSI ? Pd $ p).
                state->csi_inter = c;
            } else if (c >= 0x40 && c <= 0x7E) {
                handle_csi(state, c, state->csi_private);
            } else if (c == 0x1B) {
                // ESC inside CSI — abort the sequence, restart escape
                state->state = STATE_ESCAPE;
            } else if (c < 0x20) {
                // Other C0 controls execute within CSI (VT510 behavior)
                put_char(state, c);
            }
        } else if (state->state == STATE_APC) {
            if (c == 0x07 || c == 0x5C) { // BEL or bare backslash (rare)
                state->state = STATE_NORMAL;
                if (state->kitty_img.chunk_len > 0) {
                    if (state->kitty_img.chunk_len >= state->kitty_img.chunk_cap) {
                        state->kitty_img.chunk_cap = state->kitty_img.chunk_len + 1;
                        state->kitty_img.chunk_buf = my_realloc(state->kitty_img.chunk_buf, state->kitty_img.chunk_cap);
                    }
                    state->kitty_img.chunk_buf[state->kitty_img.chunk_len] = '\0';
                    parse_kitty_image_command(state);
                }
                state->kitty_img.chunk_len = 0;
                state->kitty_img.kitty_started = 0;
            } else if (c == 0x1B) { // ESC — begin ST sequence
                state->state = 6; // STATE_APC_ESC
            } else {
                if (!state->kitty_img.kitty_started) {
                    if (c == 'G') {
                        // This is the 'G' that starts a Kitty Graphics Protocol APC
                        state->kitty_img.kitty_started = 1;
                        state->kitty_img.chunk_len = 0;
                    }
                    // else: non-kitty APC (e.g. iTerm2), ignore
                } else {
                    if (state->kitty_img.chunk_len >= state->kitty_img.chunk_cap) {
                        state->kitty_img.chunk_cap = state->kitty_img.chunk_cap == 0 ? 1024 : state->kitty_img.chunk_cap * 2;
                        state->kitty_img.chunk_buf = my_realloc(state->kitty_img.chunk_buf, state->kitty_img.chunk_cap);
                    }
                    state->kitty_img.chunk_buf[state->kitty_img.chunk_len++] = c;
                }
            }
        } else if (state->state == 6) { // STATE_APC_ESC
            if (c == '\\') {
                state->state = STATE_NORMAL;
                if (state->kitty_img.chunk_len > 0) {
                    if (state->kitty_img.chunk_len >= state->kitty_img.chunk_cap) {
                        state->kitty_img.chunk_cap = state->kitty_img.chunk_len + 1;
                        state->kitty_img.chunk_buf = my_realloc(state->kitty_img.chunk_buf, state->kitty_img.chunk_cap);
                    }
                    state->kitty_img.chunk_buf[state->kitty_img.chunk_len] = '\0';
                    parse_kitty_image_command(state);
                }
                state->kitty_img.chunk_len = 0;
                state->kitty_img.kitty_started = 0;
            } else {
                state->state = STATE_APC;
            }
        } else if (state->state == 9) { // STATE_OSC
            if (c == 0x07) { // BEL terminator
                state->state = STATE_NORMAL;
                handle_osc(state);
                state->osc_len = 0;
            } else if (c == 0x1B) { // ESC — begin ST terminator
                state->state = 10; // STATE_OSC_ESC
            } else if (state->osc_len < (1 << 20)) { // cap OSC at 1 MB
                if (state->osc_len + 1 >= state->osc_cap) {
                    state->osc_cap = state->osc_cap == 0 ? 256 : state->osc_cap * 2;
                    state->osc_buf = my_realloc(state->osc_buf, state->osc_cap);
                }
                if (state->osc_buf) state->osc_buf[state->osc_len++] = (char)c;
            }
        } else if (state->state == 10) { // STATE_OSC_ESC
            if (c == '\\') { // ST terminator
                state->state = STATE_NORMAL;
                handle_osc(state);
                state->osc_len = 0;
            } else {
                state->state = 9; // malformed — back to OSC accumulation
            }
        }
    }
}
