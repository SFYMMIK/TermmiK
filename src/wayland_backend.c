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

#include <sys/mman.h>
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
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED && xkb_state) {
        xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_state, key + 8);
        if (sym == XKB_KEY_Up) term_send_input("\033[A", 3);
        else if (sym == XKB_KEY_Down) term_send_input("\033[B", 3);
        else if (sym == XKB_KEY_Right) term_send_input("\033[C", 3);
        else if (sym == XKB_KEY_Left) term_send_input("\033[D", 3);
        else {
            char buf[32];
            int len = xkb_keysym_to_utf8(sym, buf, sizeof(buf));
            if (len > 0) {
                term_send_input(buf, len - 1);
            }
        }
    }
}
static void keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    if (xkb_state) xkb_state_update_mask(xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}
static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay) {}

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
static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {}
static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {}
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
        if (g_width == 80 * 9 + 2 * g_config.padding_x && g_height == 24 * 18 + 2 * g_config.padding_y) {
            g_width = width * 0.9;
            g_height = height * 0.9;
            int cols = (g_width - 2 * g_config.padding_x) / 9;
            int rows = (g_height - 2 * g_config.padding_y) / 18;
            g_width = cols * 9 + 2 * g_config.padding_x;
            g_height = rows * 18 + 2 * g_config.padding_y;
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

    g_width = 80 * 9 + 2 * g_config.padding_x;
    g_height = 24 * 18 + 2 * g_config.padding_y;

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

static WindowBackend _wayland_backend = {
    .init = wayland_init,
    .cleanup = wayland_cleanup,
    .poll_events = wayland_poll_events,
    .get_fd = wayland_get_fd,
    .flush = wayland_flush
};

WindowBackend* get_wayland_backend(void) {
    return &_wayland_backend;
}
