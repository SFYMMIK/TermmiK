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

#define _GNU_SOURCE
#include "backend.h"
#include "config.h"
#include <wayland-client.h>
#include <wayland-cursor.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "render.h"

#include <sys/mman.h>
#include <sys/timerfd.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon.h>

static struct wl_display *wl_display = NULL;
static struct wl_compositor *wl_compositor = NULL;
static struct wl_shm *wl_shm = NULL;
static struct xdg_wm_base *xdg_wm_base = NULL;
static struct wl_seat *wl_seat = NULL;
static struct zxdg_decoration_manager_v1 *zxdg_decoration_manager = NULL;
static struct wl_output *wl_output = NULL;
static struct wl_keyboard *wl_keyboard = NULL;
static struct wl_pointer *wl_pointer = NULL;

static struct wl_surface *wl_surface = NULL;
static struct xdg_surface *xdg_surface = NULL;
static struct xdg_toplevel *xdg_toplevel = NULL;
static struct wl_buffer *wl_buffer = NULL;

// Pointer cursor (Wayland clients own their cursor images — without this the
// compositor shows no cursor while the pointer is over the window)
static struct wl_cursor_theme *cursor_theme = NULL;
static struct wl_cursor *cursor_left_ptr = NULL;
static struct wl_surface *cursor_surface = NULL;

// ---------------------------------------------------------------------------
// Cursor theme resolution. XCURSOR_THEME/XCURSOR_SIZE take priority; if the
// environment doesn't carry them (common when launching apps manually on
// desktop environments), fall back to the values the user set in their
// desktop settings — KDE's kcminputrc, then GTK's settings.ini.
// ---------------------------------------------------------------------------
static int read_config_value(const char *path, const char *key, char *out, int out_len) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    size_t key_len = strlen(key);
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '[' || *p == ';') continue;
        if (strncmp(p, key, key_len) == 0) {
            char *v = p + key_len;
            while (*v == ' ' || *v == '\t') v++;
            if (*v != '=') continue;
            v++;
            while (*v == ' ' || *v == '\t') v++;
            char *end = v + strlen(v);
            while (end > v && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
            *end = '\0';
            if (*v) {
                int n = 0;
                while (v[n] && n < out_len - 1) { out[n] = v[n]; n++; }
                out[n] = '\0';
                found = 1;
            }
            break;
        }
    }
    fclose(f);
    return found;
}

// Returns the explicit theme name to load, or NULL to let libwayland-cursor
// resolve XCURSOR_THEME / the system default. *size_out gets the resolved
// cursor size (> 0).
static const char *resolve_cursor_theme(char *name_out, int name_len, int *size_out) {
    name_out[0] = '\0';
    *size_out = 0;

    const char *env_size = getenv("XCURSOR_SIZE");
    if (env_size) *size_out = atoi(env_size);

    const char *env_theme = getenv("XCURSOR_THEME");
    if (env_theme && *env_theme) {
        if (*size_out <= 0) *size_out = 24;
        return NULL; // libwayland-cursor reads XCURSOR_THEME itself
    }

    const char *home = getenv("HOME");
    if (!home) {
        *size_out = (*size_out > 0) ? *size_out : 24;
        return NULL;
    }

    char path[512];
    char val[64];

    // KDE Plasma: ~/.config/kcminputrc -> [Mouse] cursorTheme= / cursorSize=
    snprintf(path, sizeof(path), "%s/.config/kcminputrc", home);
    if (read_config_value(path, "cursorTheme", name_out, name_len)) {
        if (*size_out <= 0 && read_config_value(path, "cursorSize", val, sizeof(val)))
            *size_out = atoi(val);
        if (*size_out <= 0) *size_out = 24;
        return name_out;
    }

    // GTK 3: ~/.config/gtk-3.0/settings.ini
    snprintf(path, sizeof(path), "%s/.config/gtk-3.0/settings.ini", home);
    if (read_config_value(path, "gtk-cursor-theme-name", name_out, name_len)) {
        if (*size_out <= 0 && read_config_value(path, "gtk-cursor-theme-size", val, sizeof(val)))
            *size_out = atoi(val);
        if (*size_out <= 0) *size_out = 24;
        return name_out;
    }

    // GTK 4: ~/.config/gtk-4.0/settings.ini
    snprintf(path, sizeof(path), "%s/.config/gtk-4.0/settings.ini", home);
    if (read_config_value(path, "gtk-cursor-theme-name", name_out, name_len)) {
        if (*size_out <= 0 && read_config_value(path, "gtk-cursor-theme-size", val, sizeof(val)))
            *size_out = atoi(val);
        if (*size_out <= 0) *size_out = 24;
        return name_out;
    }

    *size_out = (*size_out > 0) ? *size_out : 24;
    return NULL;
}

