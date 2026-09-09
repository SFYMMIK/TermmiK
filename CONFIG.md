# Configuring TermmiK

TermmiK supports dynamic configuration via a plain text config file. There is no need to recompile the terminal after making changes to the configuration.

> This file is the **complete reference** for every option. For a guided tour with ready-to-copy examples (glassy transparency, image backgrounds, cursor trails, theming...), see [CUSTOMIZING.md](./CUSTOMIZING.md).

## Configuration Location

TermmiK automatically looks for its configuration file at the following location:

```
~/.config/termmiK/config
```

If the file does not exist, TermmiK will safely fall back to its internal default values.

## File Format

The configuration file uses a simple `key=value` format. Empty lines and lines starting with `#` are ignored, and inline comments after a value (`key=value # note`) are stripped — just keep a space before the `#`, since `#RRGGBB` hex colors are values, not comments. Spaces around the `=` sign are supported.

Example `~/.config/termmiK/config`:
```ini
# Core
font_name=monospace
font_size=14
opacity=0.8

# Background image (blended over the background color, respects opacity)
background_image=/home/you/Pictures/wallpaper.png
background_image_opacity=0.35
background_image_mode=stretch   # stretch | center | tile

# Layout
padding_x=4
padding_y=4
padding_left=8                  # per-side overrides win over padding_x/y
padding_bottom=0
scrollback_lines=10000
window_scale=0.9                # initial window size relative to the screen

# Colors (Standard 16 color palette)
foreground=#DDDDDD
background=#1E1E1E
cursor_color=#00FF00
cursor_shape=0
selection_background=#333377    # optional: fixed selection colors
selection_foreground=#FFFFFF

color0=#000000
color1=#AA0000
color2=#00AA00
# ...
```

## Configuration Options

### Layout and Font
- `font_name` (string): The font family to request from FontConfig. Defaults to `monospace`.
- `font_size` (int): The point size of the font. Defaults to `14`.
- `padding_x` (int): Shorthand that sets **both** `padding_left` and `padding_right`. Defaults to `0`.
- `padding_y` (int): Shorthand that sets **both** `padding_top` and `padding_bottom`. Defaults to `0`.
- `padding_left` / `padding_right` / `padding_top` / `padding_bottom` (int): Per-side margins in pixels between the window edge and the text grid. Use these for asymmetric layouts; when set, they override the shorthand values.
- `scrollback_lines` (int): The number of lines to retain in the scrollback buffer. Defaults to `10000`.
- `window_scale` (float): Size of the initial window relative to the screen, from `0.1` to `1.0`. Defaults to `0.9`. The size is snapped to whole cells.

### Config Diagnostics
On launch, problems in the config file are reported to stderr with the offending line number:
- unknown keys (typos, newer config formats) — reported and ignored
- lines that are neither comments, blank, nor `key=value` — reported and ignored
- `background_image` pointing at a file that does not exist — reported

A valid config prints nothing.

### Background Image
- `background_image` (string): Path to an image file (PNG/JPEG/BMP/etc.) to render behind the cells. Empty by default (plain background color).
- `background_image_opacity` (float): How strongly the image blends over the background color, from `0.0` (invisible) to `1.0` (image only). Defaults to `1.0`.
- `background_image_mode` (string): `stretch` (default) fills the window (bilinear filtered), `center` draws the image 1:1 in the middle, `tile` repeats it.

The image is composited into the background, so **window transparency keeps working**: with `opacity` below `1.0` you get the image blended over the background color at reduced window alpha, and cells painted with the default background stay see-through.

### Visuals
- `opacity` (float): The transparency of the terminal background, from `0.0` (fully transparent) to `1.0` (fully opaque). Defaults to `1.0`. Native hardware-accelerated transparency is applied only to the background, keeping text fully opaque.
- `cursor_shape` (int): The shape of the cursor. `0` block (`█`), `1` underline (`▁`), `2` bar (`|`). Defaults to `0`. (Applications can also change it at runtime via `DECSCUSR`.)
- `cursor_blink` (int): `1` makes the cursor blink while the terminal is idle. Any input resets it to solid. Defaults to `0`.
- `cursor_blink_interval` (int): Blink period in milliseconds. Defaults to `300`.
- `cursor_trail` (int): `1` enables an animated cursor trail — when the cursor jumps (prompt redraws, vim motions...), the block glides to its new position with an ease-out curve and a fading smear, carrying the character under it. Only applies to the block cursor. Defaults to `0`.

### Colors
Colors are defined using standard 6-digit hex codes. The `#` prefix is optional.
- `foreground`: Default text color. Defaults to `#AAAAAA`.
- `background`: Default terminal background color. Defaults to `#000000`.
- `cursor_color`: The color of the cursor. Defaults to `#FFFFFF`.
- `color0` through `color15`: The 16 standard ANSI colors (0-7 standard, 8-15 bright).
- `selection_foreground` / `selection_background` (optional): Fixed colors for selected text. When unset, the selection swaps the cell's foreground and background like a classic terminal.

### Behavior
- `mouse_scroll_step` (int): Lines scrolled per wheel tick. Defaults to `3`.
- `bold_brightens_text` (int): `1` (default) maps bold ANSI colors 0-7 to their bright variants 8-15. Set to `0` for themes that encode emphasis purely in the color values.
- `term_name` (string): The value exported as `TERM` to spawned programs. Defaults to `xterm-256color`. Change it only if you know the matching terminfo entry is installed.
- `shell` (string): Command to spawn instead of `$SHELL`, with arguments — e.g. `shell=zsh -l`. Empty by default.
- `env` (string): Export an environment variable to the spawned shell. Repeatable: `env EDITOR=helix`, `env TERMINAL=termmik`. Up to 32 entries.
- `visual_bell_duration` (number): Screen flash duration when a program rings the terminal bell (`\a`), in seconds (kitty-style floats like `0.15` work; values `>= 10` are treated as milliseconds). The flash fades out using the foreground color. `0` (default) disables it.
- `cursor_stop_blinking_after` (float): Seconds of terminal idleness after which the cursor stops blinking and stays solid. Defaults to `15`. `0` keeps it blinking forever.

### Font Spacing
Fine-tune the metrics computed from your font (pixel deltas, can be negative):
- `adjust_line_height` (int): Added to the cell height. Defaults to `0`.
- `adjust_column_width` (int): Added to the cell width. Defaults to `0`.
- `adjust_baseline` (int): Shifts the text baseline inside the cell. Defaults to `0`.

### Cursor Colors
- `cursor_text_color` (optional): The text color drawn on top of a block cursor. When unset, the text keeps its own background color (inverted look).

### Runtime Color Control
Programs can change colors at runtime without touching the config file: `OSC 10`/`OSC 11` set the default foreground/background (used by theme switchers), `OSC 4` reads and sets the 16-color palette, and `OSC 104`/`OSC 110`/`OSC 111` reset them back to the values from this file.
