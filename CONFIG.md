# Configuring TermmiK

TermmiK supports dynamic configuration via a plain text config file. There is no need to recompile the terminal after making changes to the configuration.

## Configuration Location

TermmiK automatically looks for its configuration file at the following location:

```
~/.config/termmiK/config
```

If the file does not exist, TermmiK will safely fall back to its internal default values.

## File Format

The configuration file uses a simple `key=value` format. Empty lines and lines starting with `#` are ignored. Spaces around the `=` sign are supported.

Example `~/.config/termmiK/config`:
```ini
# Core
font_name=monospace
font_size=14
opacity=0.8

# Layout
padding_x=4
padding_y=4
scrollback_lines=10000

# Colors (Standard 16 color palette)
foreground=#DDDDDD
background=#1E1E1E
cursor_color=#00FF00
cursor_shape=0

color0=#000000
color1=#AA0000
color2=#00AA00
# ...
```

## Configuration Options

### Layout and Font
- `font_name` (string): The font family to request from FontConfig. Defaults to `monospace`.
- `font_size` (int): The point size of the font. Defaults to `14`.
- `padding_x` (int): The horizontal padding in pixels between the terminal border and the text grid. Defaults to `0`.
- `padding_y` (int): The vertical padding in pixels between the terminal border and the text grid. Defaults to `0`.
- `scrollback_lines` (int): The number of lines to retain in the scrollback buffer. Defaults to `10000`.

### Visuals
- `opacity` (float): The transparency of the terminal background, from `0.0` (fully transparent) to `1.0` (fully opaque). Defaults to `1.0`. Native hardware-accelerated transparency is applied only to the background, keeping text fully opaque.
- `cursor_shape` (int): The shape of the cursor. `0` represents a block cursor. Defaults to `0`.

### Colors
Colors are defined using standard 6-digit hex codes. The `#` prefix is optional.
- `foreground`: Default text color. Defaults to `#AAAAAA`.
- `background`: Default terminal background color. Defaults to `#000000`.
- `cursor_color`: The color of the cursor. Defaults to `#FFFFFF`.
- `color0` through `color15`: The 16 standard ANSI colors (0-7 standard, 8-15 bright).
