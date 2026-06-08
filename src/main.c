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

void term_resize(int width, int height) {
    int new_cols = (width - 2 * g_config.padding_x) / 9;
    if (new_cols < 1) new_cols = 1;
    int new_rows = (height - 2 * g_config.padding_y) / 18;
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

void term_scroll(int offset) {
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

    if (g_backend->init(g_config.font_name) != 0) {
        my_print("Backend init failed\n");
        return 1;
    }

    if (render_init(g_config.font_name) != 0) {
        my_print("Render init failed\n");
        return 1;
    }

    vt_init(&vt_state, g_height / 18, g_width / 9, -1);

    pid_t child_pid;
    if (pty_spawn(&g_pty_fd, &child_pid, g_height / 18, g_width / 9) != 0) {
        my_print("Failed to spawn PTY\n");
        return 1;
    }
    vt_state.pty_fd = g_pty_fd;

    struct pollfd fds[2];
    fds[0].fd = g_pty_fd;
    fds[0].events = POLLIN;
    fds[1].fd = g_backend->get_fd();
    fds[1].events = POLLIN;

    char buf[4096];

    while (1) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[1].revents & POLLIN) {
            if (g_backend->poll_events() < 0) break;
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
