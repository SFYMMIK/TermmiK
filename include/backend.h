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

#ifndef BACKEND_H
#define BACKEND_H

#include <stdint.h>

// Screen dimensions exposed to renderer
extern int g_width;
extern int g_height;

// Global pointer to the pixel buffer
extern uint32_t *g_framebuffer;

typedef struct {
    int (*init)(const char *font_pattern);
    void (*cleanup)(void);
    
    // Process backend events (non-blocking)
    // Return < 0 to quit
    int (*poll_events)(void);
    
    // Get the file descriptor to poll() on
    int (*get_fd)(void);
    
    // Flush the pixel buffer to the screen
    void (*flush)(void);
} WindowBackend;

extern WindowBackend *g_backend;

// Initializers
#ifdef _HAS_X11
WindowBackend* get_x11_backend();
#endif

#ifdef _HAS_WAYLAND
WindowBackend* get_wayland_backend();
#endif

// Callbacks that the backend will invoke
void term_resize(int width, int height);
void term_send_input(const char *buf, int len);
void term_scroll(int offset);

#endif
