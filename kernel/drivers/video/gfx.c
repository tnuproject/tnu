/**
 * @file gfx.c
 * @brief Graphics API - Hardware-independent 2D graphics primitives with
 *        Double Buffering and Dirty Rectangle Tracking for maximum fluidity.
 */

#include <tnu/gfx.h>
#include <tnu/video.h>
#include <tnu/string.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <arch/cpu.h>

/* GFX state */
static const struct video_info *vid;
static bool gfx_initialized = false;

/* Double buffering */
static uint8_t *back_buffer = NULL;
static size_t back_buffer_size = 0;

/* Dirty rectangle tracking */
static struct gfx_rect dirty_rect = {0, 0, 0, 0};
static bool has_dirty = false;

/* Inline helper: clamp value to range */
static inline int32_t clamp32(int32_t val, int32_t min, int32_t max)
{
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

static inline int32_t iabs32(int32_t x)
{
    return x < 0 ? -x : x;
}

/* Inline helper: check if point is on screen */
static inline bool on_screen(int32_t x, int32_t y)
{
    return x >= 0 && y >= 0 && 
           (uint32_t)x < vid->width && (uint32_t)y < vid->height;
}

/* Inline helper: get pixel address */
static inline uint8_t *get_pixel_addr(int32_t x, int32_t y)
{
    return (uint8_t *)vid->framebuffer_addr + (size_t)y * vid->pitch + 
           (size_t)x * vid->bytes_per_pixel;
}

static inline uint8_t *get_back_pixel_addr(int32_t x, int32_t y)
{
    if (!back_buffer) return get_pixel_addr(x, y);
    return back_buffer + (size_t)y * vid->pitch + 
           (size_t)x * vid->bytes_per_pixel;
}

/**
 * gfx_init - Initialize graphics subsystem
 */
int gfx_init(void)
{
    vid = video_get_info();
    if (!vid) {
        return -1;
    }
    
    /* Allocate back buffer for tearing-free double buffering */
    if (vid->mode_type != VIDEO_MODE_VGA_TEXT && vid->framebuffer_size > 0) {
        back_buffer_size = vid->framebuffer_size;
        back_buffer = (uint8_t *)kmalloc(back_buffer_size);
        if (back_buffer) {
            memset(back_buffer, 0, back_buffer_size);
            log_info("gfx", "Double buffering enabled (%zu KiB back-buffer)",
                     back_buffer_size / 1024);
        } else {
            log_warn("gfx", "Back-buffer allocation failed; using direct framebuffer");
        }
    }
    
    dirty_rect.x = 0;
    dirty_rect.y = 0;
    dirty_rect.w = 0;
    dirty_rect.h = 0;
    has_dirty = false;

    gfx_initialized = true;
    log_info("gfx", "Graphics 2D API initialized");
    return 0;
}

bool gfx_is_initialized(void)
{
    return gfx_initialized;
}

/**
 * gfx_draw_pixel - Draw a single pixel
 */
void gfx_draw_pixel(int32_t x, int32_t y, uint32_t color)
{
    if (!gfx_initialized || !on_screen(x, y)) {
        return;
    }
    
    uint8_t *target = get_back_pixel_addr(x, y);
    
    switch (vid->bits_per_pixel) {
        case 32:
            *(uint32_t *)target = color;
            break;
            
        case 24:
            target[0] = (uint8_t)(color & 0xff);
            target[1] = (uint8_t)((color >> 8) & 0xff);
            target[2] = (uint8_t)((color >> 16) & 0xff);
            break;
            
        case 16: {
            /* RGB565 conversion */
            uint16_t r = (uint16_t)((color >> 19) & 0x1f);
            uint16_t g = (uint16_t)((color >> 10) & 0x3f);
            uint16_t b = (uint16_t)((color >> 3) & 0x1f);
            uint16_t pixel = (uint16_t)((r << 11) | (g << 5) | b);
            *(uint16_t *)target = pixel;
            break;
        }
    }
    
    if (back_buffer) {
        gfx_mark_dirty(x, y, 1, 1);
    }
}

/**
 * gfx_get_pixel - Read pixel from framebuffer / back-buffer
 */
uint32_t gfx_get_pixel(int32_t x, int32_t y)
{
    if (!gfx_initialized || !on_screen(x, y)) {
        return GFX_COLOR_BLACK;
    }
    
    uint8_t *src = get_back_pixel_addr(x, y);
    
    switch (vid->bits_per_pixel) {
        case 32:
            return *(uint32_t *)src;
            
        case 24:
            return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16);
            
        case 16: {
            uint16_t pixel = *(uint16_t *)src;
            uint32_t r = (uint32_t)((pixel >> 11) & 0x1f) << 3;
            uint32_t g = (uint32_t)((pixel >> 5) & 0x3f) << 2;
            uint32_t b = (uint32_t)(pixel & 0x1f) << 3;
            return (r << 16) | (g << 8) | b;
        }
    }
    
    return GFX_COLOR_BLACK;
}

