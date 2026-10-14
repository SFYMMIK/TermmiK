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

/*
 * Minimal Xcursor theme reader — replaces libwayland-cursor.
 *
 * A Wayland client owns its pointer image, so TermmiK only needed a way to
 * turn the user's cursor theme into one. The Xcursor file format is a small
 * container of ARGB images (plus comments); this reads it directly and picks
 * the image closest to the requested size. No external library.
 */

#include "xcursor.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define XCURSOR_MAGIC       0x72756358u // "Xcur"
#define XCURSOR_IMAGE_TYPE  0xfffd0002u
#define XCURSOR_COMMENT_TYPE 0xfffe0001u

// Xcursor files are little-endian on disk (unlike TrueType).
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// Parse an Xcursor file and return the image closest to `want_size`.
static int parse_xcursor(const char *path, int want_size, XCursorImage *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 16) { close(fd); return 0; }
    size_t len = (size_t)st.st_size;
    unsigned char *buf = malloc(len);
    if (!buf) { close(fd); return 0; }
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != len) { free(buf); return 0; }

    int ok = 0;
    if (rd32(buf) != XCURSOR_MAGIC) goto done;
    uint32_t ntoc = rd32(buf + 12);
    if (ntoc > 4096) ntoc = 4096; // sanity
    if (16u + ntoc * 12u > len) goto done;

    int best_diff = 1 << 30;
    for (uint32_t t = 0; t < ntoc; t++) {
        const unsigned char *toc = buf + 16 + t * 12u;
        uint32_t type = rd32(toc);
        uint32_t nominal = rd32(toc + 4);
        uint32_t pos = rd32(toc + 8);
        if (type != XCURSOR_IMAGE_TYPE) continue;
        if (pos + 36u > len) continue;

        const unsigned char *img = buf + pos;
        uint32_t hdr_size = rd32(img);
        uint32_t img_type = rd32(img + 4);
        if (hdr_size < 36 || img_type != XCURSOR_IMAGE_TYPE) continue;
        if (pos + hdr_size > len) continue;
        uint32_t w = rd32(img + 16);
        uint32_t h = rd32(img + 20);
        uint32_t xhot = rd32(img + 24);
        uint32_t yhot = rd32(img + 28);
        if (w == 0 || h == 0 || w > 1024 || h > 1024) continue;
        if (pos + hdr_size + (size_t)w * h * 4u > len) continue;

        int sz = (int)nominal;
        int diff = want_size > 0 ? abs(sz - want_size) : (int)sz * -1;
        if (diff > best_diff) continue;

        unsigned char *pixels = malloc((size_t)w * h * 4u);
        if (!pixels) continue;
        memcpy(pixels, img + hdr_size, (size_t)w * h * 4u);

        free(out->pixels);
        out->pixels = pixels;
        out->w = (int)w;
        out->h = (int)h;
        out->hot_x = xhot <= w ? (int)xhot : 0;
        out->hot_y = yhot <= h ? (int)yhot : 0;
        best_diff = diff;
        ok = 1;
    }

done:
    free(buf);
    return ok;
}

// Read "Inherits=" from <theme_dir>/index.theme (first match only).
static int theme_inherits(const char *theme_dir, char *out, size_t cap) {
    char path[600];
    snprintf(path, sizeof(path), "%s/index.theme", theme_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "Inherits", 8) != 0) continue;
        p += 8;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        char *end = p + strlen(p);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
        *end = '\0';
        if (*p) {
            snprintf(out, cap, "%s", p);
            found = 1;
        }
        break;
    }
    fclose(f);
    return found;
}

// Try one theme name in one icon root. Follows Inherits= chains.
static int try_theme(const char *root, const char *theme, const char *name,
                     int size, XCursorImage *out, int depth) {
    if (!theme || !*theme || depth > 6) return 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/cursors/%s", root, theme, name);
    if (!file_exists(path)) {
        // Some legacy layouts put the cursors directly in the theme dir.
        snprintf(path, sizeof(path), "%s/%s/%s", root, theme, name);
        if (!file_exists(path)) {
            char inherit[256];
            char theme_dir[512];
            snprintf(theme_dir, sizeof(theme_dir), "%s/%s", root, theme);
            if (theme_inherits(theme_dir, inherit, sizeof(inherit))) {
                // Inherits is a colon-separated fallback list.
                char *save = NULL;
                for (char *p = strtok_r(inherit, ":", &save); p; p = strtok_r(NULL, ":", &save)) {
                    if (try_theme(root, p, name, size, out, depth + 1)) return 1;
                }
            }
            return 0;
        }
    }
    return parse_xcursor(path, size, out);
}

int xcursor_load(const char *theme, int size, const char *name, XCursorImage *out) {
    if (!out) return 0;
    out->pixels = NULL;
    out->w = out->h = out->hot_x = out->hot_y = 0;
    if (!name || !*name) name = "left_ptr";
    if (!theme || !*theme) theme = "default";

    // Icon roots, in priority order. XCURSOR_PATH overrides the defaults.
    char roots[16][512];
    int nroots = 0;
    const char *xcpath = getenv("XCURSOR_PATH");
    if (xcpath && *xcpath) {
        char tmp[2048];
        snprintf(tmp, sizeof(tmp), "%s", xcpath);
        char *save = NULL;
        for (char *p = strtok_r(tmp, ":", &save); p && nroots < 16; p = strtok_r(NULL, ":", &save)) {
            snprintf(roots[nroots++], 512, "%s", p);
        }
    } else {
        char path[512];
        const char *home = getenv("HOME");
        if (home && *home) {
            snprintf(path, sizeof(path), "%s/.icons", home);
            if (nroots < 16) snprintf(roots[nroots++], 512, "%s", path);
            snprintf(path, sizeof(path), "%s/.local/share/icons", home);
            if (nroots < 16) snprintf(roots[nroots++], 512, "%s", path);
        }
        const char *xdg_dirs = getenv("XDG_DATA_DIRS");
        if (!xdg_dirs || !*xdg_dirs) xdg_dirs = "/usr/local/share:/usr/share";
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s", xdg_dirs);
        char *save = NULL;
        for (char *p = strtok_r(tmp, ":", &save); p && nroots < 15; p = strtok_r(NULL, ":", &save)) {
            snprintf(roots[nroots++], 512, "%s/icons", p);
        }
        if (nroots < 16) snprintf(roots[nroots++], 512, "/usr/share/pixmaps");
    }

    for (int i = 0; i < nroots; i++) {
        if (try_theme(roots[i], theme, name, size, out, 0)) return 1;
    }
    return 0;
}

void xcursor_free(XCursorImage *img) {
    if (!img) return;
    free(img->pixels);
    img->pixels = NULL;
    img->w = img->h = 0;
}
