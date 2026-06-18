#ifndef TNU_DRM_KMS_H
#define TNU_DRM_KMS_H

#include <tnu/types.h>

/* Minimal DRM/KMS stub for Linux driver compatibility */

int drm_kms_init(void);
int drm_i915_probe(uint16_t vendor_id, uint16_t device_id);
void drm_kms_get_mode(uint32_t *width, uint32_t *height, uint32_t *bpp);
bool drm_kms_is_available(void);

/* DRM mode setting stubs */
int drm_mode_setcrtc(uint32_t crtc_id, uint32_t fb_id, uint32_t x, uint32_t y);
int drm_mode_getresources(void *buf, size_t size);

/* DRM GEM (Graphics Execution Manager) stubs */
int drm_gem_create(size_t size, uint32_t *handle);
int drm_gem_mmap(uint32_t handle, void **addr);

/* i915-specific stubs */
int i915_gem_execbuffer(void *batch, size_t size);
int i915_set_power_state(int state);

#endif /* TNU_DRM_KMS_H */
