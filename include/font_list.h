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

#ifndef FONT_LIST_H
#define FONT_LIST_H

// Minimal replacement for fontconfig: scan the standard font directories and
// match faces against a family name. No external libraries, no caches.

typedef struct {
    char path[512];   // font file
    int  face;        // face index inside the file (font collections)
    char family[96];  // family name from the font's name table
    char style[64];   // e.g. "Regular", "Bold Italic"
    int  monospace;
    int  outlines;    // has TrueType/CFF outlines (stb_truetype can render it)
    int  score;       // match quality: higher is better, 0 = fallback
} FontMatch;

// Fills `out` with up to `max` fonts, best match first. `pattern` is a
// fontconfig-style "Family:style=..." string; the family and style/weight
// hints are interpreted, other keys are ignored. An absolute file path is
// accepted too. Returns the number of entries (0 when nothing was found).
int font_list_match(const char *pattern, FontMatch *out, int max);

#endif
