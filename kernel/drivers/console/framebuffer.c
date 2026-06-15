#include <tnu/framebuffer.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/multiboot2.h>
#include <tnu/string.h>

static struct framebuffer_info fb;

static void write_pixel32(uint32_t x, uint32_t y, uint32_t rgb)
{
    uint32_t *base = (uint32_t *)((uint8_t *)fb.address + (uint64_t)y * fb.pitch);
    base[x] = rgb & 0x00ffffffu;
}

static void write_pixel24(uint32_t x, uint32_t y, uint32_t rgb)
{
    uint8_t *base = (uint8_t *)fb.address + (uint64_t)y * fb.pitch + x * 3;
    base[0] = (uint8_t)(rgb & 0xff);
    base[1] = (uint8_t)((rgb >> 8) & 0xff);
    base[2] = (uint8_t)((rgb >> 16) & 0xff);
}

static void write_pixel16(uint32_t x, uint32_t y, uint32_t rgb)
{
    uint16_t r = (uint16_t)((rgb >> 19) & 0x1f);
    uint16_t g = (uint16_t)((rgb >> 10) & 0x3f);
    uint16_t b = (uint16_t)((rgb >> 3) & 0x1f);
    uint16_t *base = (uint16_t *)((uint8_t *)fb.address + (uint64_t)y * fb.pitch);
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
    /*
     * Map the framebuffer Write-Combining + Writable.
     * WC coalesces consecutive stores into burst transactions, which is the
     * single biggest framebuffer speedup on real hardware and in QEMU with
     * -device VGA or -device virtio-vga.  Without WC every 4-byte store goes
     * out individually over the bus; with WC they are batched into 64-byte
     * write-combining buffers and flushed as cacheline-sized transactions.
     */
    if (!bytes || bytes > (uint64_t)((size_t)-1) ||
        vmm_map_range_identity(fb.address, (size_t)bytes,
                               VMM_FLAG_WRITABLE | VMM_FLAG_WC) < 0) {
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
    if (x + w > fb.width)  w = fb.width  - x;
    if (y + h > fb.height) h = fb.height - y;
    if (!w || !h) return 0;

    if (fb.bpp == 32) {
        uint32_t pixel = rgb & 0x00ffffffu;
        /*
         * Fast path: when x == 0 and w == fb.width the row stride equals
         * fb.pitch exactly. We can fill the entire visible rectangle with a
         * single memset32 call instead of one call per scan-line.
         */
        if (x == 0 && w == fb.width && (fb.pitch == fb.width * 4)) {
            uint32_t *dst = (uint32_t *)(fb.address + (uint64_t)y * fb.pitch);
            memset32(dst, pixel, (size_t)w * h);
        } else {
            for (uint32_t yy = 0; yy < h; yy++) {
                uint32_t *row = (uint32_t *)(fb.address + (uint64_t)(y + yy) * fb.pitch);
                memset32(row + x, pixel, w);
            }
        }
        return 0;
    }
    if (fb.bpp == 16) {
        uint16_t r16 = (uint16_t)((rgb >> 19) & 0x1f);
        uint16_t g16 = (uint16_t)((rgb >> 10) & 0x3f);
        uint16_t b16 = (uint16_t)((rgb >> 3)  & 0x1f);
        uint16_t pixel = (uint16_t)((r16 << 11) | (g16 << 5) | b16);
        /* Pack two pixels into a uint32_t for memset32 */
        uint32_t pixel32 = ((uint32_t)pixel << 16) | pixel;
        if (x == 0 && w == fb.width && (fb.pitch == fb.width * 2) && ((w & 1) == 0)) {
            uint32_t *dst = (uint32_t *)(fb.address + (uint64_t)y * fb.pitch);
            memset32(dst, pixel32, (size_t)(w >> 1) * h);
        } else {
            for (uint32_t yy = 0; yy < h; yy++) {
                uint8_t *rowb = (uint8_t *)fb.address + (uint64_t)(y + yy) * fb.pitch + x * 2;
                uint32_t remaining = w;
                /* Handle odd leading pixel */
                if ((uintptr_t)rowb & 2) {
                    *(uint16_t *)rowb = pixel;
                    rowb += 2;
                    remaining--;
                }
                memset32((uint32_t *)rowb, pixel32, remaining >> 1);
                if (remaining & 1) {
                    *(uint16_t *)(rowb + (remaining & ~1u) * 2) = pixel;
                }
            }
        }
        return 0;
    }
    if (fb.bpp == 24) {
        for (uint32_t yy = 0; yy < h; yy++) {
            uint8_t *row = (uint8_t *)fb.address + (uint64_t)(y + yy) * fb.pitch + x * 3;
            for (uint32_t xx = 0; xx < w; xx++) {
                row[xx * 3]     = (uint8_t)(rgb & 0xff);
                row[xx * 3 + 1] = (uint8_t)((rgb >> 8) & 0xff);
                row[xx * 3 + 2] = (uint8_t)((rgb >> 16) & 0xff);
            }
        }
        return 0;
    }
    /* Fallback */
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
    if (x + w > fb.width)  w = fb.width  - x;
    if (y + h > fb.height) h = fb.height - y;
    if (!w || !h) return 0;

    if (fb.bpp == 32) {
        for (uint32_t yy = 0; yy < h; yy++) {
            uint32_t *dst = (uint32_t *)(fb.address + (uint64_t)(y + yy) * fb.pitch) + x;
            const uint32_t *src = &pixels[yy * src_pitch_pixels];
            /* Mask alpha out of each pixel then bulk-copy the row */
            if (x == 0 && w == fb.width && src_pitch_pixels == fb.width) {
                /* Source and destination rows are contiguous — direct memcpy */
                memcpy(dst, src, (size_t)w * 4);
            } else {
                for (uint32_t xx = 0; xx < w; xx++) {
                    dst[xx] = src[xx] & 0x00ffffffu;
                }
            }
        }
        return 0;
    }
    if (fb.bpp == 16) {
        for (uint32_t yy = 0; yy < h; yy++) {
            uint16_t *dst = (uint16_t *)(fb.address + (uint64_t)(y + yy) * fb.pitch) + x;
            const uint32_t *src = &pixels[yy * src_pitch_pixels];
            for (uint32_t xx = 0; xx < w; xx++) {
                uint32_t rgb = src[xx];
                uint16_t r16 = (uint16_t)((rgb >> 19) & 0x1f);
                uint16_t g16 = (uint16_t)((rgb >> 10) & 0x3f);
                uint16_t b16 = (uint16_t)((rgb >>  3) & 0x1f);
                dst[xx] = (uint16_t)((r16 << 11) | (g16 << 5) | b16);
            }
        }
        return 0;
    }
    if (fb.bpp == 24) {
        for (uint32_t yy = 0; yy < h; yy++) {
            uint8_t *dst = (uint8_t *)fb.address + (uint64_t)(y + yy) * fb.pitch + x * 3;
            const uint32_t *src = &pixels[yy * src_pitch_pixels];
            for (uint32_t xx = 0; xx < w; xx++) {
                uint32_t rgb = src[xx];
                dst[xx * 3]     = (uint8_t)(rgb & 0xff);
                dst[xx * 3 + 1] = (uint8_t)((rgb >> 8)  & 0xff);
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

int framebuffer_scroll_up(uint32_t pixels, uint32_t clear_rgb)
{
    if (fb.kind != FB_KIND_LINEAR || pixels == 0) {
        return -1;
    }
    if (pixels >= fb.height) {
        return framebuffer_fillrect(0, 0, fb.width, fb.height, clear_rgb);
    }

    uint8_t *base        = (uint8_t *)fb.address;
    size_t scroll_bytes  = (size_t)pixels * fb.pitch;       /* bytes to discard at top   */
    size_t move_bytes    = (size_t)(fb.height - pixels) * fb.pitch; /* bytes to shift up */

    /*
     * memmove is now backed by 'rep movsq' (8 bytes / iter).
     * For a full-screen 1920×1080 32-bpp scroll that is ~8 MB moved as
     * ~1 M 64-bit stores — roughly 10× faster than the old byte loop.
     */
    memmove(base, base + scroll_bytes, move_bytes);

    /* Clear only the newly exposed bottom strip with memset32 (rep stosd). */
    return framebuffer_fillrect(0, fb.height - pixels, fb.width, pixels, clear_rgb);
}