/**
 * gfx_draw_line - Draw line using Bresenham's algorithm
 */
void gfx_draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color)
{
    int32_t dx = x2 - x1;
    int32_t dy = y2 - y1;
    
    if (dx == 0 && dy == 0) {
        gfx_draw_pixel(x1, y1, color);
        return;
    }
    
    if (iabs32(dx) >= iabs32(dy)) {
        if (x1 > x2) {
            int32_t tmp = x1; x1 = x2; x2 = tmp;
            tmp = y1; y1 = y2; y2 = tmp;
            dx = -dx;
            dy = -dy;
        }
        int32_t yi = dy > 0 ? 1 : -1;
        dy = iabs32(dy);
        int32_t d = 2 * dy - dx;
        int32_t y = y1;
        
        for (int32_t x = x1; x <= x2; x++) {
            gfx_draw_pixel(x, y, color);
            if (d > 0) {
                y += yi;
                d -= 2 * dx;
            }
            d += 2 * dy;
        }
    } else {
        if (y1 > y2) {
            int32_t tmp = x1; x1 = x2; x2 = tmp;
            tmp = y1; y1 = y2; y2 = tmp;
            dx = -dx;
            dy = -dy;
        }
        int32_t xi = dx > 0 ? 1 : -1;
        dx = iabs32(dx);
        int32_t d = 2 * dx - dy;
        int32_t x = x1;
        
        for (int32_t y = y1; y <= y2; y++) {
            gfx_draw_pixel(x, y, color);
            if (d > 0) {
                x += xi;
                d -= 2 * dy;
            }
            d += 2 * dx;
        }
    }
}

/**
 * gfx_draw_rect - Draw rectangle outline
 */
void gfx_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    if (w <= 0 || h <= 0) return;
    
    gfx_draw_line(x, y, x + w - 1, y, color);
    gfx_draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    gfx_draw_line(x, y, x, y + h - 1, color);
    gfx_draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

/**
 * gfx_fill_rect - Fill rectangle with solid color (optimized)
 */
void gfx_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    if (!gfx_initialized || w <= 0 || h <= 0) return;
    
    /* Clip to screen */
    int32_t x1 = clamp32(x, 0, (int32_t)vid->width);
    int32_t y1 = clamp32(y, 0, (int32_t)vid->height);
    int32_t x2 = clamp32(x + w, 0, (int32_t)vid->width);
    int32_t y2 = clamp32(y + h, 0, (int32_t)vid->height);
    
    w = x2 - x1;
    h = y2 - y1;
    if (w <= 0 || h <= 0) return;
    
    /* 32-bit fast path */
    if (vid->bits_per_pixel == 32) {
        uint32_t *target = (uint32_t *)get_back_pixel_addr(x1, y1);
        uint32_t pixel = color & 0x00ffffffu;
        
        for (int32_t row = 0; row < h; row++) {
            memset32(target, pixel, (size_t)w);
            target = (uint32_t *)((uint8_t *)target + vid->pitch);
        }
    } else {
        for (int32_t yy = y1; yy < y2; yy++) {
            for (int32_t xx = x1; xx < x2; xx++) {
                gfx_draw_pixel(xx, yy, color);
            }
        }
    }
    
    if (back_buffer) {
        gfx_mark_dirty(x1, y1, w, h);
    }
}

/**
 * gfx_draw_circle - Draw circle outline (midpoint algorithm)
 */
void gfx_draw_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color)
{
    if (radius <= 0) return;
    
    int32_t x = radius;
    int32_t y = 0;
    int32_t err = 0;
    
    while (x >= y) {
        gfx_draw_pixel(cx + x, cy + y, color);
        gfx_draw_pixel(cx + y, cy + x, color);
        gfx_draw_pixel(cx - y, cy + x, color);
        gfx_draw_pixel(cx - x, cy + y, color);
        gfx_draw_pixel(cx - x, cy - y, color);
        gfx_draw_pixel(cx - y, cy - x, color);
        gfx_draw_pixel(cx + y, cy - x, color);
        gfx_draw_pixel(cx + x, cy - y, color);
        
        y++;
        err += 1 + 2 * y;
        if (2 * (err - x) + 1 > 0) {
            x--;
            err += 1 - 2 * x;
        }
    }
}