static int shm_fd = -1;
static uint32_t *shm_data = NULL;
static int pool_size = 0;

static struct xkb_context *xkb_context = NULL;
static struct xkb_keymap *xkb_keymap = NULL;
static struct xkb_state *xkb_state = NULL;

static struct wl_data_device_manager *wl_data_device_manager = NULL;
static struct wl_data_device *wl_data_device = NULL;
static struct wl_data_offer *current_data_offer = NULL;
static char *clipboard_text = NULL;
static uint32_t current_serial = 0;

// Mime types offered by the current selection source
#define MAX_OFFERED_MIMES 16
static char offered_mimes[MAX_OFFERED_MIMES][64];
static int num_offered_mimes = 0;

static const char *clipboard_mime_candidates[] = {
    "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING", "COMPOUND_TEXT",
};

static int key_repeat_fd = -1;
static int key_repeat_rate = 25;
static int key_repeat_delay = 300;
static char key_repeat_str[32];
static int key_repeat_len = 0;

static void data_offer_offer(void *data, struct wl_data_offer *offer, const char *mime_type) {
    if (num_offered_mimes < MAX_OFFERED_MIMES && strlen(mime_type) < sizeof(*offered_mimes)) {
        strcpy(offered_mimes[num_offered_mimes++], mime_type);
    }
}
static void data_offer_source_actions(void *data, struct wl_data_offer *offer, uint32_t source_actions) {}
static void data_offer_action(void *data, struct wl_data_offer *offer, uint32_t dnd_action) {}
static const struct wl_data_offer_listener data_offer_listener = { .offer = data_offer_offer, .source_actions = data_offer_source_actions, .action = data_offer_action };

static void data_device_data_offer(void *data, struct wl_data_device *device, struct wl_data_offer *offer) {
    num_offered_mimes = 0;
    wl_data_offer_add_listener(offer, &data_offer_listener, NULL);
}
static void data_device_enter(void *data, struct wl_data_device *device, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *id) {}
static void data_device_leave(void *data, struct wl_data_device *device) {}
static void data_device_motion(void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x, wl_fixed_t y) {}
static void data_device_drop(void *data, struct wl_data_device *device) {}
static void data_device_selection(void *data, struct wl_data_device *device, struct wl_data_offer *offer) {
    if (current_data_offer && current_data_offer != offer) {
        wl_data_offer_destroy(current_data_offer);
    }
    current_data_offer = offer;
    if (!offer) num_offered_mimes = 0;
}
static const struct wl_data_device_listener data_device_listener = {
    .data_offer = data_device_data_offer,
    .enter = data_device_enter, .leave = data_device_leave, .motion = data_device_motion, .drop = data_device_drop,
    .selection = data_device_selection
};

static void data_source_target(void *data, struct wl_data_source *source, const char *mime_type) {}
static void data_source_send(void *data, struct wl_data_source *source, const char *mime_type, int32_t fd) {
    if (clipboard_text) {
        write(fd, clipboard_text, strlen(clipboard_text));
    }
    close(fd);
}
static void data_source_cancelled(void *data, struct wl_data_source *source) { wl_data_source_destroy(source); }
static void data_source_dnd_drop_performed(void *data, struct wl_data_source *source) {}
static void data_source_dnd_finished(void *data, struct wl_data_source *source) {}
static void data_source_action(void *data, struct wl_data_source *source, uint32_t dnd_action) {}
static const struct wl_data_source_listener data_source_listener = {
    .target = data_source_target, .send = data_source_send, .cancelled = data_source_cancelled,
    .dnd_drop_performed = data_source_dnd_drop_performed, .dnd_finished = data_source_dnd_finished, .action = data_source_action
};

static void wayland_set_clipboard(const char *text) {
    if (clipboard_text) free(clipboard_text);
    clipboard_text = strdup(text);
    if (!wl_data_device_manager || !wl_data_device) return;
    struct wl_data_source *source = wl_data_device_manager_create_data_source(wl_data_device_manager);
    wl_data_source_add_listener(source, &data_source_listener, NULL);
    wl_data_source_offer(source, "text/plain;charset=utf-8");
    wl_data_source_offer(source, "text/plain");
    wl_data_source_offer(source, "UTF8_STRING");
    wl_data_device_set_selection(wl_data_device, source, current_serial);
}

