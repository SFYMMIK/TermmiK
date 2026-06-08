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

#include "config.h"
#include "alloc.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

TermConfig g_config;

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
    g_config.colors[0] = 0x000000; g_config.colors[8] = 0x555555;
    g_config.colors[1] = 0xAA0000; g_config.colors[9] = 0xFF5555;
    g_config.colors[2] = 0x00AA00; g_config.colors[10] = 0x55FF55;
    g_config.colors[3] = 0xAA5500; g_config.colors[11] = 0xFFFF55;
    g_config.colors[4] = 0x0000AA; g_config.colors[12] = 0x5555FF;
    g_config.colors[5] = 0xAA00AA; g_config.colors[13] = 0xFF55FF;
    g_config.colors[6] = 0x00AAAA; g_config.colors[14] = 0x55FFFF;
    g_config.colors[7] = 0xAAAAAA; g_config.colors[15] = 0xFFFFFF;

    g_config.fg_color = 0xAAAAAA;
    g_config.bg_color = 0x000000;
    copy_string(g_config.font_name, "monospace", 9, 256);
    g_config.font_size = 14;
    g_config.padding_x = 0;
    g_config.padding_y = 0;
    g_config.opacity = 1.0f;
    g_config.cursor_color = 0xFFFFFF;
    g_config.cursor_shape = 0;
    g_config.scrollback_lines = 10000;

    const char *home = getenv("HOME");
    if (!home) return;

    char path[512];
    int h_len = 0;
    while (home[h_len] && h_len < 256) { path[h_len] = home[h_len]; h_len++; }
    const char *suffix = "/.config/termmiK/config";
    for (int i = 0; suffix[i]; i++) { path[h_len++] = suffix[i]; }
    path[h_len] = '\0';

    int fd = open(path, O_RDONLY);
    if (fd < 0) return;

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size == 0) {
        close(fd);
        return;
    }

    char *data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) return;

    int pos = 0;
    while (pos < st.st_size) {
        while (pos < st.st_size && (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\n' || data[pos] == '\r')) pos++;
        if (pos >= st.st_size) break;

        int line_end = pos;
        while (line_end < st.st_size && data[line_end] != '\n' && data[line_end] != '\r') line_end++;

        int eq = pos;
        while (eq < line_end && data[eq] != '=') eq++;

        if (eq < line_end && data[pos] != '#') {
            int key_len = eq - pos;
            int val_start = eq + 1;
            int val_len = line_end - val_start;

            const char *k = data + pos;
            const char *v = data + val_start;

            if (starts_with(k, "font_name", key_len)) {
                copy_string(g_config.font_name, v, val_len, 256);
            } else if (starts_with(k, "font_size", key_len)) {
                g_config.font_size = parse_int(v, val_len);
            } else if (starts_with(k, "padding_x", key_len)) {
                g_config.padding_x = parse_int(v, val_len);
            } else if (starts_with(k, "padding_y", key_len)) {
                g_config.padding_y = parse_int(v, val_len);
            } else if (starts_with(k, "opacity", key_len)) {
                g_config.opacity = parse_float(v, val_len);
            } else if (starts_with(k, "cursor_color", key_len)) {
                g_config.cursor_color = parse_hex(v, val_len);
            } else if (starts_with(k, "cursor_shape", key_len)) {
                g_config.cursor_shape = parse_int(v, val_len);
            } else if (starts_with(k, "scrollback_lines", key_len)) {
                g_config.scrollback_lines = parse_int(v, val_len);
            } else if (starts_with(k, "foreground", key_len)) {
                g_config.fg_color = parse_hex(v, val_len);
            } else if (starts_with(k, "background", key_len)) {
                g_config.bg_color = parse_hex(v, val_len);
            } else if (starts_with(k, "color", 5) && key_len <= 7) {
                int idx = parse_int(k + 5, key_len - 5);
                if (idx >= 0 && idx < 16) {
                    g_config.colors[idx] = parse_hex(v, val_len);
                }
            }
        }
        pos = line_end;
    }

    munmap(data, st.st_size);
}