/**
 * gfx_fill_circle - Fill circle with solid color
 */
void gfx_fill_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color)
{
    if (radius <= 0) return;
    
    for (int32_t y = -radius; y <= radius; y++) {
        for (int32_t x = -radius; x <= radius; x++) {
            if (x * x + y * y <= radius * radius) {
                gfx_draw_pixel(cx + x, cy + y, color);
            }
        }
    }
}

/**
 * gfx_draw_ellipse - Draw ellipse outline
 */
void gfx_draw_ellipse(int32_t cx, int32_t cy, int32_t rx, int32_t ry, uint32_t color)
{
    if (rx <= 0 || ry <= 0) return;

    int32_t x = 0;
    int32_t y = ry;

    int64_t rx2 = (int64_t)rx * rx;
    int64_t ry2 = (int64_t)ry * ry;

    int64_t dx = 0;
    int64_t dy = 2 * rx2 * y;
    int64_t err = ry2 - rx2 * ry + rx2 / 4;

    while (dx < dy) {
        gfx_draw_pixel(cx + x, cy + y, color);
        gfx_draw_pixel(cx - x, cy + y, color);
        gfx_draw_pixel(cx + x, cy - y, color);
        gfx_draw_pixel(cx - x, cy - y, color);

        x++;
        dx += 2 * ry2;

        if (err < 0) {
            err += ry2 + dx;
        } else {
            y--;
            dy -= 2 * rx2;
            err += ry2 + dx - dy;
        }
    }

    err = ry2 * (x + 1) * (x + 1) + rx2 * (y - 1) * (y - 1) - rx2 * ry2;

    while (y >= 0) {
        gfx_draw_pixel(cx + x, cy + y, color);
        gfx_draw_pixel(cx - x, cy + y, color);
        gfx_draw_pixel(cx + x, cy - y, color);
        gfx_draw_pixel(cx - x, cy - y, color);

        y--;
        dy -= 2 * rx2;

        if (err > 0) {
            err += rx2 - dy;
        } else {
            x++;
            dx += 2 * ry2;
            err += rx2 - dy + dx;
        }
    }
}

/**
 * gfx_fill_ellipse - Fill ellipse with solid color
 */
void gfx_fill_ellipse(int32_t cx, int32_t cy, int32_t rx, int32_t ry, uint32_t color)
{
    if (rx <= 0 || ry <= 0) return;
    
    int32_t rx2 = rx * rx;
    int32_t ry2 = ry * ry;
    
    for (int32_t y = -ry; y <= ry; y++) {
        for (int32_t x = -rx; x <= rx; x++) {
            if ((x * x * ry2 + y * y * rx2) <= rx2 * ry2) {
                gfx_draw_pixel(cx + x, cy + y, color);
            }
        }
    }
}

/**
 * gfx_draw_bitmap - Draw monochrome bitmap
 */
void gfx_draw_bitmap(int32_t x, int32_t y, const struct gfx_bitmap *bmp)
{
    if (!bmp || !bmp->data) return;
    
    for (uint32_t yy = 0; yy < bmp->height; yy++) {
        for (uint32_t xx = 0; xx < bmp->width; xx++) {
            uint32_t byte_idx = (yy * ((bmp->width + 7) / 8)) + (xx / 8);
            uint32_t bit_idx = 7 - (xx % 8);
            bool pixel = (bmp->data[byte_idx] >> bit_idx) & 1;
            
            uint32_t color = pixel ? bmp->fg_color : bmp->bg_color;
            
            /* Skip transparent pixels */
            if (!pixel && bmp->bg_color == 0xFFFFFFFF /* GFX_COLOR_NONE */) {
                continue;
            }
            
            gfx_draw_pixel(x + (int32_t)xx, y + (int32_t)yy, color);
        }
    }
}

/**
 * gfx_copy_region - Copy rectangular region
 */
