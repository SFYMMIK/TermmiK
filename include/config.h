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

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

typedef struct {
    uint32_t colors[16];
    uint32_t fg_color;
    uint32_t bg_color;
    char font_name[256];
    int font_size;
    int padding_x;
    int padding_y;
    float opacity;
    uint32_t cursor_color;
    int cursor_shape; // 0=block, 1=underline, 2=bar
    int scrollback_lines;
} TermConfig;

extern TermConfig g_config;

void config_load(void);

#endif
