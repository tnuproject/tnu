#include <tnu/framebuffer.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/multiboot2.h>
#include <tnu/string.h>

static struct framebuffer_info fb;

static void write_pixel32(uint32_t x, uint32_t y, uint32_t rgb)
{
    volatile uint8_t *base = (volatile uint8_t *)fb.address + (uint64_t)y * fb.pitch + x * 4;
    base[0] = (uint8_t)(rgb & 0xff);
    base[1] = (uint8_t)((rgb >> 8) & 0xff);
    base[2] = (uint8_t)((rgb >> 16) & 0xff);
    base[3] = 0;
}

static void write_pixel24(uint32_t x, uint32_t y, uint32_t rgb)
{
    volatile uint8_t *base = (volatile uint8_t *)fb.address + (uint64_t)y * fb.pitch + x * 3;
    base[0] = (uint8_t)(rgb & 0xff);
    base[1] = (uint8_t)((rgb >> 8) & 0xff);
    base[2] = (uint8_t)((rgb >> 16) & 0xff);
}

static void write_pixel16(uint32_t x, uint32_t y, uint32_t rgb)
{
    uint16_t r = (uint16_t)((rgb >> 19) & 0x1f);
    uint16_t g = (uint16_t)((rgb >> 10) & 0x3f);
    uint16_t b = (uint16_t)((rgb >> 3) & 0x1f);
    volatile uint16_t *base = (volatile uint16_t *)((volatile uint8_t *)fb.address + (uint64_t)y * fb.pitch);
    base[x] = (uint16_t)((r << 11) | (g << 5) | b);
}

void framebuffer_init(void)
{
    const struct boot_info *boot = boot_info_get();
    memset(&fb, 0, sizeof(fb));
    if (!boot->framebuffer_addr || boot->framebuffer_addr == 0xb8000 || boot->framebuffer_bpp < 16) {
        fb.kind = FB_KIND_VGA_TEXT;
        fb.address = 0xb8000;
        fb.width = 80;
        fb.height = 25;
        fb.pitch = 160;
        fb.bpp = 16;
        log_info("fb", "no linear framebuffer provided; using VGA text mode");
        return;
    }
    fb.kind = FB_KIND_LINEAR;
    fb.address = (uintptr_t)boot->framebuffer_addr;
    fb.width = boot->framebuffer_width;
    fb.height = boot->framebuffer_height;
    fb.pitch = boot->framebuffer_pitch;
    fb.bpp = boot->framebuffer_bpp;

    uint64_t bytes = (uint64_t)fb.pitch * fb.height;
    if (!bytes || bytes > (uint64_t)((size_t)-1) ||
        vmm_map_range_identity(fb.address, (size_t)bytes, 0) < 0) {
        log_warn("fb", "linear framebuffer at %p could not be mapped; using VGA text mode",
                 (void *)fb.address);
        fb.kind = FB_KIND_VGA_TEXT;
        fb.address = 0xb8000;
        fb.width = 80;
        fb.height = 25;
        fb.pitch = 160;
        fb.bpp = 16;
        return;
    }

    log_info("fb", "%ux%u pitch=%u bpp=%u at %p",
             fb.width, fb.height, fb.pitch, fb.bpp, (void *)fb.address);
}

const struct framebuffer_info *framebuffer_info(void)
{
    return &fb;
}

bool framebuffer_is_graphics(void)
{
    return fb.kind == FB_KIND_LINEAR;
}

int framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    if (fb.kind != FB_KIND_LINEAR || x >= fb.width || y >= fb.height) {
        return -1;
    }
    if (fb.bpp == 32) {
        write_pixel32(x, y, rgb);
        return 0;
    }
    if (fb.bpp == 24) {
        write_pixel24(x, y, rgb);
        return 0;
    }
    if (fb.bpp == 16) {
        write_pixel16(x, y, rgb);
        return 0;
    }
    return -1;
}