void gfx_copy_region(int32_t src_x, int32_t src_y,
                     int32_t dst_x, int32_t dst_y,
                     int32_t w, int32_t h)
{
    if (!gfx_initialized || w <= 0 || h <= 0) return;
    
    /* Handle overlapping regions in back_buffer or front framebuffer */
    if (src_y < dst_y || (src_y == dst_y && src_x < dst_x)) {
        /* Copy backwards */
        for (int32_t y = h - 1; y >= 0; y--) {
            uint8_t *src = get_back_pixel_addr(src_x, src_y + y);
            uint8_t *dst = get_back_pixel_addr(dst_x, dst_y + y);
            memmove(dst, src, (size_t)w * vid->bytes_per_pixel);
        }
    } else {
        /* Copy forwards */
        for (int32_t y = 0; y < h; y++) {
            uint8_t *src = get_back_pixel_addr(src_x, src_y + y);
            uint8_t *dst = get_back_pixel_addr(dst_x, dst_y + y);
            memmove(dst, src, (size_t)w * vid->bytes_per_pixel);
        }
    }
    
    if (back_buffer) {
        gfx_mark_dirty(dst_x, dst_y, w, h);
    }
}

/**
 * gfx_clear - Clear entire screen
 */
void gfx_clear(uint32_t color)
{
    gfx_fill_rect(0, 0, (int32_t)vid->width, (int32_t)vid->height, color);
}

/* Double buffering & Dirty Rectangles */

void gfx_swap_buffers(void)
{
    if (!back_buffer) return;
    
    /* Copy entire back buffer to front framebuffer via rep movsq */
    memcpy((void *)vid->framebuffer_addr, back_buffer, vid->framebuffer_size);
    has_dirty = false;
}

bool gfx_has_double_buffer(void)
{
    return back_buffer != NULL;
}

void gfx_mark_dirty(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (w <= 0 || h <= 0) return;
    
    /* Clip to screen bounds */
    int32_t x1 = clamp32(x, 0, (int32_t)vid->width);
    int32_t y1 = clamp32(y, 0, (int32_t)vid->height);
    int32_t x2 = clamp32(x + w, 0, (int32_t)vid->width);
    int32_t y2 = clamp32(y + h, 0, (int32_t)vid->height);
    
    w = x2 - x1;
    h = y2 - y1;
    if (w <= 0 || h <= 0) return;
    
    if (!has_dirty) {
        dirty_rect.x = x1;
        dirty_rect.y = y1;
        dirty_rect.w = w;
        dirty_rect.h = h;
        has_dirty = true;
    } else {
        int32_t cur_x1 = dirty_rect.x;
        int32_t cur_y1 = dirty_rect.y;
        int32_t cur_x2 = dirty_rect.x + dirty_rect.w;
        int32_t cur_y2 = dirty_rect.y + dirty_rect.h;
        
        int32_t new_x1 = x1 < cur_x1 ? x1 : cur_x1;
        int32_t new_y1 = y1 < cur_y1 ? y1 : cur_y1;
        int32_t new_x2 = x2 > cur_x2 ? x2 : cur_x2;
        int32_t new_y2 = y2 > cur_y2 ? y2 : cur_y2;
        
        dirty_rect.x = new_x1;
        dirty_rect.y = new_y1;
        dirty_rect.w = new_x2 - new_x1;
        dirty_rect.h = new_y2 - new_y1;
    }
}

/**
 * gfx_flush_dirty - Transfer only modified areas to VRAM (minimizing PCIe bus load)
 */
void gfx_flush_dirty(void)
{
    if (!has_dirty || !back_buffer) {
        has_dirty = false;
        return;
    }
    
    int32_t x = dirty_rect.x;
    int32_t y = dirty_rect.y;
    int32_t w = dirty_rect.w;
    int32_t h = dirty_rect.h;
    
    if (w <= 0 || h <= 0) {
        has_dirty = false;
        return;
    }
    
    /* Full-screen blit optimization */
    if (x == 0 && (uint32_t)w == vid->width && (size_t)w * vid->bytes_per_pixel == vid->pitch) {
        size_t offset = (size_t)y * vid->pitch;
        size_t bytes  = (size_t)h * vid->pitch;
        memcpy((uint8_t *)vid->framebuffer_addr + offset, back_buffer + offset, bytes);
    } else {
        size_t row_bytes = (size_t)w * vid->bytes_per_pixel;
        for (int32_t row = 0; row < h; row++) {
            uint8_t *src = back_buffer + (size_t)(y + row) * vid->pitch + (size_t)x * vid->bytes_per_pixel;
            uint8_t *dst = (uint8_t *)vid->framebuffer_addr + (size_t)(y + row) * vid->pitch + (size_t)x * vid->bytes_per_pixel;
            memcpy(dst, src, row_bytes);
        }
    }
    
    has_dirty = false;
}

bool gfx_get_dirty_rect(struct gfx_rect *rect)
{
    if (!has_dirty || !rect) return false;
    *rect = dirty_rect;
    return true;
}

void gfx_clear_dirty(void)
{
    has_dirty = false;
}
