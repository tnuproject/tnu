#ifndef TNU_ARCH_KEYBOARD_H
#define TNU_ARCH_KEYBOARD_H

#include <tnu/types.h>

void keyboard_init(void);
void keyboard_handle_irq(void);
int keyboard_try_getchar(void);
int keyboard_getchar(void);
bool keyboard_input_available(void);
bool keyboard_consume_interrupt(void);
void keyboard_ack_interrupt(void);
int keyboard_set_layout(const char *name);
const char *keyboard_current_layout(void);
const char *keyboard_available_layouts(void);

#define KEY_UP     0x101
#define KEY_DOWN   0x102
#define KEY_LEFT   0x103
#define KEY_RIGHT  0x104
#define KEY_HOME   0x105
#define KEY_END    0x106
#define KEY_DELETE 0x107
#define KEY_CTRL_C 0x108
#define KEY_COPY   0x109
#define KEY_PASTE  0x10a
#define KEY_SHIFT_LEFT  0x10b
#define KEY_SHIFT_RIGHT 0x10c
#define KEY_TTY1   0x10d
#define KEY_TTY2   0x10e
#define KEY_TTY3   0x10f
#define KEY_TTY4   0x110
#define KEY_TTY5   0x111
#define KEY_TTY6   0x112

#endif
