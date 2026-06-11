#ifndef TNU_ARCH_SERIAL_H
#define TNU_ARCH_SERIAL_H

void serial_init(void);
void serial_write_char(char c);
void serial_write(const char *s);
int serial_read_char(void);

#endif
