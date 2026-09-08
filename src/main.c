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
#include <time.h>
#include "alloc.h"
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
#include <errno.h>

#include "pty.h"
#include "vt_parser.h"
#include "render.h"
#include "config.h"
#include "backend.h"

int g_width = 80 * 9;
int g_height = 24 * 18;
uint32_t *g_framebuffer = NULL;

int g_pty_fd = -1;
static VTState vt_state;
static int needs_render = 1;
static int vt_initialized = 0;
WindowBackend *g_backend = NULL;

int g_select_active = 0;
int g_select_dragging = 0;
int g_select_start_row = 0;
int g_select_start_col = 0;
int g_select_end_row = 0;
int g_select_end_col = 0;

// Cursor blink phase (toggled on idle by the main loop, reset on activity)
int g_cursor_blink_on = 1;

void term_copy(void);

void term_resize(int width, int height) {
    if (!vt_initialized) return;
    
    int new_cols = (width - g_config.padding_left - g_config.padding_right) / g_cell_width;
    if (new_cols < 1) new_cols = 1;
    int new_rows = (height - g_config.padding_top - g_config.padding_bottom) / g_cell_height;
    if (new_rows < 1) new_rows = 1;

    if (new_cols != vt_state.cols || new_rows != vt_state.rows) {
        vt_resize(&vt_state, new_rows, new_cols);
        pty_resize(g_pty_fd, new_rows, new_cols);
    }
    needs_render = 1;
}

void term_send_input(const char *buf, int len) {
    if (len > 0) {
        pty_write(g_pty_fd, buf, len);
    }
}

// Paste text into the PTY, wrapping it in bracketed-paste markers if the
// running application has enabled mode ?2004. This lets shells and TUI apps
// (fish, zsh, nvim, opencode...) treat the paste as a single operation.
// A bracketed-paste end marker embedded in the payload is treated as the end
// of the paste (spec-compliant safety measure against escape injection).
void term_paste(const char *text, int len) {
    if (len <= 0) return;
    if (vt_state.bracketed_paste) {
        // Truncate the payload at an embedded end/start marker
        for (int i = 0; i + 6 <= len; i++) {
            if (text[i] == '\033' && text[i+1] == '[' &&
                text[i+2] == '2' && text[i+3] == '0' &&
                text[i+4] == '1' && text[i+5] == '~') {
                len = i;
                break;
            }
        }
        pty_write(g_pty_fd, "\033[200~", 6);
        if (len > 0) pty_write(g_pty_fd, text, len);
        pty_write(g_pty_fd, "\033[201~", 6);
    } else {
        pty_write(g_pty_fd, text, len);
    }
}

// Send a focus event to the application if it requested focus reporting (?1004)
void term_focus(int focused) {
    if (vt_state.focus_reporting) {
        pty_write(g_pty_fd, focused ? "\033[I" : "\033[O", 3);
    }
}

// Hooks used by the VT parser (OSC sequences)
void term_set_title(const char *title) {
    if (g_backend && g_backend->set_title) g_backend->set_title(title);
}

void term_set_clipboard(const char *text) {
    if (g_backend && g_backend->set_clipboard) g_backend->set_clipboard(text);
}

