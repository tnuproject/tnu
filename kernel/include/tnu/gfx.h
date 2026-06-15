#ifndef TNU_GFX_H
#define TNU_GFX_H

#include <tnu/types.h>
#include <tnu/video.h>

/**
 * GFX - Kernel Graphics API
 * 
 * Hardware-independent 2D graphics primitives.
 * Works with any framebuffer (VESA, GOP, etc.)
 * 
 * Coordinate system: Origin (0,0) at top-left, X increases right,
 * Y increases down (standard screen coordinates).
 */

/* Color utilities */
#define GFX_RGB(r, g, b)       ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))
#define GFX_RGBA(r, g, b, a)   ((uint32_t)(((a) << 24) | ((r) << 16) | ((g) << 8) | (b)))

/* Standard colors */
#define GFX_COLOR_BLACK        0x000000
#define GFX_COLOR_WHITE        0xffffff
#define GFX_COLOR_RED          0xff0000
#define GFX_COLOR_GREEN        0x00ff00
#define GFX_COLOR_BLUE         0x0000ff
#define GFX_COLOR_YELLOW       0xffff00
#define GFX_COLOR_CYAN         0x00ffff
#define GFX_COLOR_MAGENTA      0xff00ff
#define GFX_COLOR_ORANGE       0xffa500
#define GFX_COLOR_GRAY         0x808080
#define GFX_COLOR_LIGHT_GRAY   0xc0c0c0
#define GFX_COLOR_DARK_GRAY    0x404040

/* Rectangle structure */
struct gfx_rect {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

/* Point structure */
struct gfx_point {
    int32_t x;
    int32_t y;
};

/* Line parameters */
struct gfx_line {
    int32_t x1, y1;
    int32_t x2, y2;
};

/* Circle parameters */
struct gfx_circle {
    int32_t cx, cy;
    int32_t radius;
};

/* Bitmap structure (monochrome) */
struct gfx_bitmap {
    uint32_t width;
    uint32_t height;
    const uint8_t *data;        /* Row-major, 1 bit per pixel */
    uint32_t fg_color;
    uint32_t bg_color;
};

/* Text rendering parameters */
struct gfx_text {
    int32_t x;
    int32_t y;
    const char *text;
    uint32_t color;
    uint32_t bg_color;          /* GFX_COLOR_NONE for transparent */
    bool bold;
};

/**
 * gfx_init - Initialize graphics subsystem
 * 
 * Must be called after video_init()
 * 
 * Returns: 0 on success
 */
int gfx_init(void);

/**
 * gfx_is_initialized - Check if graphics subsystem is ready
 */
bool gfx_is_initialized(void);

/**
 * gfx_draw_pixel - Draw a single pixel
 * 
 * Coordinates outside screen bounds are ignored.
 * 
 * x, y: Pixel coordinates
 * color: RGB color (alpha ignored for 16/24 bpp)
 */
void gfx_draw_pixel(int32_t x, int32_t y, uint32_t color);

/**
 * gfx_draw_pixel_blend - Draw pixel with alpha blending
 * 
 * Only works in 32-bit mode with alpha channel.
 * For 16/24 bpp, behaves like gfx_draw_pixel()
 */
void gfx_draw_pixel_blend(int32_t x, int32_t y, uint32_t color);

/**
 * gfx_get_pixel - Read pixel color from framebuffer
 * 
 * Returns: RGB color at (x, y), or GFX_COLOR_BLACK if out of bounds
 */
uint32_t gfx_get_pixel(int32_t x, int32_t y);

/**
 * gfx_draw_line - Draw a line using Bresenham's algorithm
 * 
 * Supports any angle, including vertical, horizontal, and diagonal.
 */
void gfx_draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color);

/**
 * gfx_draw_rect - Draw rectangle outline
 * 
 * x, y: Top-left corner
 * w, h: Dimensions (w or h <= 0 is ignored)
 */
void gfx_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

/**
 * gfx_fill_rect - Fill rectangle with solid color
 */
void gfx_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

/**
 * gfx_draw_circle - Draw circle outline (midpoint algorithm)
 */
void gfx_draw_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color);

/**
 * gfx_fill_circle - Fill circle with solid color
 */
void gfx_fill_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color);

/**
 * gfx_draw_ellipse - Draw ellipse outline
 */
void gfx_draw_ellipse(int32_t cx, int32_t cy, int32_t rx, int32_t ry, uint32_t color);

/**
 * gfx_fill_ellipse - Fill ellipse with solid color
 */
void gfx_fill_ellipse(int32_t cx, int32_t cy, int32_t rx, int32_t ry, uint32_t color);

/**
 * gfx_draw_bitmap - Draw monochrome bitmap
 * 
 * data: 1-bit row-major bitmap (1 = fg_color, 0 = bg_color)
 * If bg_color == GFX_COLOR_NONE, transparent background
 */
void gfx_draw_bitmap(int32_t x, int32_t y, const struct gfx_bitmap *bmp);

/**
 * gfx_copy_region - Copy rectangular region within framebuffer
 * 
 * Used for scrolling, window movement, etc.
 */
void gfx_copy_region(int32_t src_x, int32_t src_y,
                     int32_t dst_x, int32_t dst_y,
                     int32_t w, int32_t h);

/**
 * gfx_clear - Clear entire screen to color
 */
void gfx_clear(uint32_t color);

/**
 * gfx_get_screen_width - Get screen width
 */
static inline uint32_t gfx_get_screen_width(void) {
    return video_get_width();
}

/**
 * gfx_get_screen_height - Get screen height
 */
static inline uint32_t gfx_get_screen_height(void) {
    return video_get_height();
}

/* Double buffering support */

/**
 * gfx_swap_buffers - Swap front and back buffers
 * 
 * Only available if double_buffered is true in video_info.
 * For single-buffered systems, this is a no-op.
 */
void gfx_swap_buffers(void);

/**
 * gfx_has_double_buffer - Check if double buffering is available
 */
bool gfx_has_double_buffer(void);

/* Dirty rectangle tracking for partial updates */

/**
 * gfx_mark_dirty - Mark region as needing redraw
 */
void gfx_mark_dirty(int32_t x, int32_t y, int32_t w, int32_t h);

/**
 * gfx_flush_dirty - Redraw all dirty regions
 * 
 * Call after batch operations to minimize fb writes.
 */
void gfx_flush_dirty(void);

/**
 * gfx_get_dirty_rect - Get bounding box of all dirty regions
 * 
 * Returns: true if there are dirty regions, false if clean
 */
bool gfx_get_dirty_rect(struct gfx_rect *rect);

/**
 * gfx_clear_dirty - Clear dirty region list
 */
void gfx_clear_dirty(void);

/* Backwards compatibility with old API */
static inline int framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t rgb) {
    if (!gfx_is_initialized()) return -1;
    gfx_draw_pixel((int32_t)x, (int32_t)y, rgb);
    return 0;
}

static inline int framebuffer_fillrect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb) {
    if (!gfx_is_initialized()) return -1;
    gfx_fill_rect((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, rgb);
    return 0;
}

#endif /* TNU_GFX_H */