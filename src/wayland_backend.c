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
#include "xcursor.h"
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
static int wl_gl_active = 0;

// Pointer cursor (Wayland clients own their cursor images — without this the
// compositor shows no cursor while the pointer is over the window). The image
// is loaded from the user's Xcursor theme by the built-in reader; there is no
// libwayland-cursor dependency.
static struct wl_surface *cursor_surface = NULL;
static struct wl_buffer *cursor_buffer = NULL;
static int cursor_fd = -1;
static void *cursor_map = NULL;
static size_t cursor_map_size = 0;
static int cursor_w = 0, cursor_h = 0, cursor_hot_x = 0, cursor_hot_y = 0;
static int cursor_scale = 1; // buffer scale of the cursor surface (HiDPI)

// Output geometry/scale (HiDPI). Pointer coordinates arrive in logical
// surface units and are multiplied by the scale to reach framebuffer pixels.
static int screen_w = 0, screen_h = 0;
static int output_scale_factor = 1;
static int wayland_ready = 0; // init finished; scale events may reconfigure live

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
        snprintf(name_out, (size_t)name_len, "%s", env_theme);
        return name_out;
    }

    const char *home = getenv("HOME");
    if (!home) {
        *size_out = (*size_out > 0) ? *size_out : 24;
        snprintf(name_out, (size_t)name_len, "default");
        return name_out;
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
    snprintf(name_out, (size_t)name_len, "default");
    return name_out;
}

static int shm_fd = -1;
static uint32_t *shm_data = NULL;
static int pool_size = 0;

// Two buffers, alternated on every commit. Rendering into a buffer the
// compositor is still scanning out tears the frame and can show a partially
// painted screen (the "black flash" during animations), so a buffer is only
// ever drawn into after its release event arrived. When both buffers are
// busy the frame is deferred — the current screen contents simply stay up —
// and the pending release re-arms rendering. That trades a dropped frame for
// a seamless image, which is the right deal for a terminal.
#define WAYLAND_NUM_BUFFERS 2
// If the compositor never releases either buffer for this long, something is
// wrong on its side — take the least recently committed buffer back by force
// so output cannot stall forever (a rare tear beats a frozen terminal).
#define WAYLAND_STARVE_LIMIT_MS 250

typedef struct {
    struct wl_buffer *buffer;
    uint32_t *data;
    int busy;
    uint64_t commit_seq;
} ShmBuffer;

static ShmBuffer shm_buffers[WAYLAND_NUM_BUFFERS];
static int draw_buffer = -1; // buffer g_framebuffer points into, -1 = none free
static uint64_t commit_seq = 0;
static int64_t starve_start_ms = 0;

static uint32_t compositor_version = 0;

// Index of a buffer safe to render into, or -1 when the compositor still
// owns both. Caller must be on the UI thread (buffers change in callbacks).
static int pick_free_buffer(void) {
    for (int i = 0; i < WAYLAND_NUM_BUFFERS; i++) {
        ShmBuffer *b = &shm_buffers[i];
        if (b->buffer && !b->busy && i != draw_buffer) return i;
    }
    return -1;
}

// Hand the renderer a buffer to draw into. Returns 1 when g_framebuffer is
// usable, 0 while every buffer is still held by the compositor.
static int arm_draw_buffer(void) {
    if (g_framebuffer) return 1;
    int idx = pick_free_buffer();
    if (idx < 0 && starve_start_ms &&
        bell_now_ms() - starve_start_ms >= WAYLAND_STARVE_LIMIT_MS) {
        // Force-reuse the least recently committed buffer.
        int oldest = -1;
        uint64_t best = UINT64_MAX;
        for (int i = 0; i < WAYLAND_NUM_BUFFERS; i++) {
            ShmBuffer *b = &shm_buffers[i];
            if (b->buffer && b->commit_seq < best) { best = b->commit_seq; oldest = i; }
        }
        if (oldest >= 0) {
            shm_buffers[oldest].busy = 0;
            idx = oldest;
        }
    }
    if (idx < 0) {
        if (!starve_start_ms) starve_start_ms = bell_now_ms();
        return 0;
    }
    draw_buffer = idx;
    g_framebuffer = shm_buffers[idx].data;
    starve_start_ms = 0;
    return 1;
}