static void wayland_get_clipboard(void) {
    if (!current_data_offer) return;

    // Pick the best text mime type the source actually offers. Some apps
    // (Chromium, GTK, clipboard managers) don't offer text/plain;charset=utf-8,
    // which previously made pasting from them silently do nothing.
    const char *mime = NULL;
    for (size_t i = 0; i < sizeof(clipboard_mime_candidates) / sizeof(*clipboard_mime_candidates) && !mime; i++) {
        for (int j = 0; j < num_offered_mimes; j++) {
            if (strcmp(offered_mimes[j], clipboard_mime_candidates[i]) == 0) {
                mime = offered_mimes[j];
                break;
            }
        }
    }
    if (!mime) mime = clipboard_mime_candidates[0];

    int fds[2];
    if (pipe(fds) < 0) return;
    wl_data_offer_receive(current_data_offer, mime, fds[1]);
    close(fds[1]);
    wl_display_flush(wl_display);

    char *buf = malloc(1024 * 1024);
    if (!buf) {
        close(fds[0]);
        return;
    }
    int total = 0;
    while (total < 1024 * 1024 - 1) {
        int n = read(fds[0], buf + total, 1024 * 1024 - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    close(fds[0]);
    if (total > 0) {
        buf[total] = 0;
        term_paste(buf, total);
    }
    free(buf);
}


static int create_shm_file(int size) {
    int fd = memfd_create("termmiK-wayland-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void resize_shm_pool(int width, int height) {
    int stride = width * 4;
    int size = stride * height;
    if (size <= pool_size) return;
    
    if (shm_fd >= 0) {
        munmap(shm_data, pool_size);
        close(shm_fd);
    }
    
    shm_fd = create_shm_file(size);
    shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    pool_size = size;
}

static void create_buffer(int width, int height) {
    if (wl_buffer) {
        wl_buffer_destroy(wl_buffer);
        wl_buffer = NULL;
    }
    
    resize_shm_pool(width, height);
    
    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, shm_fd, pool_size);
    wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    g_framebuffer = shm_data;
}

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    xdg_surface_ack_configure(xdg_surface, serial);
    
    if (!wl_buffer) {
        create_buffer(g_width, g_height);
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height, struct wl_array *states) {
    if (width > 0 && height > 0) {
        if (width != g_width || height != g_height) {
            g_width = width;
            g_height = height;
            create_buffer(g_width, g_height);
            term_resize(g_width, g_height);
        }
    }
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
    exit(0);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard, uint32_t format, int32_t fd, uint32_t size) {
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char *map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_str != MAP_FAILED) {
        xkb_keymap = xkb_keymap_new_from_string(xkb_context, map_str, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
        munmap(map_str, size);
        if (xkb_keymap) {
            xkb_state = xkb_state_new(xkb_keymap);
        }
    }
    close(fd);
}

static void key_repeat_start(const char *buf, int len) {
    if (key_repeat_fd < 0 || key_repeat_rate <= 0 || len <= 0) return;
    if (len > (int)sizeof(key_repeat_str)) len = sizeof(key_repeat_str);
    memcpy(key_repeat_str, buf, len);
    key_repeat_len = len;
    struct itimerspec ts = {0};
    ts.it_value.tv_sec = key_repeat_delay / 1000;
    ts.it_value.tv_nsec = (key_repeat_delay % 1000) * 1000000;
    if (ts.it_value.tv_sec == 0 && ts.it_value.tv_nsec == 0) {
        ts.it_value.tv_nsec = 1;
    }
    ts.it_interval.tv_sec = 0;
    ts.it_interval.tv_nsec = 1000000000 / key_repeat_rate;
    timerfd_settime(key_repeat_fd, 0, &ts, NULL);
}

static void key_repeat_stop(void) {
    if (key_repeat_fd >= 0) {
        struct itimerspec ts = {0};
        timerfd_settime(key_repeat_fd, 0, &ts, NULL);
    }
}

static void keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
    term_focus(1);
}
static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface) {
    term_focus(0);
    key_repeat_stop();
}

