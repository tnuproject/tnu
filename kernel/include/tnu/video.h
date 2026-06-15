#ifndef TNU_VIDEO_H
#define TNU_VIDEO_H

#include <tnu/types.h>

/**
 * Video subsystem initialization and configuration
 * 
 * Supports:
 * - VESA VBE framebuffer (BIOS legacy boot)
 * - UEFI GOP framebuffer (UEFI boot)
 * - VGA text mode (fallback)
 */

/* Video mode types */
enum video_mode_type {
    VIDEO_MODE_NONE = 0,
    VIDEO_MODE_VGA_TEXT,         /* Legacy VGA text mode 80x25 */
    VIDEO_MODE_VESA,             /* VESA VBE linear framebuffer */
    VIDEO_MODE_UEFI_GOP,         /* UEFI Graphics Output Protocol */
};

/* Pixel format */
enum video_pixel_format {
    VIDEO_PIXEL_FORMAT_INVALID = 0,
    VIDEO_PIXEL_FORMAT_RGB,     /* Red-Green-Blue */
    VIDEO_PIXEL_FORMAT_BGR,     /* Blue-Green-Red */
    VIDEO_PIXEL_FORMAT_XRGB,    /* 32-bit with 8-bit unused */
    VIDEO_PIXEL_FORMAT_XBGR,    /* 32-bit with 8-bit unused */
};

/* Framebuffer info - populated by boot loader */
struct video_info {
    /* Mode identification */
    enum video_mode_type mode_type;
    enum video_pixel_format pixel_format;
    
    /* Framebuffer address and size */
    uintptr_t       framebuffer_addr;
    size_t          framebuffer_size;     /* Total VRAM size */
    
    /* Screen dimensions */
    uint32_t        width;                /* Horizontal resolution */
    uint32_t        height;               /* Vertical resolution */
    uint32_t        pitch;                /* Bytes per scanline */
    
    /* Pixel format */
    uint8_t         bits_per_pixel;       /* BPP: 16, 24, 32 */
    uint8_t         bytes_per_pixel;      /* 2, 3, or 4 */
    
    /* Color masks (for packed pixel formats) */
    uint8_t         red_pos;
    uint8_t         red_size;
    uint8_t         green_pos;
    uint8_t         green_size;
    uint8_t         blue_pos;
    uint8_t         blue_size;
    uint8_t         alpha_pos;
    uint8_t         alpha_size;
    
    /* Capabilities */
    bool            write_combining;      /* WC mapping available */
    bool            double_buffered;      /* Swap buffer supported */
};

/**
 * video_init - Initialize video subsystem
 * 
 * Parses boot information from Multiboot2 or UEFI and configures
 * the framebuffer. Falls back to VGA text mode if no graphics
 * framebuffer is available.
 * 
 * Returns: 0 on success, -1 on failure
 */
int video_init(void);

/**
 * video_get_info - Get current video configuration
 * 
 * Returns: Pointer to video_info structure (never NULL after init)
 */
const struct video_info *video_get_info(void);

/**
 * video_is_graphics - Check if in graphics mode
 * 
 * Returns: true if framebuffer is available, false for VGA text
 */
bool video_is_graphics(void);

/**
 * video_get_width - Get screen width in pixels
 */
uint32_t video_get_width(void);

/**
 * video_get_height - Get screen height in pixels
 */
uint32_t video_get_height(void);

/**
 * video_get_pitch - Get bytes per scanline
 */
uint32_t video_get_pitch(void);

/**
 * video_get_bpp - Get bits per pixel
 */
uint8_t video_get_bpp(void);

/**
 * video_get_fb_addr - Get framebuffer linear address
 */
uintptr_t video_get_fb_addr(void);

/**
 * video_get_mode_name - Get human-readable mode name
 * 
 * Returns: Static string (e.g., "VESA", "GOP", "VGA_TEXT")
 */
const char *video_get_mode_name(void);

/* Backwards compatibility with old framebuffer API */
static inline const struct video_info *framebuffer_info(void) {
    return video_get_info();
}

static inline bool framebuffer_is_graphics(void) {
    return video_is_graphics();
}

static inline uint32_t framebuffer_get_width(void) {
    return video_get_width();
}

static inline uint32_t framebuffer_get_height(void) {
    return video_get_height();
}

static inline uint32_t framebuffer_get_pitch(void) {
    return video_get_pitch();
}

static inline uint8_t framebuffer_get_bpp(void) {
    return video_get_bpp();
}

static inline uintptr_t framebuffer_get_addr(void) {
    return video_get_fb_addr();
}

#endif /* TNU_VIDEO_H */