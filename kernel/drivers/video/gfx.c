/**
 * @file gfx.c
 * @brief Graphics API - Hardware-independent 2D graphics primitives
 */

#include <tnu/gfx.h>
#include <tnu/video.h>
#include <tnu/string.h>
#include <tnu/log.h>
#include <arch/cpu.h>

/* Kernel memory allocator */
extern void *vmm_alloc_pages(size_t pages);

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
    if (!back_buffer) return NULL;
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
    
    /* Allocate back buffer for double buffering */
    if (vid->mode_type != VIDEO_MODE_VGA_TEXT && vid->framebuffer_size > 0) {
        back_buffer_size = vid->framebuffer_size;
        back_buffer = NULL;
        log_warn("gfx", "Double buffering disabled - no kernel page allocator available");
    }
    
    gfx_initialized = true;
    log_info("gfx", "Graphics API initialized");
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
    
    uint8_t *dst = get_pixel_addr(x, y);
    uint8_t *back = get_back_pixel_addr(x, y);
    
    switch (vid->bits_per_pixel) {
        case 32:
            *(uint32_t *)dst = color;
            if (back) *(uint32_t *)back = color;
            break;
            
        case 24:
            dst[0] = (uint8_t)(color & 0xff);
            dst[1] = (uint8_t)((color >> 8) & 0xff);
            dst[2] = (uint8_t)((color >> 16) & 0xff);
            if (back) {
                back[0] = dst[0];
                back[1] = dst[1];
                back[2] = dst[2];
            }
            break;
            
        case 16: {
            /* RGB565 conversion */
            uint16_t r = (uint16_t)((color >> 19) & 0x1f);
            uint16_t g = (uint16_t)((color >> 10) & 0x3f);
            uint16_t b = (uint16_t)((color >> 3) & 0x1f);
            uint16_t pixel = (uint16_t)((r << 11) | (g << 5) | b);
            *(uint16_t *)dst = pixel;
            if (back) *(uint16_t *)back = pixel;
            break;
        }
    }
    
    gfx_mark_dirty(x, y, 1, 1);
}

/**
 * gfx_get_pixel - Read pixel from framebuffer
 */
uint32_t gfx_get_pixel(int32_t x, int32_t y)
{
    if (!gfx_initialized || !on_screen(x, y)) {
        return GFX_COLOR_BLACK;
    }
    
    uint8_t *src = get_pixel_addr(x, y);
    
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
        /* Horizontal-ish */
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
        /* Vertical-ish */
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
 * gfx_fill_rect - Fill rectangle with solid color
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
    
    /* Fast path for 32-bit */
    if (vid->bits_per_pixel == 32) {
        uint32_t *dst = (uint32_t *)get_pixel_addr(x1, y1);
        uint32_t *back = back_buffer ? (uint32_t *)get_back_pixel_addr(x1, y1) : NULL;
        
        for (int32_t row = 0; row < h; row++) {
            for (int32_t col = 0; col < w; col++) {
                dst[col] = color;
                if (back) back[col] = color;
            }
            dst = (uint32_t *)((uint8_t *)dst + vid->pitch);
            if (back) back = (uint32_t *)((uint8_t *)back + vid->pitch);
        }
    } else {
        /* Slow path */
        for (int32_t yy = y1; yy < y2; yy++) {
            for (int32_t xx = x1; xx < x2; xx++) {
                gfx_draw_pixel(xx, yy, color);
            }
        }
    }
    
    gfx_mark_dirty(x1, y1, w, h);
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
    if (rx <= 0 || ry <= 0)
        return;

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


    err =
        ry2 * (x + 1) * (x + 1) +
        rx2 * (y - 1) * (y - 1) -
        rx2 * ry2;


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
    
    /* Handle overlapping regions */
    if (src_y < dst_y || (src_y == dst_y && src_x < dst_x)) {
        /* Copy backwards */
        for (int32_t y = h - 1; y >= 0; y--) {
            uint8_t *src = get_pixel_addr(src_x, src_y + y);
            uint8_t *dst = get_pixel_addr(dst_x, dst_y + y);
            memmove(dst, src, (size_t)w * vid->bytes_per_pixel);
        }
    } else {
        /* Copy forwards */
        for (int32_t y = 0; y < h; y++) {
            uint8_t *src = get_pixel_addr(src_x, src_y + y);
            uint8_t *dst = get_pixel_addr(dst_x, dst_y + y);
            memmove(dst, src, (size_t)w * vid->bytes_per_pixel);
        }
    }
    
    gfx_mark_dirty(dst_x, dst_y, w, h);
}

/**
 * gfx_clear - Clear entire screen
 */
void gfx_clear(uint32_t color)
{
    gfx_fill_rect(0, 0, (int32_t)vid->width, (int32_t)vid->height, color);
}

/* Double buffering */

void gfx_swap_buffers(void)
{
    if (!back_buffer) return;
    
    /* Copy back buffer to front */
    memcpy((void *)vid->framebuffer_addr, back_buffer, vid->framebuffer_size);
}

bool gfx_has_double_buffer(void)
{
    return back_buffer != NULL;
}

/* Dirty rectangle tracking */

void gfx_mark_dirty(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!has_dirty) {
        dirty_rect.x = x;
        dirty_rect.y = y;
        dirty_rect.w = w;
        dirty_rect.h = h;
        has_dirty = true;
    } else {
        /* Expand rectangle */
        int32_t x1 = dirty_rect.x < x ? dirty_rect.x : x;
        int32_t y1 = dirty_rect.y < y ? dirty_rect.y : y;
        int32_t x2 = dirty_rect.x + dirty_rect.w;
        int32_t y2 = dirty_rect.y + dirty_rect.h;
        int32_t nx2 = x + w;
        int32_t ny2 = y + h;
        
        dirty_rect.x = x1;
        dirty_rect.y = y1;
        dirty_rect.w = (x2 > nx2 ? x2 : nx2) - x1;
        dirty_rect.h = (y2 > ny2 ? y2 : ny2) - y1;
    }
}

void gfx_flush_dirty(void)
{
    /* For now, just clear dirty flag */
    /* In full implementation, would copy only dirty region */
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
