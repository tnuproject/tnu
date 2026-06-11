#include <arch/io.h>
#include <arch/serial.h>

#define COM1 0x3f8
#define SERIAL_LOOPBACK_TEST 0xae

static int serial_ready;

void serial_init(void)
{
    serial_ready = 0;

    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xc7);
    outb(COM1 + 4, 0x1e);
    outb(COM1 + 0, SERIAL_LOOPBACK_TEST);
    if (inb(COM1 + 0) != SERIAL_LOOPBACK_TEST) {
        return;
    }

    outb(COM1 + 4, 0x0b);
    serial_ready = 1;
}

static int can_transmit(void)
{
    return inb(COM1 + 5) & 0x20;
}

static int can_receive(void)
{
    return inb(COM1 + 5) & 0x01;
}

void serial_write_char(char c)
{
    if (!serial_ready) {
        return;
    }
    while (!can_transmit()) {
    }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s)
{
    while (*s) {
        serial_write_char(*s++);
    }
}

int serial_read_char(void)
{
    if (!serial_ready || !can_receive()) {
        return -1;
    }
    return inb(COM1);
}
