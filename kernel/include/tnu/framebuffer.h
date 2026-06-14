#ifndef TNU_FRAMEBUFFER_H
#define TNU_FRAMEBUFFER_H

#include <tnu/types.h>

enum framebuffer_kind {
    FB_KIND_NONE = 0,
    FB_KIND_VGA_TEXT,
    FB_KIND_LINEAR,
};

struct framebuffer_info {
    enum framebuffer_kind kind;
    uintptr_t address;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
};

void framebuffer_init(void);
const struct framebuffer_info *framebuffer_info(void);
bool framebuffer_is_graphics(void);
int framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t rgb);
int framebuffer_fillrect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
int framebuffer_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     const uint32_t *pixels, uint32_t src_pitch_pixels);
int framebuffer_scroll_up(uint32_t pixels, uint32_t clear_rgb);

#endif
