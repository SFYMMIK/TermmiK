# Customizing TermmiK

TermmiK is built to be customizable **without giving up speed or a small footprint** — every option below is handled by the same tight C renderer, costs nothing when unused, and needs no recompile. This page shows what you can do; the complete option reference lives in [CONFIG.md](./CONFIG.md).

All options go in `~/.config/termmiK/config` (`key=value`, one per line). If something's wrong, TermmiK prints the exact line number on launch.

---

## 1. The look: fonts, spacing, colors

```ini
font_name=JetBrains Mono
font_size=14

# Fine-tune the grid without touching the font (pixel deltas, negatives ok)
adjust_line_height=2
adjust_column_width=1
adjust_baseline=1

# Full 16-color palette + defaults (hex, '#' optional)
foreground=#DDDDDD
background=#101014
color0=#000000
color8=#767676
cursor_color=#00FF00
```

Fonts are rasterized in-process (`stb_truetype`) — no fontconfig rendering stack, no Pango/Cairo. Box drawing, block elements and DEC line art are **synthesized procedurally** to fill the whole cell, so TUI frames connect perfectly with any font.

## 2. The cursor: block, bar, blink, glide

```ini
# Shape: 0 = block (█), 1 = underline (▁), 2 = bar (|)
cursor_shape=2

# 0 = no cursor at all, 1 = blinking (fixed 300ms cadence), 2 = steady
cursor_blink=1

# While blinking: go solid after N seconds of idleness
cursor_stop_blinking_after=15

# Animated trail: the cursor glides to its new position with a fading trail
cursor_trail=1

# Text color under a block cursor (otherwise inverted)
cursor_text_color=#101014
```

Apps can restyle the cursor at runtime too — vim's insert-mode bar, tmux shapes, all via `DECSCUSR`.

## 3. Glass: transparency & image backgrounds

```ini
# Per-pixel window transparency — text stays crisp
opacity=0.7

# ...or put a picture behind your shell
background_image=/home/you/Pictures/wall.png
background_image_opacity=0.5    # blend over the background color
background_image_mode=stretch   # stretch | center | tile
```

The image is pre-composited once — per-frame cost is a memory copy — and it plays perfectly with transparency: at `opacity=0.7` the whole scene sits at 70% window alpha. Programs can even re-theme at runtime via `OSC 11` and the image re-blends live.

## 4. Margins & window geometry

```ini
padding_x=8                     # sets left+right...
padding_y=8                     # ...and top+bottom
padding_left=16                 # per-side overrides for asymmetric layouts
padding_bottom=0

window_scale=0.9                # initial window size vs. the screen
```

## 5. Scroll, bell & typing sounds

```ini
scrollback_lines=10000          # history depth
mouse_scroll_step=5             # lines per wheel tick
visual_bell_duration=0.15       # screen flash when a program rings the bell

# Mechanical-style typing sounds
key_sound=default               # or a path to your own 16-bit PCM .wav
key_sound_volume=0.5
```

## 6. What runs inside

```ini
shell=zsh -l                    # command to spawn (default: $SHELL)
env EDITOR=helix                # repeatable environment variables
env TERMINAL=termmik
term_name=xterm-256color        # the exported TERM
bold_brightens_text=1           # bold maps to the bright palette
```

## 7. Selection colors

```ini
# Classic terminals invert on selection; you can pin exact colors instead
selection_foreground=#101010
selection_background=#FFCC00
```

## 8. Runtime theming (no config edit needed)

Programs control TermmiK live:

- `OSC 4` — read or set any palette entry
- `OSC 10` / `OSC 11` — query or change default foreground/background (this is how termenv/bubbletea-based tools pick themes)
- `OSC 104` / `OSC 110` / `OSC 111` — reset palette and defaults to your config values
- `OSC 52` — copy to the system clipboard (nvim, tmux, ...)
- `DECSCUSR` — cursor shape per-mode
- `CSI 8 t` — resize the window from a script

Try it live: `printf '\033]11;#1a1b26\033\\'` retints the background instantly.

---

## Full reference

Every option, default and unit is documented in [CONFIG.md](./CONFIG.md), including the line-numbered diagnostics TermmiK prints when your config has a mistake.
