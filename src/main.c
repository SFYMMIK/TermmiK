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

#include <stdio.h>
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
WindowBackend *g_backend = NULL;

int g_select_active = 0;
int g_select_dragging = 0;
int g_select_start_row = 0;
int g_select_start_col = 0;
int g_select_end_row = 0;
int g_select_end_col = 0;

void term_copy(void);

void term_resize(int width, int height) {
    int new_cols = (width - 2 * g_config.padding_x) / g_cell_width;
    if (new_cols < 1) new_cols = 1;
    int new_rows = (height - 2 * g_config.padding_y) / g_cell_height;
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

static int last_mouse_col = 0;
static int last_mouse_row = 0;

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
    int col = (x - g_config.padding_x) / g_cell_width;
    int row = (y - g_config.padding_y) / g_cell_height;
    last_mouse_col = col;
    last_mouse_row = row;
    
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

void term_mouse_motion(int x, int y) {
    int col = (x - g_config.padding_x) / g_cell_width;
    int row = (y - g_config.padding_y) / g_cell_height;
    last_mouse_col = col;
    last_mouse_row = row;

    if (vt_state.mouse_tracking_mode > 0) {
        if (vt_state.mouse_tracking_mode >= 1002) {
            // Button 0 (left) + 32 (motion) = 32
            term_send_mouse_event(32, 1, col, row);
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
            for (int c = start_c; c <= end_c; c++) {
                uint32_t code = row_cells[c].char_code;
                if (!code) code = ' ';
                if (code < 128) {
                    buf[idx++] = (char)code;
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

    g_width = 80 * g_cell_width + 2 * g_config.padding_x;
    g_height = 24 * g_cell_height + 2 * g_config.padding_y;

    if (g_backend->init(g_config.font_name) != 0) {
        my_print("Backend init failed\n");
        return 1;
    }

    vt_init(&vt_state, g_height / g_cell_height, g_width / g_cell_width, -1);

    pid_t child_pid;
    if (pty_spawn(&g_pty_fd, &child_pid, g_height / g_cell_height, g_width / g_cell_width) != 0) {
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

    while (1) {
        if (poll(fds, nfds, -1) < 0) {
            if (errno == EINTR) continue;
            break;
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
