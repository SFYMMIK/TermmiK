# Configuring TermmiK

TermmiK supports dynamic configuration via a plain text config file. There is no need to recompile the terminal after making changes to the configuration.

> This file is the **complete reference** for every option. For a guided tour with ready-to-copy examples (glassy transparency, image backgrounds, theming...), see [CUSTOMIZING.md](./CUSTOMIZING.md).

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
scrollback_lines=1000
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
- `font_name` (string): The font family to use. Defaults to `monospace`. Family names are matched by TermmiK's built-in font finder against the fonts installed in the standard directories (`~/.local/share/fonts`, `~/.fonts`, `/usr/share/fonts`, ...); an absolute path to a `.ttf`/`.otf` file also works. Style hints like `JetBrains Mono:style=Bold` are honored; other fontconfig-style keys are ignored.
- `font_size` (int): The point size of the font. Defaults to `14`. The configured value is what `Ctrl+Shift+0` resets to after runtime zooming (`Ctrl+Shift+=`/`-`, or Ctrl+mouse-wheel).
- `padding_x` (int): Shorthand that sets **both** `padding_left` and `padding_right`. Defaults to `0`.
- `padding_y` (int): Shorthand that sets **both** `padding_top` and `padding_bottom`. Defaults to `0`.
- `padding_left` / `padding_right` / `padding_top` / `padding_bottom` (int): Per-side margins in pixels between the window edge and the text grid. Use these for asymmetric layouts; when set, they override the shorthand values.
- `scrollback_lines` (int): The number of lines to retain in the scrollback buffer. Defaults to `1000`, which keeps memory low; each retained line costs roughly 3.2 KB at 200 columns, so `10000` is about 32 MB.
- `window_scale` (float): Size of the initial window relative to the screen, from `0.1` to `1.0`. Defaults to `0.9`. The size is snapped to whole cells.

### Config Diagnostics
On launch, problems in the config file are reported to stderr with the offending line number:
- unknown keys (typos, newer config formats) — reported and ignored
- lines that are neither comments, blank, nor `key=value` — reported and ignored
- `background_image` pointing at a file that does not exist — reported
- removed options (e.g. `cursor_trail`) — reported and ignored

A valid config prints nothing.

### Background Image
- `background_image` (string): Path to an image file (PNG/JPEG/BMP/etc.) to render behind the cells. Empty by default (plain background color).
- `background_image_opacity` (float): How strongly the image blends over the background color, from `0.0` (invisible) to `1.0` (image only). Defaults to `1.0`.
- `background_image_mode` (string): `stretch` (default) fills the window (bilinear filtered), `center` draws the image 1:1 in the middle, `tile` repeats it.

The image is composited into the background, so **window transparency keeps working**: with `opacity` below `1.0` you get the image blended over the background color at reduced window alpha, and cells painted with the default background stay see-through.

### Visuals
- `gpu` (int/string): Rendering backend. `0`/`off` (default) uses the built-in CPU renderer; `1`/`auto` enables the experimental EGL/GLES2 GPU renderer when the driver provides a context (silently falls back to CPU otherwise). GPU mode keeps text and images on the GPU — uploaded images release their CPU copy — but is still being hardened against compositor interactions, so it is opt-in for now.
- `frame_rate` (int/string): Maximum frames the terminal presents per second. Defaults to `0` — as fast as output and the compositor allow (on Wayland the compositor already paces to the display refresh). Set e.g. `30`/`60`/`144` to coalesce fast-writing animations: output is still parsed at full speed, but the screen updates at most that often, which saves CPU and battery on low-end machines or on X11 (which has no vsync) when apps like `cava` or `cmatrix` animate faster than you need. Lower values add up to `1000/frame_rate` ms of presentation delay to typing echo and selection dragging, so values below ~24 are not recommended for interactive work; `off`/`unlimited`/`max` are accepted as aliases for `0`.
- `opacity` (float): The transparency of the terminal background, from `0.0` (fully transparent) to `1.0` (fully opaque). Defaults to `1.0`. Native hardware-accelerated transparency is applied only to the background, keeping text fully opaque.
- `cursor_shape` (int): The shape of the cursor. `0` block (`█`), `1` underline (`▁`), `2` bar (`|`). Defaults to `0`. (Applications can also change it at runtime via `DECSCUSR`.)
- `cursor_blink` (int): `0` — no cursor at all (overrides every application). `1` — the cursor blinks while the terminal is idle (fixed 300ms cadence); any input makes it solid. `2` — a steady, non-blinking cursor. Modes `1` and `2` both honor the running application's cursor hide request (`?25l`), so full-screen programs like cava, vim or less never get a stray block cursor drawn over their output. Defaults to `2`.

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

### Typing Sounds

Removed. `key_sound`, `key_sound_enabled` and `key_sound_volume` are no longer
supported; old config files using them get a line-numbered diagnostic on
launch and the options are ignored.

### Font Spacing
Fine-tune the metrics computed from your font (pixel deltas, can be negative):
- `adjust_line_height` (int): Added to the cell height. Defaults to `0`.
- `adjust_column_width` (int): Added to the cell width. Defaults to `0`.
- `adjust_baseline` (int): Shifts the text baseline inside the cell. Defaults to `0`.

### Cursor Colors
- `cursor_text_color` (optional): The text color drawn on top of a block cursor. When unset, the text keeps its own background color (inverted look).

### Runtime Color Control
Programs can change colors at runtime without touching the config file: `OSC 10`/`OSC 11` set the default foreground/background (used by theme switchers), `OSC 4` reads and sets the 16-color palette, and `OSC 104`/`OSC 110`/`OSC 111` reset them back to the values from this file.
