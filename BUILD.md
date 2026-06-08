# Building TermmiK

TermmiK is designed to be extremely easy to compile from source. Since it does not rely on massive GUI toolkits like GTK or Qt, the dependencies are very minimal.

## Dependencies

Before compiling, ensure you have a C compiler (`gcc` or `clang`), `make`, and the basic display server headers installed on your system.

### Ubuntu / Debian
```bash
# Core build tools, musl libc, and font rendering
sudo apt install build-essential musl-tools libfontconfig1-dev

# X11 Support (Optional but default)
sudo apt install libx11-dev libxext-dev libxrandr-dev

# Wayland Support (Optional but default)
sudo apt install libwayland-dev libxkbcommon-dev
```

### Arch Linux
```bash
# Core build tools, musl libc, and font rendering
sudo pacman -S base-devel musl fontconfig

# X11 Support
sudo pacman -S libx11 libxext libxrandr

# Wayland Support
sudo pacman -S wayland libxkbcommon
```

## Compilation

By default, TermmiK compiles with support for **both** X11 and Wayland. It automatically detects the correct protocol at runtime.

To build the default binary with standard GCC:
```bash
make
```

### Static Linking / Musl libc (Recommended for Minimal Bloat)
TermmiK was architected to be incredibly lightweight and specifically targets `musl libc` for minimal memory overhead.

To compile a fully static binary using `musl`:
```bash
make CC="musl-gcc -static"
```

> [!WARNING]
> Building a **fully static** musl binary requires that all of your system graphics libraries (`libX11`, `libwayland-client`, `libfontconfig`) are *also* compiled against musl libc. On standard GNU/Linux distributions (like Ubuntu or Arch), these libraries are dynamically linked against `glibc` by default. To successfully compile a 100% statically linked musl binary without linker errors, it is highly recommended to build TermmiK on a native musl distribution like **Alpine Linux** or **Void Linux (musl)**.

### Advanced Compilation Flags

You can optimize the binary size even further by strictly compiling only the backend you intend to use.

**Compile for Wayland ONLY (Disables X11 support):**
```bash
make DISABLE_X11=1
```

**Compile for X11 ONLY (Disables Wayland support):**
```bash
make DISABLE_WAYLAND=1
```

**Clean build directory:**
If you want to switch compilation targets or rebuild from scratch, always run:
```bash
make clean
```

## Installation
TermmiK produces a single, heavily-optimized binary.
To install it, simply move the executable to a directory in your PATH:
```bash
sudo cp TermmiK /usr/local/bin/termmiK
```
