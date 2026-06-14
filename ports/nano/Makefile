CC     ?= gcc
CFLAGS := -std=gnu11 -O2 -g -Wall \
           -ffreestanding -fno-stack-protector -fno-builtin -fno-pic \
           -m64 -mno-red-zone \
           -I../../userspace/libc/include \
           -Isrc \
           -Isrc/upstream/src \
           -include src/config.h
LDFLAGS := -T ../../userspace/linker.ld -nostdlib -static -no-pie \
            -Wl,-z,max-page-size=0x1000
USER_CRT := ../../build/obj/userspace/libc/src/crt0.o
USER_LIB := ../../build/user/libtnu.a

UPSTREAM := src/upstream
PATCHES := patches

NANO_SRCS := $(UPSTREAM)/src/browser.c \
             $(UPSTREAM)/src/chars.c \
             $(UPSTREAM)/src/color.c \
             $(UPSTREAM)/src/cut.c \
             $(UPSTREAM)/src/files.c \
             $(UPSTREAM)/src/global.c \
             $(UPSTREAM)/src/help.c \
             $(UPSTREAM)/src/history.c \
             $(UPSTREAM)/src/move.c \
             $(UPSTREAM)/src/nano.c \
             $(UPSTREAM)/src/prompt.c \
             $(UPSTREAM)/src/rcfile.c \
             $(UPSTREAM)/src/search.c \
             $(UPSTREAM)/src/text.c \
             $(UPSTREAM)/src/utils.c \
             $(UPSTREAM)/src/winio.c

SHIM_SRCS := src/tnu_curses.c

NANO_OBJS := $(patsubst $(UPSTREAM)/src/%.c, \
             ../../build/obj/ports/nano/upstream/%.o, $(NANO_SRCS))
SHIM_OBJ  := ../../build/obj/ports/nano/tnu_curses.o

.PHONY: all clean fetch patch

all: fetch patch ../../build/user/nano

# Clone nano from GitHub if not present
fetch: $(UPSTREAM)/src/nano.c
$(UPSTREAM)/src/nano.c:
	@if [ ! -f "$@" ]; then \
		if [ -d "$(UPSTREAM)" ] && [ -n "$$(ls -A "$(UPSTREAM)" 2>/dev/null)" ]; then \
			echo "nano: $(UPSTREAM) exists but $@ is missing"; \
			exit 1; \
		fi; \
		echo "nano: cloning madnight/nano from GitHub..."; \
		git clone --depth 1 https://github.com/madnight/nano.git $(UPSTREAM); \
	fi

# Apply patches after cloning (optional)
patch: $(UPSTREAM)/src/nano.c
	@echo "nano: patch step (no patches to apply)"

../../build/user/nano: $(NANO_OBJS) $(SHIM_OBJ) $(USER_LIB) $(USER_CRT)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(USER_CRT) $(NANO_OBJS) $(SHIM_OBJ) $(USER_LIB) -lgcc

../../build/obj/ports/nano/upstream/%.o: $(UPSTREAM)/src/%.c src/config.h src/tnu_curses.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-error -w -c $< -o $@

../../build/obj/ports/nano/tnu_curses.o: src/tnu_curses.c src/tnu_curses.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf ../../build/obj/ports/nano ../../build/user/nano
