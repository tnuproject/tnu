#include <arch/io.h>
#include <arch/vga.h>

#define VGA_AC_INDEX 0x3c0
#define VGA_AC_WRITE 0x3c0
#define VGA_AC_READ 0x3c1
#define VGA_MISC_WRITE 0x3c2
#define VGA_SEQ_INDEX 0x3c4
#define VGA_SEQ_DATA 0x3c5
#define VGA_GC_INDEX 0x3ce
#define VGA_GC_DATA 0x3cf
#define VGA_CRTC_INDEX 0x3d4
#define VGA_CRTC_DATA 0x3d5
#define VGA_INSTAT_READ 0x3da

/* Standard VGA mode 3: 80x25 color text at physical 0xb8000. */
static const uint8_t vga_seq_regs[5] = {
    0x03, 0x00, 0x03, 0x00, 0x02,
};

static const uint8_t vga_crtc_regs[25] = {
    0x5f, 0x4f, 0x50, 0x82, 0x55, 0x81, 0xbf, 0x1f,
    0x00, 0x4f, 0x0d, 0x0e, 0x00, 0x00, 0x00, 0x00,
    0x9c, 0x8e, 0x8f, 0x28, 0x1f, 0x96, 0xb9, 0xa3,
    0xff,
};

static const uint8_t vga_gc_regs[9] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0e, 0x00, 0xff,
};

static const uint8_t vga_attr_regs[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x0c, 0x00, 0x0f, 0x08, 0x00,
};

static void vga_write_seq(uint8_t index, uint8_t value)
{
    outb(VGA_SEQ_INDEX, index);
    io_wait();
    outb(VGA_SEQ_DATA, value);
    io_wait();
}

static void vga_write_crtc(uint8_t index, uint8_t value)
{
    outb(VGA_CRTC_INDEX, index);
    io_wait();
    outb(VGA_CRTC_DATA, value);
    io_wait();
}

static void vga_write_gc(uint8_t index, uint8_t value)
{
    outb(VGA_GC_INDEX, index);
    io_wait();
    outb(VGA_GC_DATA, value);
    io_wait();
}

static void vga_write_attr(uint8_t index, uint8_t value)
{
    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, index);
    io_wait();
    outb(VGA_AC_WRITE, value);
    io_wait();
}

static void vga_disable_blink(void)
{
    uint8_t attr;

    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, 0x10);
    attr = inb(VGA_AC_READ);
    attr &= (uint8_t)~0x08;
    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, 0x10);
    outb(VGA_AC_WRITE, attr);
    io_wait();
}

static void vga_set_cursor_shape(void)
{
    vga_write_crtc(0x0a, 0x0d);
    vga_write_crtc(0x0b, 0x0e);
}

static void vga_set_start_address(uint16_t address)
{
    vga_write_crtc(0x0c, (uint8_t)(address >> 8));
    vga_write_crtc(0x0d, (uint8_t)(address & 0xff));
}

void vga_init_text_mode(void)
{
    /* Reset the attribute controller flip-flop and blank display. */
    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, 0x00);
    io_wait();

    outb(VGA_MISC_WRITE, 0x67);
    io_wait();

    /* Program sequencer while held in reset, then release reset. */
    vga_write_seq(0x00, 0x01);
    for (uint8_t i = 1; i < 5; i++) {
        vga_write_seq(i, vga_seq_regs[i]);
    }
    vga_write_seq(0x00, vga_seq_regs[0]);

    /* Unlock protected CRTC registers before programming timing. */
    outb(VGA_CRTC_INDEX, 0x03);
    io_wait();
    outb(VGA_CRTC_DATA, (uint8_t)(inb(VGA_CRTC_DATA) | 0x80));
    io_wait();
    outb(VGA_CRTC_INDEX, 0x11);
    io_wait();
    outb(VGA_CRTC_DATA, (uint8_t)(inb(VGA_CRTC_DATA) & 0x7f));
    io_wait();

    for (uint8_t i = 0; i < 25; i++) {
        vga_write_crtc(i, vga_crtc_regs[i]);
    }

    for (uint8_t i = 0; i < 9; i++) {
        vga_write_gc(i, vga_gc_regs[i]);
    }

    for (uint8_t i = 0; i < 21; i++) {
        vga_write_attr(i, vga_attr_regs[i]);
    }

    vga_set_start_address(0);
    vga_set_cursor_shape();
    vga_disable_blink();

    /* Unblank display and return AC to normal address/data flip-flop state. */
    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, 0x20);
    io_wait();
}
