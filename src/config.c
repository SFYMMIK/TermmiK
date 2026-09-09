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

#include "config.h"
#include "alloc.h"
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>

TermConfig g_config;

// Startup snapshot for OSC 104/110/111 palette resets
static uint32_t default_palette[16];
static uint32_t default_fg = 0xDDDDDD;
static uint32_t default_bg = 0x000000;
static int defaults_saved = 0;

void config_reset_palette(int idx) {
    if (!defaults_saved || idx < 0 || idx >= 16) return;
    g_config.colors[idx] = default_palette[idx];
}

void config_reset_default_colors(void) {
    if (!defaults_saved) return;
    g_config.fg_color = default_fg;
    g_config.bg_color = default_bg;
}

static void snapshot_defaults(void) {
    memcpy(default_palette, g_config.colors, sizeof(default_palette));
    default_fg = g_config.fg_color;
    default_bg = g_config.bg_color;
    defaults_saved = 1;
}

static int starts_with(const char *str, const char *prefix, int len) {
    for (int i = 0; i < len; i++) {
        if (!prefix[i]) return 1;
        if (str[i] != prefix[i]) return 0;
    }
    return prefix[len] == 0;
}

static uint32_t parse_hex(const char *str, int len) {
    if (len > 0 && str[0] == '#') { str++; len--; }
    uint32_t val = 0;
    for (int i = 0; i < len && i < 6; i++) {
        val <<= 4;
        char c = str[i];
        if (c >= '0' && c <= '9') val |= (c - '0');
        else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
    }
    return val;
}

static int parse_int(const char *str, int len) {
    int val = 0;
    for (int i = 0; i < len; i++) {
        if (str[i] >= '0' && str[i] <= '9') {
            val = val * 10 + (str[i] - '0');
        }
    }
    return val;
}

static float parse_float(const char *str, int len) {
    float val = 0.0f;
    float frac = 0.0f;
    float div = 1.0f;
    int in_frac = 0;
    for (int i = 0; i < len; i++) {
        if (str[i] == '.') { in_frac = 1; continue; }
        if (str[i] >= '0' && str[i] <= '9') {
            if (!in_frac) val = val * 10 + (str[i] - '0');
            else { frac = frac * 10 + (str[i] - '0'); div *= 10; }
        }
    }
    return val + (frac / div);
}