static void shm_buffer_release(void *data, struct wl_buffer *wl_buffer) {
    (void)wl_buffer;
    ((ShmBuffer *)data)->busy = 0;
    // The renderer may have been waiting for exactly this buffer.
    if (!g_framebuffer) term_request_render();
}

static const struct wl_buffer_listener shm_buffer_listener = {
    .release = shm_buffer_release,
};

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

// Load the pointer image from the theme and publish it as a wl_buffer.
// One static image is used (same behaviour as before, which took the first
// image of the themed cursor). Returns 1 when the cursor is usable.
static int load_pointer_cursor(const char *theme, int size) {
    XCursorImage img;
    if (!xcursor_load(theme, size, "left_ptr", &img) &&
        !xcursor_load(theme, size, "default", &img)) {
        return 0;
    }
    if (!wl_shm || img.w <= 0 || img.h <= 0) { xcursor_free(&img); return 0; }

    cursor_map_size = (size_t)img.w * img.h * 4u;
    cursor_fd = create_shm_file((int)cursor_map_size);
    if (cursor_fd < 0) { xcursor_free(&img); return 0; }
    cursor_map = mmap(NULL, cursor_map_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, cursor_fd, 0);
    if (cursor_map == MAP_FAILED) {
        cursor_map = NULL;
        close(cursor_fd);
        cursor_fd = -1;
        xcursor_free(&img);
        return 0;
    }
    memcpy(cursor_map, img.pixels, cursor_map_size);

    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, cursor_fd, (int)cursor_map_size);
    if (pool) {
        cursor_buffer = wl_shm_pool_create_buffer(pool, 0, img.w, img.h, img.w * 4,
                                                  WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
    }
    if (!cursor_buffer) {
        munmap(cursor_map, cursor_map_size);
        cursor_map = NULL;
        close(cursor_fd);
        cursor_fd = -1;
        cursor_map_size = 0;
        xcursor_free(&img);
        return 0;
    }

    cursor_w = img.w;
    cursor_h = img.h;
    cursor_hot_x = img.hot_x;
    cursor_hot_y = img.hot_y;
    xcursor_free(&img);
    return 1;
}

static void destroy_shm_buffers(void) {
    for (int i = 0; i < WAYLAND_NUM_BUFFERS; i++) {
        if (shm_buffers[i].buffer) {
            wl_buffer_destroy(shm_buffers[i].buffer);
        }
        shm_buffers[i].buffer = NULL;
        shm_buffers[i].data = NULL;
        shm_buffers[i].busy = 0;
        shm_buffers[i].commit_seq = 0;
    }
    draw_buffer = -1;
    starve_start_ms = 0;
    g_framebuffer = NULL;
    // Any cached damage state for the old buffers is meaningless now.
    render_forget_all_buffers();
}

