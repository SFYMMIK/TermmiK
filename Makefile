CC = gcc
CFLAGS = -fno-builtin -Wall -Wextra -Wno-unused-parameter -Os -ffunction-sections -fdata-sections -Iinclude -flto
LDFLAGS = -lm -lfontconfig -Wl,--gc-sections -flto

SRCS = src/main.c src/pty.c src/vt_parser.c src/render.c src/alloc.c src/config.c

ifndef DISABLE_X11
    CFLAGS += -D_HAS_X11
    LDFLAGS += -lX11 -lXrandr -lXext
    SRCS += src/x11_backend.c
endif

ifndef DISABLE_WAYLAND
    CFLAGS += -D_HAS_WAYLAND
    LDFLAGS += -lwayland-client -lxkbcommon
    SRCS += src/wayland_backend.c src/xdg-shell-protocol.c src/xdg-decoration-protocol.c
endif

OBJS = $(patsubst src/%.c,build/%.o,$(SRCS))
EXEC = TermmiK

all: build_dir $(EXEC)

build_dir:
	mkdir -p build

$(EXEC): $(OBJS)
	$(CC) $(OBJS) -o $(EXEC) $(LDFLAGS)
	strip $(EXEC)

build/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build $(EXEC)