int framebuffer_fillrect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    if (fb.kind != FB_KIND_LINEAR || x >= fb.width || y >= fb.height) {
        return -1;
    }
    if (x + w > fb.width) {
        w = fb.width - x;
    }
    if (y + h > fb.height) {
        h = fb.height - y;
    }
    if (fb.bpp == 32) {
        uint32_t pixel = rgb & 0x00ffffffu;
        for (uint32_t yy = 0; yy < h; yy++) {
            volatile uint32_t *row =
                (volatile uint32_t *)((volatile uint8_t *)fb.address + (uint64_t)(y + yy) * fb.pitch);
            for (uint32_t xx = 0; xx < w; xx++) {
                row[x + xx] = pixel;
            }
        }
        return 0;
    }
    if (fb.bpp == 16) {
        uint16_t r = (uint16_t)((rgb >> 19) & 0x1f);
        uint16_t g = (uint16_t)((rgb >> 10) & 0x3f);
        uint16_t b = (uint16_t)((rgb >> 3) & 0x1f);
        uint16_t pixel = (uint16_t)((r << 11) | (g << 5) | b);
        for (uint32_t yy = 0; yy < h; yy++) {
            volatile uint16_t *row =
                (volatile uint16_t *)((volatile uint8_t *)fb.address + (uint64_t)(y + yy) * fb.pitch);
            for (uint32_t xx = 0; xx < w; xx++) {
                row[x + xx] = pixel;
            }
        }
        return 0;
    }
    if (fb.bpp == 24) {
        for (uint32_t yy = 0; yy < h; yy++) {
            volatile uint8_t *row = (volatile uint8_t *)fb.address +
                                    (uint64_t)(y + yy) * fb.pitch + x * 3;
            for (uint32_t xx = 0; xx < w; xx++) {
                row[xx * 3] = (uint8_t)(rgb & 0xff);
                row[xx * 3 + 1] = (uint8_t)((rgb >> 8) & 0xff);
                row[xx * 3 + 2] = (uint8_t)((rgb >> 16) & 0xff);
            }
        }
        return 0;
    }
    for (uint32_t yy = 0; yy < h; yy++) {
        for (uint32_t xx = 0; xx < w; xx++) {
            framebuffer_putpixel(x + xx, y + yy, rgb);
        }
    }
    return 0;
}

int framebuffer_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     const uint32_t *pixels, uint32_t src_pitch_pixels)
{
    if (!pixels || fb.kind != FB_KIND_LINEAR || x >= fb.width || y >= fb.height) {
        return -1;
    }
    if (x + w > fb.width) {
        w = fb.width - x;
    }
    if (y + h > fb.height) {
        h = fb.height - y;
    }
    if (fb.bpp == 32) {
        for (uint32_t yy = 0; yy < h; yy++) {
            volatile uint32_t *dst =
                (volatile uint32_t *)((volatile uint8_t *)fb.address + (uint64_t)(y + yy) * fb.pitch);
            const uint32_t *src = &pixels[yy * src_pitch_pixels];
            for (uint32_t xx = 0; xx < w; xx++) {
                dst[x + xx] = src[xx] & 0x00ffffffu;
            }
        }
        return 0;
    }
    if (fb.bpp == 16) {
        for (uint32_t yy = 0; yy < h; yy++) {
            volatile uint16_t *dst =
                (volatile uint16_t *)((volatile uint8_t *)fb.address + (uint64_t)(y + yy) * fb.pitch);
            const uint32_t *src = &pixels[yy * src_pitch_pixels];
            for (uint32_t xx = 0; xx < w; xx++) {
                uint32_t rgb = src[xx];
                uint16_t r = (uint16_t)((rgb >> 19) & 0x1f);
                uint16_t g = (uint16_t)((rgb >> 10) & 0x3f);
                uint16_t b = (uint16_t)((rgb >> 3) & 0x1f);
                dst[x + xx] = (uint16_t)((r << 11) | (g << 5) | b);
            }
        }
        return 0;
    }
    if (fb.bpp == 24) {
        for (uint32_t yy = 0; yy < h; yy++) {
            volatile uint8_t *dst = (volatile uint8_t *)fb.address +
                                    (uint64_t)(y + yy) * fb.pitch + x * 3;
            const uint32_t *src = &pixels[yy * src_pitch_pixels];
            for (uint32_t xx = 0; xx < w; xx++) {
                uint32_t rgb = src[xx];
                dst[xx * 3] = (uint8_t)(rgb & 0xff);
                dst[xx * 3 + 1] = (uint8_t)((rgb >> 8) & 0xff);
                dst[xx * 3 + 2] = (uint8_t)((rgb >> 16) & 0xff);
            }
        }
        return 0;
    }
    for (uint32_t yy = 0; yy < h; yy++) {
        for (uint32_t xx = 0; xx < w; xx++) {
            framebuffer_putpixel(x + xx, y + yy, pixels[yy * src_pitch_pixels + xx]);
        }
    }
    return 0;
}
