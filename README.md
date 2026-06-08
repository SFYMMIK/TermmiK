# TermmiK

TermmiK is a terminal emulator made with the intentions of it being very minimal and lightweight in both size and memory usage, while still maintaining (some) customizability.

Written purely in C, TermmiK leverages zero-bloat graphics protocols and avoids heavy UI toolkits.

## Features

- **Extreme Memory Efficiency**: Bypasses heavy UI rendering libraries (like Pango, Cairo, or GTK) in favor of raw framebuffer manipulation.
- **Microscopic Binary**: Compiled with Link-Time Optimization (LTO) and aggressive space-saving flags, resulting in a tiny executable.
- **Native Wayland & X11 Support**: Supports both major Linux display protocols natively without translation layers.
- **Hardware-Accelerated Transparency**: Supports perfectly smooth, per-pixel alpha blending for terminal backgrounds on both Wayland and X11 without fading the text.
- **Customizable**: Tweak fonts, padding, colors, opacity, and cursor styles through a simple configuration without recompiling.
- **Gamma-Corrected Subpixel Emulation**: Achieves bold, crisp text rendering comparable to Freetype, but without the massive memory overhead, via mathematically optimized `stb_truetype` alpha blending.
- **Dynamic Resizing**: Seamless window reflowing and recalculations on resize events with zero visual artifacts.

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