// (Re)create the SHM pool and both scanout buffers for the current size.
static int create_shm_buffers(int width, int height) {
    destroy_shm_buffers();
    if (!wl_shm || width <= 0 || height <= 0) return -1;

    int stride = width * 4;
    int size = stride * height;
    int total = size * WAYLAND_NUM_BUFFERS;

    if (total > pool_size) {
        if (shm_fd >= 0) {
            if (shm_data) munmap(shm_data, pool_size);
            close(shm_fd);
            shm_data = NULL;
            shm_fd = -1;
            pool_size = 0;
        }
        shm_fd = create_shm_file(total);
        if (shm_fd < 0) return -1;
        shm_data = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        if (shm_data == MAP_FAILED) {
            shm_data = NULL;
            close(shm_fd);
            shm_fd = -1;
            return -1;
        }
        pool_size = total;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, shm_fd, pool_size);
    if (!pool) return -1;
    for (int i = 0; i < WAYLAND_NUM_BUFFERS; i++) {
        shm_buffers[i].buffer = wl_shm_pool_create_buffer(pool, i * size, width, height,
                                                          stride, WL_SHM_FORMAT_ARGB8888);
        if (!shm_buffers[i].buffer) {
            wl_shm_pool_destroy(pool);
            destroy_shm_buffers();
            return -1;
        }
        shm_buffers[i].data = (uint32_t *)((unsigned char *)shm_data + i * size);
        shm_buffers[i].busy = 0;
        wl_buffer_add_listener(shm_buffers[i].buffer, &shm_buffer_listener, &shm_buffers[i]);
    }
    wl_shm_pool_destroy(pool);

    draw_buffer = -1;
    g_framebuffer = NULL;
    starve_start_ms = 0;
    arm_draw_buffer();
    return 0;
}

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    xdg_surface_ack_configure(xdg_surface, serial);
    
    if (!shm_buffers[0].buffer) {
        create_shm_buffers(g_width, g_height);
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
            if (wl_gl_active) {
                // EGL owns the buffers now; just resync the text grid.
                term_resize(g_width, g_height);
            } else if (create_shm_buffers(g_width, g_height) == 0) {
                // Resize the buffer set first; the text grid follows. If the
                // allocation fails, rendering stays paused (no framebuffer).
                term_resize(g_width, g_height);
            }
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
        // Replace (never leak) the previous keymap — compositors send a new
        // one whenever the layout changes
        struct xkb_keymap *new_keymap = xkb_keymap_new_from_string(
            xkb_context, map_str, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
        munmap(map_str, size);
        if (new_keymap) {
            struct xkb_state *new_state = xkb_state_new(new_keymap);
            if (new_state) {
                if (xkb_state) xkb_state_unref(xkb_state);
                if (xkb_keymap) xkb_keymap_unref(xkb_keymap);
                xkb_keymap = new_keymap;
                xkb_state = new_state;
            } else {
                xkb_keymap_unref(new_keymap);
            }
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
    // Font zoom (Ctrl+Shift+=/-/0 and Ctrl+KP_Add/KP_Subtract)
    if (ctrl && (shift || sym == XKB_KEY_KP_Add || sym == XKB_KEY_KP_Subtract)) {
        if (sym == XKB_KEY_plus || sym == XKB_KEY_equal || sym == XKB_KEY_KP_Add) {
            term_zoom(+1);
            return;
        }
        if (sym == XKB_KEY_minus || sym == XKB_KEY_underscore || sym == XKB_KEY_KP_Subtract) {
            term_zoom(-1);
            return;
        }
        if (sym == XKB_KEY_0 || sym == XKB_KEY_KP_0 ||
            sym == XKB_KEY_parenright) { // Shift+0 reports as ')'
            term_zoom_reset();
            return;
        }
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
        term_clear_selection();
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
    if (cursor_surface && cursor_buffer) {
        wl_surface_attach(cursor_surface, cursor_buffer, 0, 0);
        wl_surface_damage(cursor_surface, 0, 0,
                          cursor_w / cursor_scale, cursor_h / cursor_scale);
        wl_surface_commit(cursor_surface);
        // Hotspots are surface-local: divide the pixel hotspot by the scale.
        wl_pointer_set_cursor(pointer, serial, cursor_surface,
                              cursor_hot_x / cursor_scale, cursor_hot_y / cursor_scale);
        return;
    }
    wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
}
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface) {}


static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    current_serial = serial;
    // Surface coordinates are logical; the framebuffer is in output pixels.
    int x = wl_fixed_to_int(last_ptr_x) * output_scale_factor;
    int y = wl_fixed_to_int(last_ptr_y) * output_scale_factor;
    int pressed = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    if (button == 272) { // Left click
        if (pressed) {
            if (xkb_state && wayland_mods() & 4) term_open_link(x, y); // Ctrl+click
            else term_mouse_down(x, y);
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
    term_mouse_motion(wl_fixed_to_int(last_ptr_x) * output_scale_factor,
                      wl_fixed_to_int(last_ptr_y) * output_scale_factor);
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    int v = wl_fixed_to_int(value);
    if (v == 0) return;
    if (xkb_state && (wayland_mods() & 4)) {
        // Ctrl+wheel zooms the font, like the key bindings.
        term_zoom(v < 0 ? +1 : -1);
        return;
    }
    int step = (g_config.mouse_scroll_step > 0) ? g_config.mouse_scroll_step : 3;
    if (v < 0) term_scroll(step); // scroll up
    else term_scroll(-step); // scroll down
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    // The compositor can add/remove devices at runtime; recreate only what
    // actually changed instead of leaking duplicate proxies
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !wl_keyboard) {
        wl_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(wl_keyboard, &keyboard_listener, NULL);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && wl_keyboard) {
        key_repeat_stop();
        wl_keyboard_destroy(wl_keyboard);
        wl_keyboard = NULL;
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !wl_pointer) {
        wl_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(wl_pointer, &pointer_listener, NULL);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && wl_pointer) {
        wl_pointer_destroy(wl_pointer);
        wl_pointer = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {}
static const struct wl_seat_listener seat_listener = { .capabilities = seat_capabilities, .name = seat_name };

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char *make, const char *model, int32_t transform) {}

// Size the window to a whole number of cells (window_scale of the screen).
static void compute_initial_size(void) {
    if (screen_w <= 0 || screen_h <= 0) return;
    float scale = g_config.window_scale;
    if (scale <= 0.05f || scale > 1.0f) scale = 0.9f;
    int cols = (int)(screen_w * scale - g_config.padding_left - g_config.padding_right) / g_cell_width;
    int rows = (int)(screen_h * scale - g_config.padding_top - g_config.padding_bottom) / g_cell_height;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    g_width = cols * g_cell_width + g_config.padding_left + g_config.padding_right;
    g_height = rows * g_cell_height + g_config.padding_top + g_config.padding_bottom;
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        screen_w = width;
        screen_h = height;
    }
}
static void output_done(void *data, struct wl_output *wl_output) {}

// HiDPI: the compositor tells us the output scale. Font metrics are scaled so
// text keeps its physical size but renders at native resolution, and the
// surface is told the buffer scale so logical coordinates stay unchanged.
static void output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
    if (factor < 1) factor = 1;
    if (factor == output_scale_factor) return;
    output_scale_factor = factor;
    if (compositor_version < 3) return; // no buffer-scale support: stay at 1x
    if (!wayland_ready) return; // applied during init, before buffers exist
    if (wl_surface && compositor_version >= 3)
        wl_surface_set_buffer_scale(wl_surface, factor);
    term_set_scale((float)factor);
    int buffer_recreated = 0;
#ifdef _HAS_GPU
    if (wl_gl_active) {
        extern void gpu_resize(int width, int height);
        gpu_resize(g_width, g_height);
        buffer_recreated = 1;
    }
#endif
    if (!buffer_recreated && create_shm_buffers(g_width, g_height) == 0) {
        term_resize(g_width, g_height);
    }
}
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    // Bind min(advertised, supported) — binding a higher version than the
    // compositor offers is a protocol error that would disconnect us.
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor_version = version < 4 ? version : 4;
        if (compositor_version < 1) compositor_version = 1;
        wl_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, compositor_version);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        wl_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, NULL);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        uint32_t v = version < 3 ? version : 3;
        wl_data_device_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, v < 1 ? 1 : v);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(wl_seat, &seat_listener, NULL);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        zxdg_decoration_manager = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        uint32_t v = version < 2 ? version : 2;
        wl_output = wl_registry_bind(registry, name, &wl_output_interface, v < 1 ? 1 : v);
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

    // HiDPI: the output events (mode + scale) arrived in the roundtrips
    // above. Scale the font metrics so the first buffers are created at the
    // native pixel size, then size the window to whole cells.
    if (output_scale_factor > 1 && compositor_version >= 3)
        term_set_scale((float)output_scale_factor);
    compute_initial_size();

    // Load the user's cursor theme (XDG env -> KDE/GTK config -> system
    // default) so the pointer matches their desktop. Must happen before the
    // surface is mapped: the pointer can enter it as soon as it exists.
    char theme_name[64];
    int cursor_size = 0;
    const char *theme = resolve_cursor_theme(theme_name, sizeof(theme_name), &cursor_size);
    // Load the pointer image at the output's pixel scale so it stays crisp
    // and the same physical size on HiDPI displays.
    int cursor_px = cursor_size * output_scale_factor;
    if (!load_pointer_cursor(theme, cursor_px) && strcmp(theme, "default") != 0) {
        // Configured theme unusable — fall back to the system default theme.
        load_pointer_cursor("default", cursor_px); // no cursor if this fails too
    }
    cursor_scale = output_scale_factor;
    cursor_surface = wl_compositor_create_surface(wl_compositor);
    if (compositor_version >= 3 && cursor_scale > 1) {
        wl_surface_set_buffer_scale(cursor_surface, cursor_scale);
    }

    wl_surface = wl_compositor_create_surface(wl_compositor);
    if (compositor_version >= 3 && output_scale_factor > 1) {
        // Buffer pixels are scale× the logical surface coordinates.
        wl_surface_set_buffer_scale(wl_surface, output_scale_factor);
    }
    xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, wl_surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);

    xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, NULL);
    xdg_toplevel_set_title(xdg_toplevel, "TermmiK");
    xdg_toplevel_set_app_id(xdg_toplevel, "termmik");

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

    wayland_ready = 1; // live scale changes may now reconfigure the buffers
    return 0;
}

