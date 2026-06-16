/**
 * @file video.c
 * @brief Video subsystem - VESA/GOP framebuffer driver
 * 
 * Supports:
 * - Multiboot2 VESA framebuffer (GRUB legacy boot)
 * - UEFI GOP framebuffer (UEFI boot)
 * - VGA text mode fallback
 */

#include <tnu/video.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/multiboot2.h>
#include <tnu/string.h>
#include <arch/cpu.h>

/* Video state */
static struct video_info video;
static bool video_initialized = false;

/* Forward declarations */
static int parse_vesa_framebuffer(const struct multiboot_tag_framebuffer *fb_tag);
static int parse_uefi_gop_framebuffer(void);
static int setup_framebuffer_mapping(void);

/**
 * video_init - Initialize video subsystem
 */
int video_init(void)
{
    memset(&video, 0, sizeof(video));
    
    /* Default to VGA text mode */
    video.mode_type = VIDEO_MODE_VGA_TEXT;
    video.width = 80;
    video.height = 25;
    video.pitch = 160;
    video.bits_per_pixel = 16;
    video.bytes_per_pixel = 2;
    video.pixel_format = VIDEO_PIXEL_FORMAT_RGB;
    video.framebuffer_addr = 0xb8000;
    video.write_combining = false;
    video.double_buffered = false;
    
    /* Try to detect framebuffer from boot info */
    const struct boot_info *boot = boot_info_get();
    
    if (boot && boot->framebuffer_addr && boot->framebuffer_addr != 0xb8000) {
        /* Multiboot2 provided framebuffer info */
        if (parse_vesa_framebuffer(NULL) == 0) {
            log_info("video", "Framebuffer detected from boot info");
        }
    }
    
    /* Try UEFI GOP if no framebuffer yet */
    if (video.mode_type == VIDEO_MODE_VGA_TEXT) {
        parse_uefi_gop_framebuffer();
    }
    
    /* Map framebuffer memory */
    if (video.mode_type != VIDEO_MODE_VGA_TEXT) {
        setup_framebuffer_mapping();
    }
    
    /* Log video mode info */
    log_info("video", "Video initialized:");
    log_info("video", "  Mode: %s", video_get_mode_name());
    log_info("video", "  Resolution: %ux%ux%u",
             video.width, video.height, video.bits_per_pixel);
    log_info("video", "  Pitch: %u bytes", video.pitch);
    log_info("video", "  Framebuffer: %p", (void *)video.framebuffer_addr);
    log_info("video", "  Pixel format: %s", 
             video.pixel_format == VIDEO_PIXEL_FORMAT_XRGB ? "XRGB" :
             video.pixel_format == VIDEO_PIXEL_FORMAT_XBGR ? "XBGR" :
             video.pixel_format == VIDEO_PIXEL_FORMAT_RGB ? "RGB" : "BGR");
    
    video_initialized = true;
    return 0;
}

/**
 * parse_vesa_framebuffer - Parse VESA framebuffer from Multiboot2
 */
static int parse_vesa_framebuffer(__attribute__((unused)) const struct multiboot_tag_framebuffer *fb_tag)
{
    const struct boot_info *boot = boot_info_get();
    
    if (!boot || !boot->framebuffer_addr || boot->framebuffer_bpp < 16) {
        return -1;
    }
    
    video.mode_type = VIDEO_MODE_VESA;
    video.framebuffer_addr = (uintptr_t)boot->framebuffer_addr;
    video.width = boot->framebuffer_width;
    video.height = boot->framebuffer_height;
    video.pitch = boot->framebuffer_pitch;
    video.bits_per_pixel = boot->framebuffer_bpp;
    
    /* Calculate bytes per pixel */
    video.bytes_per_pixel = (video.bits_per_pixel + 7) / 8;
    
    /* Calculate framebuffer size */
    video.framebuffer_size = (size_t)video.pitch * video.height;
    
    /* Determine pixel format based on BPP */
    if (video.bits_per_pixel == 32) {
        /* Most VESA modes use XRGB (little-endian: 0x00RRGGBB) */
        video.pixel_format = VIDEO_PIXEL_FORMAT_XRGB;
        video.red_pos = 16;
        video.red_size = 8;
        video.green_pos = 8;
        video.green_size = 8;
        video.blue_pos = 0;
        video.blue_size = 8;
        video.alpha_pos = 24;
        video.alpha_size = 0;  /* Unused in XRGB */
    } else if (video.bits_per_pixel == 24) {
        video.pixel_format = VIDEO_PIXEL_FORMAT_BGR;
        video.red_pos = 16;
        video.red_size = 8;
        video.green_pos = 8;
        video.green_size = 8;
        video.blue_pos = 0;
        video.blue_size = 8;
        video.alpha_pos = 0;
        video.alpha_size = 0;
    } else if (video.bits_per_pixel == 16) {
        video.pixel_format = VIDEO_PIXEL_FORMAT_RGB;
        /* RGB565 format */
        video.red_pos = 11;
        video.red_size = 5;
        video.green_pos = 5;
        video.green_size = 6;
        video.blue_pos = 0;
        video.blue_size = 5;
        video.alpha_pos = 0;
        video.alpha_size = 0;
    } else {
        return -1;  /* Unsupported BPP */
    }
    
    video.write_combining = true;
    return 0;
}

