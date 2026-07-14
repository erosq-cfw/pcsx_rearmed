#ifndef DRM_OUT_H_INCLUDED
#define DRM_OUT_H_INCLUDED
#include <stdint.h>
/* Minimal KMS output for the erosq SLCD: one 320x240 RGB565 dumb buffer on the
 * first connector/crtc. No libdrm (raw ioctls). Proven on doom/gbemu. */
#define DRM_OUT_W 320
#define DRM_OUT_H 240
int drm_open(void);                    /* modeset; 0 ok, -1 fail */
uint16_t *drm_pixels(int *stride_px);  /* mapped RGB565 buffer + row stride (px) */
int drm_present(void);                 /* push current buffer (SETCRTC same mode); 0 ok, -1 fail */
void drm_close(void);
#endif
