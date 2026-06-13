CC     ?= gcc
CFLAGS := -std=gnu11 -O2 -g -Wall \
           -ffreestanding -fno-stack-protector -fno-builtin -fno-pic \
           -m64 -mno-red-zone \
           -I../../userspace/libc/include \
           -Isrc
LDFLAGS := -T ../../userspace/linker.ld -nostdlib -static -no-pie \
            -Wl,-z,max-page-size=0x1000
USER_CRT := ../../build/obj/userspace/libc/src/crt0.o
USER_LIB := ../../build/user/libtnu.a

SRCS := src/fastfetch.c
OBJS := $(patsubst src/%.c,../../build/obj/ports/fastfetch/%.o,$(SRCS))

.PHONY: all clean

all: ../../build/user/fastfetch

../../build/user/fastfetch: $(OBJS) $(USER_LIB) $(USER_CRT)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(USER_CRT) $(OBJS) $(USER_LIB) -lgcc

../../build/obj/ports/fastfetch/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf ../../build/obj/ports/fastfetch ../../build/user/fastfetch
