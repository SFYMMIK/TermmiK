# TermmiK

TermmiK is a Linux terminal emulator built around one idea: **be as customizable as you possibly can, while staying optimized, fast, and low on resources.** Every feature — from image backgrounds to full VT compatibility — is implemented to cost as little as possible in binary size, memory, and frame time.

Written purely in C, TermmiK leverages zero-bloat graphics protocols and avoids heavy UI toolkits: no Pango, no Cairo, no GTK. Text is rasterized with `stb_truetype`, images with `stb_image`, and everything else is raw framebuffer manipulation.

## Features

- **Extreme Memory Efficiency**: Bypasses heavy UI rendering libraries (like Pango, Cairo, or GTK) in favor of raw framebuffer manipulation — full features without the framework tax.
- **Tiny, Fast Binary**: Compiled with Link-Time Optimization (LTO) and aggressive space-saving flags, resulting in a small executable that launches instantly.
- **Maximally Customizable**: Fonts, per-side margins (asymmetric padding), opacity, cursor shape and blink, selection colors, scroll step, image backgrounds, `TERM` override, initial window scale and more — all through a simple config file with line-numbered diagnostics when something's wrong, no recompiling.
- **Native Wayland & X11 Support**: Supports both major Linux display protocols natively without translation layers.
- **Native Clipboard & Selection**: Fully integrated, bloat-free Wayland (`wl_data_device`) and X11 clipboard sharing. Highlight text with your mouse and use `Ctrl+Shift+C` / `Ctrl+Shift+V` without relying on external tools like `xclip` or `wl-clipboard`. Applications can also write to the clipboard directly via `OSC 52` (tmux, nvim, opencode...), and clipboard reads negotiate the best MIME type the source offers.
- **Bracketed Paste & TUI-Ready Input**: Pastes are wrapped in bracketed-paste markers (`?2004`) when the running program requests it, so multi-line pastes land safely in shells and TUI apps. Full xterm-style key encoding for arrows, Home/End, Insert/Delete, PageUp/PageDown, `F1`–`F12`, `Shift+Tab`, and Ctrl/Alt/Shift modifier combos, including application cursor key mode (`DECCKM`) and focus reporting (`?1004`).
- **Full SGR Text Attributes**: Bold (double-struck glyphs), italic (sheared glyphs), underline, strikethrough and dim are parsed *and rendered*, alongside 16/256-color and truecolor (`38;2;R;G;B`) foreground/background, colon sub-parameter forms (`SGR 4:3`, `38:2::R:G:B`), and underline color (`SGR 58/59`). `COLORTERM=truecolor` is exported to the shell.
- **Unicode Width Model**: CJK and emoji characters occupy two cells (with correct wrap and backspace behavior), combining marks are handled gracefully, invalid UTF-8 resyncs with `U+FFFD` replacement instead of corrupting the stream.
- **Mouse Reporting & Alternate Scroll**: Mouse press/release/drag/hover events are forwarded to applications (`?1000`/`?1002`/`?1003`, with SGR pixel-exact encoding via `?1006`), and the scroll wheel drives pagers like `less` in full-screen apps (`?1007`). The alternate screen never pollutes scrollback.
- **Flawless Keyboard Polling**: Hardware-level keyboard repeating optimized out of the critical Wayland event loop using asynchronous `timerfd` interrupts.
- **Hardware-Accelerated Transparency**: Supports perfectly smooth, per-pixel alpha blending for terminal backgrounds on both Wayland and X11 without fading the text.
- **Image Backgrounds**: Render any PNG/JPEG as your terminal background — stretched (bilinear), centered or tiled — blended over the background color and fully compositing with window transparency and OSC color changes.
- **Gamma-Corrected Subpixel Emulation**: Achieves bold, crisp text rendering comparable to Freetype, but without the massive memory overhead, via mathematically optimized `stb_truetype` alpha blending.
- **Seamless Line Art**: All 128 Box Drawing characters (single, double, heavy, mixed, rounded, dashed and diagonals), Block Elements (`█▀▄░▒▓`...) and DEC scan lines are synthesized procedurally to span the full cell, so TUI frames and logos (opencode, htop, lazygit...) render perfectly connected with zero gaps — no font dependency.
- **Dynamic Resizing**: Seamless window reflowing and recalculations on resize events with zero visual artifacts.
- **Kitty Graphics Protocol Support**: Fully supports displaying high-resolution images inline via the Kitty image protocol. Handles compressed pixel data (`o=z`), chunked data transfers (`m=1`), and precise cursor-aligned image positioning, making tools like `icat` and `fastfetch` render beautifully.
- **Proper Terminal Queries**: Replies to `DSR`, `CPR`/`DECXCPR`, `DA1`/`DA2`, `XTVERSION`, `DECRQM` and kitty-keyboard queries, and answers `OSC 10`/`OSC 11` color queries so programs (termenv, bubbletea, etc.) can detect your background and pick the right theme automatically. Window title updates via `OSC 0/2` are honored. Also supported: `OSC 4`/`104` palette get/set/reset, `OSC 10`/`11` runtime color changes (live theme switching), window ops (`CSI 14/16/18 t` size reports and `CSI 8 t` resize requests), tab stops (`HTS`/`TBC`/`CHT`/`CBT`), cursor styles (`DECSCUSR`), soft reset (`DECSTR`), origin mode (`DECOM`), scroll region inserts/deletes and `REP` — verified against vim, less, nano, top and ncurses apps via the built-in test suite (`make test`, `make test-apps`).

## Build Configurations

For detailed instructions on compiling the terminal from source, installing dependencies, and applying size-optimizing compiler flags, please see [BUILD.md](./BUILD.md).



## Running

Simply run the executable from your shell:

```bash
./TermmiK
```

TermmiK will automatically query your display size and intelligently spawn an optimized terminal window exactly 10% smaller than your screen resolution.

## Configuration

TermmiK supports dynamic configuration via a flat-text config file located at `~/.config/termmiK/config`.

For a full list of configuration options, color palettes, and formatting details, please refer to [CONFIG.md](./CONFIG.md).