static void wayland_cleanup(void) {
    destroy_shm_buffers();
    if (shm_fd >= 0) {
        if (shm_data) munmap(shm_data, pool_size);
        close(shm_fd);
        shm_fd = -1;
        shm_data = NULL;
        pool_size = 0;
    }
    if (xdg_toplevel) xdg_toplevel_destroy(xdg_toplevel);
    if (xdg_surface) xdg_surface_destroy(xdg_surface);
    if (cursor_surface) wl_surface_destroy(cursor_surface);
    if (wl_surface) wl_surface_destroy(wl_surface);
    if (cursor_buffer) wl_buffer_destroy(cursor_buffer);
    if (cursor_map) munmap(cursor_map, cursor_map_size);
    if (cursor_fd >= 0) close(cursor_fd);
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
                term_clear_selection();
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
    if (wl_gl_active) return; // EGL presents via eglSwapBuffers
    if (draw_buffer < 0) return; // no buffer was armed for this frame
    ShmBuffer *buf = &shm_buffers[draw_buffer];
    if (!buf->buffer || buf->busy) return;

    wl_surface_attach(wl_surface, buf->buffer, 0, 0);
    // Only report what actually changed this frame (the renderer tracks it);
    // a compositor re-reads less and the commit stays cheap.
    int y0 = g_damage_y0, y1 = g_damage_y1;
    if (y1 > y0) {
        if (compositor_version >= 4) {
            wl_surface_damage_buffer(wl_surface, 0, y0, g_width, y1 - y0);
        } else {
            // Older compositors: surface-local damage only (buffer damage is
            // protocol version 4+)
            wl_surface_damage(wl_surface, 0, y0, g_width, y1 - y0);
        }
    }
    wl_surface_commit(wl_surface);
    buf->busy = 1;
    buf->commit_seq = ++commit_seq;
    wl_display_flush(wl_display);

    // The next frame may only be drawn once the compositor releases a
    // buffer. If neither is free yet the render simply waits; the release
    // callback re-arms g_framebuffer and asks for a redraw.
    draw_buffer = -1;
    g_framebuffer = NULL;
    arm_draw_buffer();
}