/**
 * parse_uefi_gop_framebuffer - Try to detect UEFI GOP framebuffer
 * 
 * Note: This is a placeholder. In a full implementation, this would
 * parse UEFI system table to find GOP and query video modes.
 * For now, we rely on Multiboot2 to pass GOP info.
 */
static int parse_uefi_gop_framebuffer(void)
{
    /* 
     * In a full implementation, we would:
     * 1. Find UEFI System Table from boot info
     * 2. Locate GOP protocol
     * 3. Query available modes
     * 4. Select desired resolution
     * 
     * For now, assume Multiboot2 has already provided this info
     * or we're in BIOS mode with VESA.
     */
    const struct boot_info *boot = boot_info_get();
    
    if (boot && boot->framebuffer_addr && boot->framebuffer_bpp >= 16) {
        /* Check if this looks like GOP (usually 32-bit) */
        if (boot->framebuffer_bpp == 32) {
            video.mode_type = VIDEO_MODE_UEFI_GOP;
            log_info("video", "Detected UEFI GOP framebuffer");
        }
    }
    
    return (video.mode_type != VIDEO_MODE_VGA_TEXT) ? 0 : -1;
}

/**
 * setup_framebuffer_mapping - Map framebuffer into virtual address space
 */
static int setup_framebuffer_mapping(void)
{
    if (video.mode_type == VIDEO_MODE_VGA_TEXT) {
        return 0;
    }
    
    if (!video.framebuffer_size || video.framebuffer_size > 256 * 1024 * 1024) {
        log_warn("video", "Invalid framebuffer size: %zu", video.framebuffer_size);
        return -1;
    }
    
    /* Map framebuffer with Write-Combining for performance */
    uint32_t flags = VMM_FLAG_WRITABLE;
    if (video.write_combining) {
        flags |= VMM_FLAG_WC;
    }
    
    if (vmm_map_range_identity(video.framebuffer_addr, 
                                video.framebuffer_size, 
                                flags) < 0) {
        log_warn("video", "Failed to map framebuffer at %p", 
                 (void *)video.framebuffer_addr);
        /* Fall back to non-WC mapping */
        if (vmm_map_range_identity(video.framebuffer_addr,
                                    video.framebuffer_size,
                                    VMM_FLAG_WRITABLE) < 0) {
            log_warn("video", "Failed to map framebuffer at all");
            return -1;
        }
        video.write_combining = false;
    }
    
    log_info("video", "Framebuffer mapped %s",
             video.write_combining ? "with Write-Combining" : "without WC");
    return 0;
}

/* Public API implementations */

const struct video_info *video_get_info(void)
{
    return &video;
}

bool video_is_graphics(void)
{
    return video.mode_type != VIDEO_MODE_VGA_TEXT;
}

uint32_t video_get_width(void)
{
    return video.width;
}

uint32_t video_get_height(void)
{
    return video.height;
}

uint32_t video_get_pitch(void)
{
    return video.pitch;
}

uint8_t video_get_bpp(void)
{
    return video.bits_per_pixel;
}

uintptr_t video_get_fb_addr(void)
{
    return video.framebuffer_addr;
}

const char *video_get_mode_name(void)
{
    switch (video.mode_type) {
        case VIDEO_MODE_VGA_TEXT: return "VGA_TEXT";
        case VIDEO_MODE_VESA:     return "VESA";
        case VIDEO_MODE_UEFI_GOP: return "UEFI_GOP";
        default:                  return "UNKNOWN";
    }
}