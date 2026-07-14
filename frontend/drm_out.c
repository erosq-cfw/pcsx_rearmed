/* Raw-DRM/KMS output for the erosq SLCD. Lifted verbatim in shape from
 * doom/src/drm_out.c (proven on this exact GC9308 panel): grab the first
 * connector+crtc, create ONE 320x240 RGB565 dumb fb, SETCRTC it (full
 * modeset -> erosq-drm pipe .enable), and re-SETCRTC the same mode/fb on each
 * present (the .update/DMA path). No libdrm. */
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include "drm_out.h"

static int fd = -1;
static uint32_t fb_id, crtc_id, conn_id, pitch;
static uint16_t *px;
static struct drm_mode_modeinfo mode;

int drm_open(void)
{
	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open card0"); return -1; }

	struct drm_mode_card_res res = {0};
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("getres0"); return -1; }
	uint32_t conn_ids[8] = {0}, crtc_ids[8] = {0}, enc_ids[8] = {0}, fb_ids[8] = {0};
	res.connector_id_ptr = (uintptr_t)conn_ids; res.count_connectors = 8;
	res.crtc_id_ptr = (uintptr_t)crtc_ids; res.count_crtcs = 8;
	res.encoder_id_ptr = (uintptr_t)enc_ids; res.count_encoders = 8;
	res.fb_id_ptr = (uintptr_t)fb_ids; res.count_fbs = 8;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("getres"); return -1; }
	if (!res.count_connectors || !res.count_crtcs) { fprintf(stderr, "no conn/crtc\n"); return -1; }
	conn_id = conn_ids[0]; crtc_id = crtc_ids[0];

	/* First GETCONNECTOR probes; the fill pass zeroes prop/encoder counts so the
	 * kernel won't write them into NULL pointers (-> EFAULT). */
	struct drm_mode_get_connector gc = { .connector_id = conn_id };
	struct drm_mode_modeinfo modes[16];
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc)) { perror("getconn"); return -1; }
	gc.modes_ptr = (uintptr_t)modes;
	if (gc.count_modes > 16) gc.count_modes = 16;
	gc.count_props = 0; gc.props_ptr = 0; gc.prop_values_ptr = 0;
	gc.count_encoders = 0; gc.encoders_ptr = 0;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc)) { perror("getconn2"); return -1; }
	if (!gc.count_modes) { fprintf(stderr, "no modes\n"); return -1; }
	mode = modes[0];

	struct drm_mode_create_dumb cd = { .width = DRM_OUT_W, .height = DRM_OUT_H, .bpp = 16 };
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("create_dumb"); return -1; }
	struct drm_mode_fb_cmd fbc = { .width = DRM_OUT_W, .height = DRM_OUT_H, .bpp = 16,
		.depth = 16, .pitch = cd.pitch, .handle = cd.handle };
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbc)) { perror("addfb"); return -1; }
	struct drm_mode_map_dumb md = { .handle = cd.handle };
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { perror("map_dumb"); return -1; }
	px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (px == MAP_FAILED) { perror("mmap"); px = NULL; return -1; }
	fb_id = fbc.fb_id; pitch = cd.pitch;

	/* Initial modeset. Don't swallow failure: if we're not DRM master yet
	 * (erosq splash/handoff race) SETCRTC fails, and reporting success would
	 * leave the emu running full-speed against a black panel. */
	if (drm_present() != 0) { fprintf(stderr, "drm_open: initial modeset failed\n"); return -1; }
	return 0;
}

uint16_t *drm_pixels(int *stride_px)
{
	if (stride_px) *stride_px = pitch / 2;
	return px;
}

int drm_present(void)
{
	struct drm_mode_crtc sc = { .crtc_id = crtc_id, .fb_id = fb_id,
		.set_connectors_ptr = (uintptr_t)&conn_id, .count_connectors = 1,
		.mode_valid = 1, .mode = mode };
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc)) { perror("setcrtc"); return -1; }
	return 0;
}

void drm_close(void)
{
	if (fd >= 0) close(fd);
	fd = -1; px = NULL;
}