// mods bitfield: bit0 = shift, bit1 = alt, bit2 = ctrl
// Fills `out` with the xterm-style escape sequence and returns its length.
int term_send_special(int key, int mods, char *out) {
    static const struct { int code; char final; } nav[] = {
        [TKEY_HOME] = {1, 'H'}, [TKEY_END] = {1, 'F'},
    };
    static const struct { int code; char final; } tilde[] = {
        [TKEY_INSERT] = {2, '~'}, [TKEY_DELETE] = {3, '~'},
        [TKEY_PAGEUP] = {5, '~'}, [TKEY_PAGEDOWN] = {6, '~'},
    };
    static const struct { int code; char final; } fkeys[] = {
        [TKEY_F5] = {15, '~'}, [TKEY_F6] = {17, '~'}, [TKEY_F7] = {18, '~'},
        [TKEY_F8] = {19, '~'}, [TKEY_F9] = {20, '~'}, [TKEY_F10] = {21, '~'},
        [TKEY_F11] = {23, '~'}, [TKEY_F12] = {24, '~'},
    };
    static const char f1234[] = { 'P', 'Q', 'R', 'S' };

    int mod = 1 + (mods & 1) + ((mods >> 1) & 1) * 2 + ((mods >> 2) & 1) * 4;
    int app = vt_state.app_cursor_keys;

    switch (key) {
        case TKEY_UP: case TKEY_DOWN: case TKEY_RIGHT: case TKEY_LEFT: {
            char f = 'A' + (key - TKEY_UP);
            if (mod > 1) return snprintf(out, 32, "\033[1;%d%c", mod, f);
            if (app) return snprintf(out, 32, "\033O%c", f);
            return snprintf(out, 32, "\033[%c", f);
        }
        case TKEY_HOME: case TKEY_END:
            if (mod > 1) return snprintf(out, 32, "\033[1;%d%c", mod, nav[key].final);
            if (app) return snprintf(out, 32, "\033O%c", nav[key].final);
            return snprintf(out, 32, "\033[%c", nav[key].final);
        case TKEY_INSERT: case TKEY_DELETE: case TKEY_PAGEUP: case TKEY_PAGEDOWN:
            if (mod > 1) return snprintf(out, 32, "\033[%d;%d%c", tilde[key].code, mod, tilde[key].final);
            return snprintf(out, 32, "\033[%d%c", tilde[key].code, tilde[key].final);
        case TKEY_F5: case TKEY_F6: case TKEY_F7: case TKEY_F8:
        case TKEY_F9: case TKEY_F10: case TKEY_F11: case TKEY_F12:
            if (mod > 1) return snprintf(out, 32, "\033[%d;%d%c", fkeys[key].code, mod, fkeys[key].final);
            return snprintf(out, 32, "\033[%d%c", fkeys[key].code, fkeys[key].final);
        case TKEY_F1: case TKEY_F2: case TKEY_F3: case TKEY_F4:
            if (mod > 1) return snprintf(out, 32, "\033[1;%d%c", mod, f1234[key - TKEY_F1]);
            return snprintf(out, 32, "\033O%c", f1234[key - TKEY_F1]);
        case TKEY_SHIFT_TAB:
            return snprintf(out, 32, "\033[Z");
    }
    return 0;
}

static int last_mouse_col = 0;
static int last_mouse_row = 0;
static int held_mouse_button = -1; // 0=left, 1=middle, 2=right, -1=none

static void term_send_mouse_event(int button, int is_press, int col, int row) {
    if (vt_state.mouse_tracking_mode == 0) return;
    int x = col + 1;
    int y = row + 1;
    if (vt_state.mouse_sgr_mode) {
        char buf[64];
        int len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c", button, x, y, is_press ? 'M' : 'm');
        term_send_input(buf, len);
    } else {
        if (x > 223) x = 223;
        if (y > 223) y = 223;
        int b = is_press ? button : 3;
        char buf[6];
        buf[0] = '\033'; buf[1] = '['; buf[2] = 'M';
        buf[3] = (char)(32 + b);
        buf[4] = (char)(32 + x);
        buf[5] = (char)(32 + y);
        term_send_input(buf, 6);
    }
}

void term_mouse_down(int x, int y) {
    int col = (x - g_config.padding_left) / g_cell_width;
    int row = (y - g_config.padding_top) / g_cell_height;
    last_mouse_col = col;
    last_mouse_row = row;
    held_mouse_button = 0;

    if (vt_state.mouse_tracking_mode > 0) {
        term_send_mouse_event(0, 1, col, row);
        return;
    }

    g_select_active = 0;
    g_select_dragging = 1;
    g_select_start_col = col;
    g_select_start_row = row - vt_state.scroll_offset;
    g_select_end_col = col;
    g_select_end_row = g_select_start_row;
    needs_render = 1;
}

// Forward middle/right (or extra) buttons to the application when it has
// requested mouse events. No-op otherwise.
void term_mouse_other(int x, int y, int button, int pressed) {
    int col = (x - g_config.padding_left) / g_cell_width;
    int row = (y - g_config.padding_top) / g_cell_height;
    last_mouse_col = col;
    last_mouse_row = row;

    if (vt_state.mouse_tracking_mode == 0) return;
    held_mouse_button = pressed ? button : -1;
    term_send_mouse_event(button, pressed, col, row);
}

void term_mouse_motion(int x, int y) {
    int col = (x - g_config.padding_left) / g_cell_width;
    int row = (y - g_config.padding_top) / g_cell_height;
    last_mouse_col = col;
    last_mouse_row = row;

    if (vt_state.mouse_tracking_mode > 0) {
        if (vt_state.mouse_tracking_mode == 1003) {
            // Any-motion tracking: 35 = plain hover, 32-34 = motion with button held
            int code = (held_mouse_button >= 0) ? 32 + held_mouse_button : 35;
            term_send_mouse_event(code, 1, col, row);
        } else if (vt_state.mouse_tracking_mode >= 1002 && held_mouse_button >= 0) {
            // Button-event tracking: motion while a button is held
            term_send_mouse_event(32 + held_mouse_button, 1, col, row);
        }
        return;
    }

    if (g_select_dragging) {
        g_select_active = 1;
        if (col < 0) col = 0;
        if (col >= vt_state.cols) col = vt_state.cols - 1;
        g_select_end_col = col;
        g_select_end_row = row - vt_state.scroll_offset;
        needs_render = 1;
    }
}

