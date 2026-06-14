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

#define _GNU_SOURCE
#include "backend.h"
#include "config.h"
#include <wayland-client.h>
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

static int key_repeat_fd = -1;
static int key_repeat_rate = 25;
static int key_repeat_delay = 300;
static char key_repeat_str[32];
static int key_repeat_len = 0;

static void data_offer_offer(void *data, struct wl_data_offer *offer, const char *mime_type) {}
static void data_offer_source_actions(void *data, struct wl_data_offer *offer, uint32_t source_actions) {}
static void data_offer_action(void *data, struct wl_data_offer *offer, uint32_t dnd_action) {}
static const struct wl_data_offer_listener data_offer_listener = { .offer = data_offer_offer, .source_actions = data_offer_source_actions, .action = data_offer_action };

static void data_device_data_offer(void *data, struct wl_data_device *device, struct wl_data_offer *offer) {
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
    int fds[2];
    if (pipe(fds) < 0) return;
    wl_data_offer_receive(current_data_offer, "text/plain;charset=utf-8", fds[1]);
    close(fds[1]);
    wl_display_flush(wl_display);
    
    char *buf = malloc(1024 * 1024);
    if (!buf) return;
    int total = 0;
    while (total < 1024 * 1024 - 1) {
        int n = read(fds[0], buf + total, 1024 * 1024 - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    close(fds[0]);
    if (total > 0) {
        buf[total] = 0;
        term_send_input(buf, total);
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

static void keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {}
static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface) {}
static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    current_serial = serial;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED && xkb_state) {
        xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_state, key + 8);
        char buf[32];
        int len = 0;

        if (sym == XKB_KEY_Up) { strcpy(buf, "\033[A"); len = 3; }
        else if (sym == XKB_KEY_Down) { strcpy(buf, "\033[B"); len = 3; }
        else if (sym == XKB_KEY_Right) { strcpy(buf, "\033[C"); len = 3; }
        else if (sym == XKB_KEY_Left) { strcpy(buf, "\033[D"); len = 3; }
        else {
            len = xkb_state_key_get_utf8(xkb_state, key + 8, buf, sizeof(buf));
            
            if (sym == XKB_KEY_C || sym == XKB_KEY_c) {
                if (xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) &&
                    xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE)) {
                    extern void term_copy(void);
                    term_copy();
                    return;
                }
            } else if (sym == XKB_KEY_V || sym == XKB_KEY_v) {
                if (xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) &&
                    xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE)) {
                    wayland_get_clipboard();
                    return;
                }
            }
        }
        
        if (len > 0) {
            term_send_input(buf, len);
            if (key_repeat_fd >= 0 && key_repeat_rate > 0) {
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
        }
    } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (key_repeat_fd >= 0) {
            struct itimerspec ts = {0};
            timerfd_settime(key_repeat_fd, 0, &ts, NULL);
        }
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

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {}
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface) {}


static wl_fixed_t last_ptr_x = 0;
static wl_fixed_t last_ptr_y = 0;
static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    current_serial = serial;
    if (button == 272) { // Left click
        extern void term_mouse_down(int x, int y);
        extern void term_mouse_up(int x, int y);
        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
            term_mouse_down(wl_fixed_to_int(last_ptr_x), wl_fixed_to_int(last_ptr_y));
        } else {
            term_mouse_up(wl_fixed_to_int(last_ptr_x), wl_fixed_to_int(last_ptr_y));
            extern void term_copy(void);
            term_copy();
        }
    }
}
static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    last_ptr_x = surface_x;
    last_ptr_y = surface_y;
    extern void term_mouse_motion(int x, int y);
    term_mouse_motion(wl_fixed_to_int(last_ptr_x), wl_fixed_to_int(last_ptr_y));
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        int v = wl_fixed_to_int(value);
        if (v < 0) term_scroll(3); // scroll up
        else if (v > 0) term_scroll(-3); // scroll down
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
static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        if (g_width == 80 * g_cell_width + 2 * g_config.padding_x && g_height == 24 * g_cell_height + 2 * g_config.padding_y) {
            g_width = width * 0.9;
            g_height = height * 0.9;
            int cols = (g_width - 2 * g_config.padding_x) / g_cell_width;
            int rows = (g_height - 2 * g_config.padding_y) / g_cell_height;
            g_width = cols * g_cell_width + 2 * g_config.padding_x;
            g_height = rows * g_cell_height + 2 * g_config.padding_y;
        }
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
    wl_display_roundtrip(wl_display);
    wl_display_roundtrip(wl_display);

    if (!wl_compositor || !wl_shm || !xdg_wm_base) return -1;

    xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

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
    if (wl_surface) wl_surface_destroy(wl_surface);
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
    .handle_timer = wayland_handle_timer
};

WindowBackend* get_wayland_backend(void) {
    return &_wayland_backend;
}