// mods bitfield: bit0 = shift, bit1 = alt, bit2 = ctrl
static int wayland_mods(void) {
    int mods = 0;
    if (xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= 1;
    if (xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= 2;
    if (xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= 4;
    return mods;
}

static int special_key_for_sym(xkb_keysym_t sym) {
    switch (sym) {
        case XKB_KEY_Up: return TKEY_UP;
        case XKB_KEY_Down: return TKEY_DOWN;
        case XKB_KEY_Right: return TKEY_RIGHT;
        case XKB_KEY_Left: return TKEY_LEFT;
        case XKB_KEY_Home: return TKEY_HOME;
        case XKB_KEY_End: return TKEY_END;
        case XKB_KEY_Insert: return TKEY_INSERT;
        case XKB_KEY_Delete: return TKEY_DELETE;
        case XKB_KEY_Page_Up: return TKEY_PAGEUP;
        case XKB_KEY_Page_Down: return TKEY_PAGEDOWN;
        case XKB_KEY_F1: return TKEY_F1;
        case XKB_KEY_F2: return TKEY_F2;
        case XKB_KEY_F3: return TKEY_F3;
        case XKB_KEY_F4: return TKEY_F4;
        case XKB_KEY_F5: return TKEY_F5;
        case XKB_KEY_F6: return TKEY_F6;
        case XKB_KEY_F7: return TKEY_F7;
        case XKB_KEY_F8: return TKEY_F8;
        case XKB_KEY_F9: return TKEY_F9;
        case XKB_KEY_F10: return TKEY_F10;
        case XKB_KEY_F11: return TKEY_F11;
        case XKB_KEY_F12: return TKEY_F12;
    }
    return -1;
}

// Map Ctrl+<key> to its C0 control character (xkbcommon does not apply Ctrl
// to xkb_state_key_get_utf8, so Ctrl+C previously sent a plain 'c').
static int ctrl_char_for_sym(xkb_keysym_t sym) {
    if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) return sym - XKB_KEY_a + 1;
    if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) return sym - XKB_KEY_A + 1;
    switch (sym) {
        case XKB_KEY_space: case XKB_KEY_at: case XKB_KEY_2: return 0x00;
        case XKB_KEY_3: case XKB_KEY_bracketleft: return 0x1B;
        case XKB_KEY_4: case XKB_KEY_backslash: return 0x1C;
        case XKB_KEY_5: case XKB_KEY_bracketright: return 0x1D;
        case XKB_KEY_6: case XKB_KEY_asciicircum: return 0x1E;
        case XKB_KEY_7: case XKB_KEY_underscore: case XKB_KEY_slash: return 0x1F;
        case XKB_KEY_8: return 0x7F;
    }
    return -1;
}

static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    current_serial = serial;
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        key_repeat_stop();
        return;
    }
    if (!xkb_state) return;

    xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_state, key + 8);
    int mods = wayland_mods();
    int shift = mods & 1;
    int ctrl = mods & 4;
    char buf[64];
    int len = 0;

    // Clipboard shortcuts
    if (ctrl && shift && (sym == XKB_KEY_C || sym == XKB_KEY_c)) {
        term_copy();
        return;
    }
    if (ctrl && shift && (sym == XKB_KEY_V || sym == XKB_KEY_v)) {
        wayland_get_clipboard();
        return;
    }

    if (sym == XKB_KEY_BackSpace) {
        buf[0] = '\x7f';
        len = 1;
    } else if (sym == XKB_KEY_KP_Enter) {
        buf[0] = '\r';
        len = 1;
    } else if (sym == XKB_KEY_ISO_Left_Tab || (sym == XKB_KEY_Tab && shift)) {
        len = term_send_special(TKEY_SHIFT_TAB, 0, buf);
    } else {
        int spec = special_key_for_sym(sym);
        if (spec >= 0) {
            len = term_send_special(spec, mods, buf);
        } else if (ctrl) {
            int cc = ctrl_char_for_sym(sym);
            if (cc >= 0) {
                if (mods & 2) buf[len++] = '\033'; // Ctrl+Alt = ESC + control char
                buf[len++] = (char)cc;
            }
        } else {
            len = xkb_state_key_get_utf8(xkb_state, key + 8, buf + (mods & 2 ? 1 : 0),
                                         sizeof(buf) - (mods & 2 ? 1 : 0));
            if (len > 0 && (mods & 2)) {
                buf[0] = '\033'; // Alt/Meta sends an ESC prefix
                len++;
            }
        }
    }

    if (len > 0) {
        term_send_input(buf, len);
        key_repeat_start(buf, len);
    }
}
static void keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    if (xkb_state) xkb_state_update_mask(xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}
static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay) {
    key_repeat_rate = rate;
    key_repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static wl_fixed_t last_ptr_x = 0;
static wl_fixed_t last_ptr_y = 0;
static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    last_ptr_x = surface_x;
    last_ptr_y = surface_y;
    // Attach the themed default arrow so the cursor stays visible over the
    // window; fall back to the compositor default if no theme was loaded.
    if (cursor_surface && cursor_left_ptr && cursor_left_ptr->image_count > 0) {
        struct wl_cursor_image *img = cursor_left_ptr->images[0];
        struct wl_buffer *img_buf = wl_cursor_image_get_buffer(img);
        if (img_buf) {
            wl_surface_attach(cursor_surface, img_buf, 0, 0);
            wl_surface_damage(cursor_surface, 0, 0, img->width, img->height);
            wl_surface_commit(cursor_surface);
            wl_pointer_set_cursor(pointer, serial, cursor_surface, img->hotspot_x, img->hotspot_y);
            return;
        }
    }
    wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
}
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface) {}