void term_mouse_up(int x, int y) {
    held_mouse_button = -1;
    if (vt_state.mouse_tracking_mode > 0) {
        term_send_mouse_event(0, 0, last_mouse_col, last_mouse_row);
        return;
    }
    g_select_dragging = 0;
}

void term_copy(void) {
    if (!g_select_active || !g_backend->set_clipboard) return;

    int r1 = g_select_start_row, c1 = g_select_start_col;
    int r2 = g_select_end_row, c2 = g_select_end_col;

    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        int tr = r1; r1 = r2; r2 = tr;
        int tc = c1; c1 = c2; c2 = tc;
    }

    char *buf = malloc(1024 * 1024);
    if (!buf) return;
    int idx = 0;

    for (int r = r1; r <= r2; r++) {
        int start_c = (r == r1) ? c1 : 0;
        int end_c = (r == r2) ? c2 : vt_state.cols - 1;

        Cell *row_cells = NULL;
        if (r < 0) {
            int max_sb = g_config.scrollback_lines;
            if (max_sb > MAX_SCROLLBACK) max_sb = MAX_SCROLLBACK;
            if (max_sb <= 0) max_sb = 1;

            int back = -r;
            if (back <= vt_state.scrollback_count) {
                int real_idx = (vt_state.scrollback_head - back + max_sb) % max_sb;
                row_cells = vt_state.scrollback[real_idx].cells;
            }
        } else if (r < vt_state.rows) {
            row_cells = &vt_state.cells[r * vt_state.cols];
        }

        if (row_cells) {
            for (int c = start_c; c <= end_c && idx < 1024 * 1024 - 8; c++) {
                uint32_t code = row_cells[c].char_code;
                if (!code) code = ' ';
                // Encode as UTF-8 so selections keep non-ASCII characters
                if (code < 0x80) {
                    buf[idx++] = (char)code;
                } else if (code < 0x800) {
                    buf[idx++] = (char)(0xC0 | (code >> 6));
                    buf[idx++] = (char)(0x80 | (code & 0x3F));
                } else if (code < 0x10000) {
                    buf[idx++] = (char)(0xE0 | (code >> 12));
                    buf[idx++] = (char)(0x80 | ((code >> 6) & 0x3F));
                    buf[idx++] = (char)(0x80 | (code & 0x3F));
                } else if (code <= 0x10FFFF) {
                    buf[idx++] = (char)(0xF0 | (code >> 18));
                    buf[idx++] = (char)(0x80 | ((code >> 12) & 0x3F));
                    buf[idx++] = (char)(0x80 | ((code >> 6) & 0x3F));
                    buf[idx++] = (char)(0x80 | (code & 0x3F));
                }
            }
        }
        if (r != r2) buf[idx++] = '\n';
    }
    buf[idx] = '\0';
    g_backend->set_clipboard(buf);
    free(buf);
}

