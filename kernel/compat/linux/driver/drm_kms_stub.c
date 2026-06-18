/*
 * drm_kms_stub.c - Minimal DRM/KMS stub for Linux driver runtime
 *
 * This provides basic DRM/KMS infrastructure stubs to satisfy Linux drivers
 * that expect DRM APIs (like i915), forwarding actual rendering to TNU's
 * native framebuffer (/dev/fb0).
 *
 * Full DRM/KMS implementation requires:
 * - Mode setting (CRTC/encoder/connector management)
 * - GEM (Graphics Execution Manager) memory management
 * - Command submission and synchronization
 * - i915-specific hardware programming
 *
 * This stub provides minimal scaffolding for future development.
 */

#include <tnu/types.h>
#include <tnu/log.h>
#include <tnu/framebuffer.h>
#include <tnu/string.h>

/* DRM device state */
struct drm_device {
    uint32_t dev_id;
    uint16_t vendor_id;
    uint16_t device_id;
    bool initialized;
    void *driver_private;
};

/* DRM mode configuration */
struct drm_mode_config {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
};

static struct drm_device drm_dev;
static struct drm_mode_config drm_mode;

int drm_kms_init(void)
{
    const struct framebuffer_info *fb = framebuffer_info();
    if (!framebuffer_is_graphics()) {
        log_warn("drm", "graphics mode not available");
        return -1;
    }

    memset(&drm_dev, 0, sizeof(drm_dev));
    drm_dev.initialized = true;

    drm_mode.width = fb->width;
    drm_mode.height = fb->height;
    drm_mode.bpp = fb->bpp;

    log_info("drm", "KMS stub initialized: %ux%u %ubpp (forwarding to TNU fb0)",
             drm_mode.width, drm_mode.height, drm_mode.bpp);
    return 0;
}

int drm_i915_probe(uint16_t vendor_id, uint16_t device_id)
{
    if (vendor_id != 0x8086) {
        return -1; /* Not Intel */
    }

    drm_dev.vendor_id = vendor_id;
    drm_dev.device_id = device_id;

    log_info("drm", "i915: Intel GPU %04x detected, using KMS stub + fb0 backend",
             device_id);

    /* Initialize basic KMS infrastructure */
    if (drm_kms_init() < 0) {
        log_warn("drm", "i915: KMS init failed, falling back to legacy framebuffer");
        return -1;
    }

    return 0;
}

void drm_kms_get_mode(uint32_t *width, uint32_t *height, uint32_t *bpp)
{
    if (width) *width = drm_mode.width;
    if (height) *height = drm_mode.height;
    if (bpp) *bpp = drm_mode.bpp;
}

bool drm_kms_is_available(void)
{
    return drm_dev.initialized;
}

/* Stubs for future DRM/KMS API implementation */

int drm_mode_setcrtc(uint32_t crtc_id, uint32_t fb_id, uint32_t x, uint32_t y)
{
    (void)crtc_id;
    (void)fb_id;
    (void)x;
    (void)y;
    log_warn("drm", "mode_setcrtc not fully implemented (stub)");
    return 0; /* Pretend success, use existing fb0 */
}

int drm_mode_getresources(void *buf, size_t size)
{
    (void)buf;
    (void)size;
    log_warn("drm", "mode_getresources not fully implemented (stub)");
    return 0;
}

int drm_gem_create(size_t size, uint32_t *handle)
{
    (void)size;
    if (handle) *handle = 1; /* Dummy handle */
    log_warn("drm", "gem_create not fully implemented (stub)");
    return 0;
}

int drm_gem_mmap(uint32_t handle, void **addr)
{
    (void)handle;
    if (addr) *addr = NULL;
    log_warn("drm", "gem_mmap not fully implemented (stub)");
    return -1;
}

/* i915-specific stubs */

int i915_gem_execbuffer(void *batch, size_t size)
{
    (void)batch;
    (void)size;
    log_warn("drm", "i915_gem_execbuffer not implemented (no GPU command submission)");
    return -1;
}

int i915_set_power_state(int state)
{
    (void)state;
    /* Power management stub - just succeed */
    return 0;
}
