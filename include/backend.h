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
    int (*get_timer_fd)(void);
    void (*set_clipboard)(const char *text);
    void (*get_clipboard)(void);
    void (*handle_timer)(void);
    void (*set_title)(const char *title);
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
void term_clear_selection(void); // dismiss the current text selection
void sound_play_key(void);       // play the configured typing sound (no-op if unset)
void term_scroll(int offset);

// Shared input helpers implemented in main.c
void term_paste(const char *text, int len);          // paste with bracketed-paste support
void term_focus(int focused);                        // focus in/out events (?1004)
int term_send_special(int key, int mods, char *out); // escape sequences for nav/F-keys
void term_mouse_other(int x, int y, int button, int pressed); // middle/right mouse buttons
void term_mouse_down(int x, int y);
void term_mouse_up(int x, int y);
void term_mouse_motion(int x, int y);
void term_copy(void);

// Special key codes (used with term_send_special)
enum {
    TKEY_UP, TKEY_DOWN, TKEY_RIGHT, TKEY_LEFT,
    TKEY_HOME, TKEY_END, TKEY_INSERT, TKEY_DELETE,
    TKEY_PAGEUP, TKEY_PAGEDOWN,
    TKEY_F1, TKEY_F2, TKEY_F3, TKEY_F4, TKEY_F5, TKEY_F6,
    TKEY_F7, TKEY_F8, TKEY_F9, TKEY_F10, TKEY_F11, TKEY_F12,
    TKEY_SHIFT_TAB
};

#endif