void term_scroll(int offset) {
    if (vt_state.mouse_tracking_mode > 0) {
        int button = (offset > 0) ? 64 : 65; // 64 = up, 65 = down
        term_send_mouse_event(button, 1, last_mouse_col, last_mouse_row);
        return;
    }

    if (vt_state.alt_screen_active) {
        if (vt_state.alt_scroll) {
            // Alternate scroll mode (?1007): the app is full-screen, forward the
            // wheel as arrow keys so pagers like `less` scroll naturally.
            char buf[32];
            int len = term_send_special(offset > 0 ? TKEY_UP : TKEY_DOWN, 0, buf);
            term_send_input(buf, len);
        }
        // Without ?1007 there is nothing to scroll in the alt screen — do not
        // overlay scrollback on top of the alternate buffer.
        return;
    }

    vt_state.scroll_offset += offset;
    if (vt_state.scroll_offset < 0) vt_state.scroll_offset = 0;
    if (vt_state.scroll_offset > vt_state.scrollback_count) {
        vt_state.scroll_offset = vt_state.scrollback_count;
    }
    needs_render = 1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    config_load();

#ifdef _HAS_WAYLAND
    if (getenv("WAYLAND_DISPLAY")) {
        my_print("Starting Wayland backend...\n");
        g_backend = get_wayland_backend();
    }
#endif
#ifdef _HAS_X11
    if (!g_backend && getenv("DISPLAY")) {
        my_print("Starting X11 backend...\n");
        g_backend = get_x11_backend();
    }
#endif

    if (!g_backend) {
        my_print("No display server found or built without backend support\n");
        return 1;
    }

    if (render_init(g_config.font_name) != 0) {
        my_print("Render init failed\n");
        return 1;
    }

    g_width = 80 * g_cell_width + g_config.padding_left + g_config.padding_right;
    g_height = 24 * g_cell_height + g_config.padding_top + g_config.padding_bottom;

    if (g_backend->init(g_config.font_name) != 0) {
        my_print("Backend init failed\n");
        return 1;
    }

    int initial_cols = (g_width - g_config.padding_left - g_config.padding_right) / g_cell_width;
    if (initial_cols < 1) initial_cols = 1;
    int initial_rows = (g_height - g_config.padding_top - g_config.padding_bottom) / g_cell_height;
    if (initial_rows < 1) initial_rows = 1;

    vt_init(&vt_state, initial_rows, initial_cols, -1);
    vt_initialized = 1;

    pid_t child_pid;
    if (pty_spawn(&g_pty_fd, &child_pid, initial_rows, initial_cols) != 0) {
        my_print("Failed to spawn PTY\n");
        return 1;
    }
    vt_state.pty_fd = g_pty_fd;

    struct pollfd fds[3];
    int nfds = 2;
    fds[0].fd = g_pty_fd;
    fds[0].events = POLLIN;
    fds[1].fd = g_backend->get_fd();
    fds[1].events = POLLIN;

    int timer_fd = g_backend->get_timer_fd ? g_backend->get_timer_fd() : -1;
    if (timer_fd >= 0) {
        fds[2].fd = timer_fd;
        fds[2].events = POLLIN;
        nfds = 3;
    }

    char buf[4096];
    struct timespec last_scroll_time = {0};

    while (1) {
        int timeout = -1;
        if (g_select_dragging) {
            timeout = 100;
        } else if (g_config.cursor_blink) {
            timeout = (g_config.cursor_blink_interval > 0) ? g_config.cursor_blink_interval : 300;
        }

        int poll_result = poll(fds, nfds, timeout);
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (poll_result == 0) {
            // Idle timeout
            if (g_config.cursor_blink && !g_select_dragging) {
                g_cursor_blink_on = !g_cursor_blink_on;
                needs_render = 1;
            }
        } else {
            // Any activity keeps the cursor solid and resets the phase
            if (g_config.cursor_blink && !g_cursor_blink_on) {
                g_cursor_blink_on = 1;
                needs_render = 1;
            }
        }

        if (g_select_dragging) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec - last_scroll_time.tv_sec) * 1000 + 
                              (now.tv_nsec - last_scroll_time.tv_nsec) / 1000000;
            if (elapsed_ms >= 100) {
                int scrolled = 0;
                if (last_mouse_row <= 0) {
                    term_scroll(1);
                    scrolled = 1;
                } else if (last_mouse_row >= vt_state.rows - 1) {
                    term_scroll(-1);
                    scrolled = 1;
                }
                
                if (scrolled) {
                    g_select_active = 1;
                    int col = last_mouse_col;
                    if (col < 0) col = 0;
                    if (col >= vt_state.cols) col = vt_state.cols - 1;
                    g_select_end_col = col;
                    g_select_end_row = last_mouse_row - vt_state.scroll_offset;
                    needs_render = 1;
                }
                last_scroll_time = now;
            }
        }

        if (fds[1].revents & POLLIN) {
            if (g_backend->poll_events() < 0) break;
        }
        if (nfds == 3 && (fds[2].revents & POLLIN)) {
            if (g_backend->handle_timer) g_backend->handle_timer();
        }

        if (fds[0].revents & POLLIN) {
            ssize_t bytes = pty_read(g_pty_fd, buf, sizeof(buf));
            if (bytes <= 0) break; // Child died
            vt_state.scroll_offset = 0;
            vt_process(&vt_state, buf, bytes);
            needs_render = 1;
        } else if (fds[0].revents & (POLLHUP | POLLERR)) {
            break;
        }

        if (needs_render) {
            render_draw(&vt_state);
            g_backend->flush();
            needs_render = 0;
        }
    }

    g_backend->cleanup();
    return 0;
}