static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    current_serial = serial;
    int x = wl_fixed_to_int(last_ptr_x);
    int y = wl_fixed_to_int(last_ptr_y);
    int pressed = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    if (button == 272) { // Left click
        if (pressed) {
            term_mouse_down(x, y);
        } else {
            term_mouse_up(x, y);
            term_copy();
        }
    } else if (button == 273) { // Right click
        term_mouse_other(x, y, 2, pressed);
    } else if (button == 274) { // Middle click
        term_mouse_other(x, y, 1, pressed);
    }
}
static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    last_ptr_x = surface_x;
    last_ptr_y = surface_y;
    term_mouse_motion(wl_fixed_to_int(last_ptr_x), wl_fixed_to_int(last_ptr_y));
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        int v = wl_fixed_to_int(value);
        int step = (g_config.mouse_scroll_step > 0) ? g_config.mouse_scroll_step : 3;
        if (v < 0) term_scroll(step); // scroll up
        else if (v > 0) term_scroll(-step); // scroll down
    }
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        wl_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(wl_keyboard, &keyboard_listener, NULL);
    }
    if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
        wl_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(wl_pointer, &pointer_listener, NULL);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {}
static const struct wl_seat_listener seat_listener = { .capabilities = seat_capabilities, .name = seat_name };

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char *make, const char *model, int32_t transform) {}
static int sized_from_output = 0;
static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    if ((flags & WL_OUTPUT_MODE_CURRENT) && !sized_from_output) {
        sized_from_output = 1;
        float scale = g_config.window_scale;
        if (scale <= 0.05f || scale > 1.0f) scale = 0.9f;
        int cols = (int)(width * scale - g_config.padding_left - g_config.padding_right) / g_cell_width;
        int rows = (int)(height * scale - g_config.padding_top - g_config.padding_bottom) / g_cell_height;
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
        g_width = cols * g_cell_width + g_config.padding_left + g_config.padding_right;
        g_height = rows * g_cell_height + g_config.padding_top + g_config.padding_bottom;
    }
}
static void output_done(void *data, struct wl_output *wl_output) {}
static void output_scale(void *data, struct wl_output *wl_output, int32_t factor) {}
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        wl_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        wl_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, NULL);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        wl_data_device_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(wl_seat, &seat_listener, NULL);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        zxdg_decoration_manager = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        wl_output = wl_registry_bind(registry, name, &wl_output_interface, 2);
        wl_output_add_listener(wl_output, &output_listener, NULL);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = { .global = registry_global, .global_remove = registry_global_remove };

