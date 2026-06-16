CC     ?= gcc
CFLAGS := -std=gnu11 -O2 -g \
           -ffreestanding -fno-stack-protector -fno-builtin -fno-pic \
           -m64 -mno-red-zone \
           -I../../userspace/libc/include \
           -I../../kernel/include \
           -Isrc \
           -Isrc/doomgeneric/doomgeneric
LDFLAGS := -T ../../userspace/linker.ld -nostdlib -static -no-pie \
            -Wl,-z,max-page-size=0x1000
USER_CRT := ../../build/obj/userspace/libc/src/crt0.o
USER_LIB := ../../build/user/libtnu.a

UPSTREAM := src/doomgeneric/doomgeneric

# All doom source files, excluding platform backends (sdl/allegro/win/xlib etc.)
DOOM_SRCS := $(filter-out \
    $(UPSTREAM)/doomgeneric_sdl.c \
    $(UPSTREAM)/doomgeneric_allegro.c \
    $(UPSTREAM)/doomgeneric_win.c \
    $(UPSTREAM)/doomgeneric_xlib.c \
    $(UPSTREAM)/doomgeneric_emscripten.c \
    $(UPSTREAM)/doomgeneric_soso.c \
    $(UPSTREAM)/doomgeneric_sosox.c \
    $(UPSTREAM)/doomgeneric_linuxvt.c \
    $(UPSTREAM)/mus2mid.c \
    $(UPSTREAM)/i_allegromusic.c \
    $(UPSTREAM)/i_allegrosound.c \
    $(UPSTREAM)/i_sdlmusic.c \
    $(UPSTREAM)/i_sdlsound.c \
    $(UPSTREAM)/gusconf.c \
    $(UPSTREAM)/icon.c, \
    $(wildcard $(UPSTREAM)/*.c))

# Ensure core game entrypoints are always linked.
# doomgeneric/doomgeneric.c and d_main.c define the actual engine entrypoints
# (doomgeneric_Create / doomgeneric_Tick / DG_ScreenBuffer). Do not add them
# if they are already included by the upstream wildcard above.
# (The filter-out list below already excludes several backends; it should still
# keep the core files.)

TNU_SRC  := src/doomgeneric_tnu.c

DOOM_OBJS := $(patsubst $(UPSTREAM)/%.c, \
             ../../build/obj/ports/doom/upstream/%.o, $(DOOM_SRCS))
TNU_OBJ   := ../../build/obj/ports/doom/doomgeneric_tnu.o

.PHONY: all build clean fetch

all: fetch
	$(MAKE) build

build: ../../build/user/doom

fetch: $(UPSTREAM)/doomgeneric.h
$(UPSTREAM)/doomgeneric.h:
	@if [ ! -f "$@" ]; then \
		if [ -d "src/doomgeneric" ] && [ -n "$$(ls -A src/doomgeneric 2>/dev/null)" ]; then \
			echo "doom: src/doomgeneric exists but $@ is missing"; \
			exit 1; \
		fi; \
		echo "doom: cloning ozkl/doomgeneric..."; \
		git clone --depth 1 https://github.com/ozkl/doomgeneric.git src/doomgeneric; \
	fi

../../build/user/doom: $(DOOM_OBJS) $(TNU_OBJ) $(USER_LIB) $(USER_CRT)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(USER_CRT) $(DOOM_OBJS) $(TNU_OBJ) $(USER_LIB) -lgcc

../../build/obj/ports/doom/upstream/%.o: $(UPSTREAM)/%.c src/config.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-error -w -c $< -o $@

../../build/obj/ports/doom/doomgeneric_tnu.o: src/doomgeneric_tnu.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf ../../build/obj/ports/doom ../../build/user/doom
