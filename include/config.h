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

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

typedef struct {
    uint32_t colors[16];
    uint32_t fg_color;
    uint32_t bg_color;
    char font_name[256];
    int font_size;
    int padding_left;
    int padding_right;
    int padding_top;
    int padding_bottom;
    float opacity;
    uint32_t cursor_color;
    int cursor_shape; // 0=block, 1=underline, 2=bar
    int cursor_blink; // 0=hidden, 1=blinking, 2=steady always visible
    int cursor_trail;           // 0/1 — animated cursor trail on cursor jumps
    int scrollback_lines;
    int mouse_scroll_step;
    int selection_fg_set;
    int selection_bg_set;
    uint32_t selection_foreground;
    uint32_t selection_background;
    int bold_brightens_text; // 0/1 — bold maps ANSI 0-7 to the bright palette
    char term_name[64];      // TERM value exported to the shell
    float window_scale;      // initial window size relative to the screen
    char background_image[512];
    float background_image_opacity; // blend of image over the background color
    int background_image_mode;      // 0=stretch, 1=center, 2=tile

    // Kitty-style extras
    char shell[128];            // command to spawn (empty = $SHELL)
    char env_vars[32][256];     // NAME=VALUE pairs exported to the child
    int num_env_vars;
    int cursor_text_color_set;
    uint32_t cursor_text_color; // text color under a block cursor
    int visual_bell_duration;   // ms of screen flash on BEL (0 = off)
    float cursor_stop_blinking_after; // seconds of idleness before blink stops
    int adjust_line_height;     // pixel deltas applied to the cell metrics
    int adjust_column_width;
    int adjust_baseline;
} TermConfig;

extern TermConfig g_config;

void config_load(void);
void config_reset_palette(int idx);      // OSC 104
void config_reset_default_colors(void);  // OSC 110/111

#endif
