#ifndef TNU_ARCH_KEYBOARD_H
#define TNU_ARCH_KEYBOARD_H

void keyboard_init(void);
void keyboard_handle_irq(void);
int keyboard_try_getchar(void);
int keyboard_getchar(void);
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

#endif