static void copy_string(char *dest, const char *src, int len, int max_len) {
    int i;
    for (i = 0; i < len && i < max_len - 1; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

extern char *getenv(const char *name);

void config_load(void) {
    g_config.colors[0] = 0x000000; g_config.colors[8] = 0x767676;
    g_config.colors[1] = 0xcc0403; g_config.colors[9] = 0xf2201f;
    g_config.colors[2] = 0x19cb00; g_config.colors[10] = 0x23fd00;
    g_config.colors[3] = 0xcecb00; g_config.colors[11] = 0xfffd00;
    g_config.colors[4] = 0x0d73cc; g_config.colors[12] = 0x1a8fcf;
    g_config.colors[5] = 0xcb1ed1; g_config.colors[13] = 0xfd28ff;
    g_config.colors[6] = 0x0dcdcd; g_config.colors[14] = 0x14ffff;
    g_config.colors[7] = 0xdddddd; g_config.colors[15] = 0xffffff;

    g_config.fg_color = 0xdddddd;
    g_config.bg_color = 0x000000;
    copy_string(g_config.font_name, "monospace", 9, 256);
    g_config.font_size = 14;
    g_config.padding_left = 0;
    g_config.padding_right = 0;
    g_config.padding_top = 0;
    g_config.padding_bottom = 0;
    g_config.opacity = 1.0f;
    g_config.cursor_color = 0xFFFFFF;
    g_config.cursor_shape = 0;
    g_config.cursor_blink = 0;
    g_config.cursor_blink_interval = 300;
    g_config.cursor_trail = 0;
    g_config.scrollback_lines = 10000;
    g_config.mouse_scroll_step = 3;
    g_config.selection_fg_set = 0;
    g_config.selection_bg_set = 0;
    g_config.selection_foreground = 0x000000;
    g_config.selection_background = 0xFFFFFF;
    g_config.bold_brightens_text = 1;
    copy_string(g_config.term_name, "xterm-256color", 15, 64);
    g_config.window_scale = 0.9f;
    g_config.background_image[0] = '\0';
    g_config.background_image_opacity = 1.0f;
    g_config.background_image_mode = 0;
    g_config.shell[0] = '\0';
    g_config.num_env_vars = 0;
    g_config.cursor_text_color_set = 0;
    g_config.cursor_text_color = 0x000000;
    g_config.visual_bell_duration = 0;
    g_config.cursor_stop_blinking_after = 15.0f;
    g_config.adjust_line_height = 0;
    g_config.adjust_column_width = 0;
    g_config.adjust_baseline = 0;

    const char *home = getenv("HOME");
    if (!home) { snapshot_defaults(); return; }

    char path[512];
    int h_len = 0;
    while (home[h_len] && h_len < 256) { path[h_len] = home[h_len]; h_len++; }
    const char *suffix = "/.config/termmiK/config";
    for (int i = 0; suffix[i]; i++) { path[h_len++] = suffix[i]; }
    path[h_len] = '\0';

    int fd = open(path, O_RDONLY);
    if (fd < 0) { snapshot_defaults(); return; }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size == 0) {
        close(fd);
        snapshot_defaults();
        return;
    }

    char *data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) { snapshot_defaults(); return; }

    int pos = 0;
    int line_no = 0;

    while (pos < st.st_size) {
        line_no++;
        int line_end = pos;
        while (line_end < st.st_size && data[line_end] != '\n') line_end++;

        int len = line_end - pos;
        if (len > 0 && data[line_end - 1] == '\r') len--;

        int ke = pos;
        int line_stop = pos + len;
        while (ke < line_stop && (data[ke] == ' ' || data[ke] == '\t')) ke++;

        if (ke < line_stop && data[ke] != '#') {
            int eq = ke;
            while (eq < line_stop && data[eq] != '=') eq++;

            if (eq >= line_stop || eq == ke) {
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "TermmiK: config line %d: expected key=value, line ignored\n", line_no);
                my_print(msg);
            } else {
                int key_len = eq - ke;
                int val_start = eq + 1;
                int val_len = line_stop - val_start;

                // Strip trailing inline comments: a '#' preceded by whitespace
                // starts a comment. A '#' attached to the value (hex colors like
                // #RRGGBB) is left intact.
                for (int ci = 0; ci < val_len; ci++) {
                    char pcv = data[val_start + ci - 1];
                    if (data[val_start + ci] == '#' && ci > 0 && (pcv == ' ' || pcv == '\t')) {
                        val_len = ci;
                        break;
                    }
                }
                while (val_len > 0 && (data[val_start + val_len - 1] == ' ' ||
                                       data[val_start + val_len - 1] == '\t')) {
                    val_len--;
                }

                const char *k = data + ke;
                const char *v = data + val_start;

            if (starts_with(k, "font_name", key_len)) {
                copy_string(g_config.font_name, v, val_len, 256);
            } else if (starts_with(k, "font_size", key_len)) {
                g_config.font_size = parse_int(v, val_len);
            } else if (starts_with(k, "padding_x", key_len)) {
                g_config.padding_left = g_config.padding_right = parse_int(v, val_len);
            } else if (starts_with(k, "padding_y", key_len)) {
                g_config.padding_top = g_config.padding_bottom = parse_int(v, val_len);
            } else if (starts_with(k, "padding_left", key_len)) {
                g_config.padding_left = parse_int(v, val_len);
            } else if (starts_with(k, "padding_right", key_len)) {
                g_config.padding_right = parse_int(v, val_len);
            } else if (starts_with(k, "padding_top", key_len)) {
                g_config.padding_top = parse_int(v, val_len);
            } else if (starts_with(k, "padding_bottom", key_len)) {
                g_config.padding_bottom = parse_int(v, val_len);
            } else if (starts_with(k, "opacity", key_len)) {
                g_config.opacity = parse_float(v, val_len);
            } else if (starts_with(k, "cursor_color", key_len)) {
                g_config.cursor_color = parse_hex(v, val_len);
            } else if (starts_with(k, "cursor_shape", key_len)) {
                g_config.cursor_shape = parse_int(v, val_len);
            } else if (starts_with(k, "cursor_blink_interval", key_len)) {
                g_config.cursor_blink_interval = parse_int(v, val_len);
            } else if (starts_with(k, "cursor_blink", key_len)) {
                g_config.cursor_blink = parse_int(v, val_len);
            } else if (starts_with(k, "cursor_trail", key_len)) {
                g_config.cursor_trail = parse_int(v, val_len);
            } else if (starts_with(k, "scrollback_lines", key_len)) {
                g_config.scrollback_lines = parse_int(v, val_len);
            } else if (starts_with(k, "mouse_scroll_step", key_len)) {
                g_config.mouse_scroll_step = parse_int(v, val_len);
            } else if (starts_with(k, "selection_foreground", key_len)) {
                g_config.selection_foreground = parse_hex(v, val_len);
                g_config.selection_fg_set = 1;
            } else if (starts_with(k, "selection_background", key_len)) {
                g_config.selection_background = parse_hex(v, val_len);
                g_config.selection_bg_set = 1;
            } else if (starts_with(k, "bold_brightens_text", key_len)) {
                g_config.bold_brightens_text = parse_int(v, val_len);
            } else if (starts_with(k, "term_name", key_len)) {
                copy_string(g_config.term_name, v, val_len, 64);
            } else if (starts_with(k, "window_scale", key_len)) {
                g_config.window_scale = parse_float(v, val_len);
                if (g_config.window_scale < 0.1f) g_config.window_scale = 0.1f;
                if (g_config.window_scale > 1.0f) g_config.window_scale = 1.0f;
            } else if (starts_with(k, "background_image_opacity", key_len)) {
                g_config.background_image_opacity = parse_float(v, val_len);
                if (g_config.background_image_opacity < 0.0f) g_config.background_image_opacity = 0.0f;
                if (g_config.background_image_opacity > 1.0f) g_config.background_image_opacity = 1.0f;
            } else if (starts_with(k, "background_image_mode", key_len)) {
                // stretch / center / tile (first letter is enough)
                if (val_len > 0) {
                    char m = (v[0] >= 'A' && v[0] <= 'Z') ? v[0] + 32 : v[0];
                    if (m == 'c') g_config.background_image_mode = 1;
                    else if (m == 't') g_config.background_image_mode = 2;
                    else g_config.background_image_mode = 0;
                }
            } else if (starts_with(k, "background_image", key_len)) {
                copy_string(g_config.background_image, v, val_len, 512);
                struct stat img_st;
                if (val_len > 0 && stat(g_config.background_image, &img_st) != 0) {
                    char msg[640];
                    snprintf(msg, sizeof(msg),
                             "TermmiK: config line %d: background_image file not found: %s\n",
                             line_no, g_config.background_image);
                    my_print(msg);
                }
            } else if (starts_with(k, "shell", key_len)) {
                copy_string(g_config.shell, v, val_len, 128);
            } else if (starts_with(k, "env", key_len)) {
                // env NAME=VALUE — repeatable, exported to the spawned shell
                if (val_len > 0 && val_len < 256) {
                    if (g_config.num_env_vars < 32) {
                        copy_string(g_config.env_vars[g_config.num_env_vars++], v, val_len, 256);
                    } else {
                        char msg[96];
                        snprintf(msg, sizeof(msg),
                                 "TermmiK: config line %d: too many env entries, ignored\n", line_no);
                        my_print(msg);
                    }
                }
            } else if (starts_with(k, "cursor_text_color", key_len)) {
                g_config.cursor_text_color = parse_hex(v, val_len);
                g_config.cursor_text_color_set = 1;
            } else if (starts_with(k, "visual_bell_duration", key_len)) {
                // Accept both seconds (kitty style float) and milliseconds >= 10
                float f = parse_float(v, val_len);
                g_config.visual_bell_duration = (f > 0.0f && f < 10.0f) ? (int)(f * 1000.0f) : (int)f;
                if (g_config.visual_bell_duration < 0) g_config.visual_bell_duration = 0;
            } else if (starts_with(k, "cursor_stop_blinking_after", key_len)) {
                g_config.cursor_stop_blinking_after = parse_float(v, val_len);
                if (g_config.cursor_stop_blinking_after < 0) g_config.cursor_stop_blinking_after = 0;
            } else if (starts_with(k, "adjust_line_height", key_len)) {
                g_config.adjust_line_height = parse_int(v, val_len);
            } else if (starts_with(k, "adjust_column_width", key_len)) {
                g_config.adjust_column_width = parse_int(v, val_len);
            } else if (starts_with(k, "adjust_baseline", key_len)) {
                g_config.adjust_baseline = parse_int(v, val_len);
            } else if (starts_with(k, "foreground", key_len)) {
                g_config.fg_color = parse_hex(v, val_len);
            } else if (starts_with(k, "background", key_len)) {
                g_config.bg_color = parse_hex(v, val_len);
            } else if (starts_with(k, "color", 5) && key_len <= 7) {
                int idx = parse_int(k + 5, key_len - 5);
                if (idx >= 0 && idx < 16) {
                    g_config.colors[idx] = parse_hex(v, val_len);
                }
            } else {
                // Unknown key — likely a typo or a newer config format
                char msg[128];
                int n = snprintf(msg, sizeof(msg),
                                 "TermmiK: config line %d: unknown key '", line_no);
                for (int kc = 0; kc < key_len && n < (int)sizeof(msg) - 12; kc++) {
                    msg[n++] = k[kc];
                }
                snprintf(msg + n, sizeof(msg) - n, "' ignored\n");
                my_print(msg);
            }
        }
        }
        pos = line_end + 1;
    }

    munmap(data, st.st_size);

    // Snapshot the startup palette for OSC 104/110/111 resets
    snapshot_defaults();
}
