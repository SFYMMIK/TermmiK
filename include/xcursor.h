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

#ifndef XCURSOR_H
#define XCURSOR_H

// Minimal Xcursor theme reader. libwayland-cursor was only used to turn an
// Xcursor theme into a pointer image; this does the same without the extra
// library: find the theme file, parse the Xcursor container and hand back
// the ARGB image closest to the requested size.

typedef struct {
    unsigned char *pixels; // ARGB8888, w*h*4 bytes (malloc'd by the loader)
    int w, h;
    int hot_x, hot_y;
} XCursorImage;

// Searches the standard XCursor theme directories for `name` (e.g.
// "left_ptr") inside `theme`, following the theme's Inherits= chain. Picks
// the image closest to the requested nominal `size` (0 = best available).
// Returns 1 on success; the caller frees with xcursor_free().
int xcursor_load(const char *theme, int size, const char *name, XCursorImage *out);

void xcursor_free(XCursorImage *img);

#endif
