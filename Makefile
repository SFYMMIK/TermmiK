# TermmiK
# Copyright (C) 2026 SfymmiK
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

CC = gcc
CFLAGS = -fno-builtin -Wall -Wextra -Wno-unused-parameter -O2 -g -ffunction-sections -fdata-sections -Iinclude
LDFLAGS = -lm -lfontconfig -Wl,--gc-sections -flto 

SRCS = src/main.c src/pty.c src/vt_parser.c src/render.c src/alloc.c src/config.c

ifndef DISABLE_X11
    CFLAGS += -D_HAS_X11
    LDFLAGS += -lX11 -lXrandr -lXext
    SRCS += src/x11_backend.c
endif

ifndef DISABLE_WAYLAND
    CFLAGS += -D_HAS_WAYLAND
    LDFLAGS += -lwayland-client -lwayland-cursor -lxkbcommon
    SRCS += src/wayland_backend.c src/xdg-shell-protocol.c src/xdg-decoration-protocol.c
endif

OBJS = $(patsubst src/%.c,build/%.o,$(SRCS))
EXEC = TermmiK

# Standard installation paths
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
DATADIR = $(PREFIX)/share
DESKTOPDIR = $(DATADIR)/applications
ICONDIR = $(DATADIR)/icons/hicolor
ICON_SIZES = 16 24 32 48 64 128 256 512

all: build_dir $(EXEC)

# VT parser test harness (no display needed)
test: build_dir build/test_parser.o build/vt_parser.o build/config.o build/alloc.o
	$(CC) build/test_parser.o build/vt_parser.o build/config.o build/alloc.o -o build/test_parser -lm
	./build/test_parser

# Integration test: run real TUI apps through the parser on a PTY
test-apps: build_dir build/test_apps.o build/vt_parser.o build/config.o build/alloc.o
	$(CC) build/test_apps.o build/vt_parser.o build/config.o build/alloc.o -o build/test_apps -lm
	./build/test_apps

build/test_parser.o: tests/test_parser.c
	$(CC) $(CFLAGS) -c $< -o $@

build/test_apps.o: tests/test_apps.c
	$(CC) $(CFLAGS) -c $< -o $@

build_dir:
	mkdir -p build

$(EXEC): $(OBJS)
	$(CC) $(OBJS) -o $(EXEC) $(LDFLAGS)
	strip $(EXEC)

build/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build $(EXEC)

# Create the bin directory if it doesn't exist, then copy the binary
install: $(EXEC)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(DESKTOPDIR)
	install -m 755 $(EXEC) $(DESTDIR)$(BINDIR)/termmik
	install -m 644 termmik.desktop $(DESTDIR)$(DESKTOPDIR)/termmik.desktop
	@for size in $(ICON_SIZES); do \
		install -d $(DESTDIR)$(ICONDIR)/$${size}x$${size}/apps; \
		install -m 644 icons/termmik-$${size}.png $(DESTDIR)$(ICONDIR)/$${size}x$${size}/apps/termmik.png; \
	done

# Remove the binary
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/termmik $(DESTDIR)$(DESKTOPDIR)/termmik.desktop
	@for size in $(ICON_SIZES); do \
		rm -f $(DESTDIR)$(ICONDIR)/$${size}x$${size}/apps/termmik.png; \
	done

.PHONY: all build_dir clean install uninstall test