static int wayland_init(const char *font_pattern) {
    (void)font_pattern;
    wl_display = wl_display_connect(NULL);
    if (!wl_display) return -1;

    struct wl_registry *registry = wl_display_get_registry(wl_display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    
    xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    
    wl_display_roundtrip(wl_display);
    wl_display_roundtrip(wl_display);

    if (!wl_compositor || !wl_shm || !xdg_wm_base) return -1;

    // Load the user's cursor theme (XDG env -> KDE/GTK config -> system
    // default) so the pointer matches their desktop. Must happen before the
    // surface is mapped: the pointer can enter it as soon as it exists.
    char theme_name[64];
    int cursor_size = 0;
    const char *load_name = resolve_cursor_theme(theme_name, sizeof(theme_name), &cursor_size);
    cursor_theme = wl_cursor_theme_load(load_name, cursor_size, wl_shm);
    if (cursor_theme) {
        cursor_left_ptr = wl_cursor_theme_get_cursor(cursor_theme, "left_ptr");
        if (!cursor_left_ptr && load_name) {
            // Theme loaded but has no left_ptr — retry with the system default
            wl_cursor_theme_destroy(cursor_theme);
            cursor_theme = wl_cursor_theme_load(NULL, cursor_size, wl_shm);
            cursor_left_ptr = cursor_theme ? wl_cursor_theme_get_cursor(cursor_theme, "left_ptr") : NULL;
        }
    }
    cursor_surface = wl_compositor_create_surface(wl_compositor);

    wl_surface = wl_compositor_create_surface(wl_compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, wl_surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);

    xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, NULL);
    xdg_toplevel_set_title(xdg_toplevel, "TermmiK");

    if (zxdg_decoration_manager) {
        struct zxdg_toplevel_decoration_v1 *decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(zxdg_decoration_manager, xdg_toplevel);
        zxdg_toplevel_decoration_v1_set_mode(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }

    if (wl_data_device_manager && wl_seat) {
        wl_data_device = wl_data_device_manager_get_data_device(wl_data_device_manager, wl_seat);
        wl_data_device_add_listener(wl_data_device, &data_device_listener, NULL);
    }
    
    key_repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);


    wl_surface_commit(wl_surface);
    wl_display_roundtrip(wl_display); // Wait for configure

    return 0;
}

static void wayland_cleanup(void) {
    if (wl_buffer) wl_buffer_destroy(wl_buffer);
    if (xdg_toplevel) xdg_toplevel_destroy(xdg_toplevel);
    if (xdg_surface) xdg_surface_destroy(xdg_surface);
    if (cursor_surface) wl_surface_destroy(cursor_surface);
    if (wl_surface) wl_surface_destroy(wl_surface);
    if (cursor_theme) wl_cursor_theme_destroy(cursor_theme);
    if (wl_keyboard) wl_keyboard_destroy(wl_keyboard);
    if (wl_pointer) wl_pointer_destroy(wl_pointer);
    if (wl_seat) wl_seat_destroy(wl_seat);
    if (wl_output) wl_output_destroy(wl_output);
    if (zxdg_decoration_manager) zxdg_decoration_manager_v1_destroy(zxdg_decoration_manager);
    if (xdg_wm_base) xdg_wm_base_destroy(xdg_wm_base);
    if (wl_shm) wl_shm_destroy(wl_shm);
    if (wl_compositor) wl_compositor_destroy(wl_compositor);
    if (wl_display) wl_display_disconnect(wl_display);
    if (xkb_state) xkb_state_unref(xkb_state);
    if (xkb_keymap) xkb_keymap_unref(xkb_keymap);
    if (xkb_context) xkb_context_unref(xkb_context);
}

static int wayland_poll_events(void) {
    if (wl_display_dispatch(wl_display) == -1) return -1;
    return 0;
}

static void wayland_handle_timer(void) {
    if (key_repeat_fd >= 0) {
        uint64_t expirations;
        if (read(key_repeat_fd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
            for (uint64_t i = 0; i < expirations; i++) {
                term_send_input(key_repeat_str, key_repeat_len);
            }
        }
    }
}

static int wayland_get_fd(void) {
    return wl_display_get_fd(wl_display);
}

static void wayland_set_title(const char *title) {
    if (xdg_toplevel) xdg_toplevel_set_title(xdg_toplevel, title);
}

static void wayland_flush(void) {
    if (!wl_buffer) return;
    wl_surface_attach(wl_surface, wl_buffer, 0, 0);
    wl_surface_damage_buffer(wl_surface, 0, 0, g_width, g_height);
    wl_surface_commit(wl_surface);
    wl_display_flush(wl_display);
}

static int wayland_get_timer_fd(void) { return key_repeat_fd; }

static WindowBackend _wayland_backend = {
    .init = wayland_init,
    .cleanup = wayland_cleanup,
    .poll_events = wayland_poll_events,
    .get_fd = wayland_get_fd,
    .flush = wayland_flush,
    .get_timer_fd = wayland_get_timer_fd,
    .set_clipboard = wayland_set_clipboard,
    .get_clipboard = wayland_get_clipboard,
    .handle_timer = wayland_handle_timer,
    .set_title = wayland_set_title
};

WindowBackend* get_wayland_backend(void) {
    return &_wayland_backend;
}