static int wayland_get_timer_fd(void) { return key_repeat_fd; }

// Called by the main loop right before a frame is drawn: hand it a released
// scanout buffer, or report that every buffer is still held. Also enforces
// the starvation fallback so output cannot stall forever on a compositor
// that fails to send release events.
static int wayland_ready_to_draw(void) {
    if (wl_gl_active) return 1;
    return arm_draw_buffer();
}

static void *wayland_native_display(void) { return wl_display; }
static void *wayland_native_window(void) { return wl_surface; }
static int wayland_native_platform(void) { return 1; } // Wayland

static void wayland_gl_begin(void) {
    wl_gl_active = 1;
    // The EGL swapchain replaces the SHM buffers — release them (and the
    // pool mapping) so the GPU path doesn't pay for a CPU swapchain.
    destroy_shm_buffers();
    if (shm_fd >= 0) {
        if (shm_data) munmap(shm_data, pool_size);
        close(shm_fd);
        shm_data = NULL;
        shm_fd = -1;
        pool_size = 0;
    }
}

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
    .set_title = wayland_set_title,
    .get_native_display = wayland_native_display,
    .get_native_window = wayland_native_window,
    .get_native_platform = wayland_native_platform,
    .gl_begin = wayland_gl_begin,
    .ready_to_draw = wayland_ready_to_draw
};

WindowBackend* get_wayland_backend(void) {
    return &_wayland_backend;
}
