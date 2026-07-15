/*
 * pcsx_rearmed standalone platform backend for the Iriver/AK "EROS Q"
 * (Ingenic X1000, mipsel musl, no GPU, 320x240 SLCD).
 *
 * Video : raw-KMS 320x240 RGB565 dumb buffer (drm_out.c). The frontend renders
 *         a PSX frame into our shadow buffer (cspace_blit, pl_plat_* left NULL);
 *         we nearest-neighbour scale it to the panel and SETCRTC-present.
 * Audio : dfsound SPU plugin + ALSA backend (SOUND_DRIVERS=alsa) — not here.
 * Input : libpicofe in_evdev over /dev/input/event* (on-device gpio-keys +
 *         an optional BT gamepad's BTN_* codes).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <linux/input.h>

#include "libpicofe/input.h"
#include "libpicofe/linux/in_evdev.h"
#include "libpicofe/plat.h"
#include "libpicofe/menu.h"
#include "plugin_lib.h"
#include "main.h"
#include "menu.h"
#include "plat.h"
#include "drm_out.h"

#define PANEL_W DRM_OUT_W
#define PANEL_H DRM_OUT_H

/* Normally defined by frontend/pcsxr-threads.c (built only with USE_RTHREADS).
 * We build single-threaded (single-core X1000, 32MB RAM), so define it here as
 * 1 core -> the "SPU multithreading" menu option stays disabled. */
int pcsxr_sthread_core_count = 1;

/* --- input -------------------------------------------------------------- */
/* Only ~6 keys + a scroll wheel exist on the device (x1000-erosq gpio-keys),
 * so the physical map is a minimal-but-playable pad; a paired BT gamepad's
 * BTN_* codes reach the full PSX pad + the emu menu. */
static const struct in_default_bind in_evdev_defbinds[] = {
	/* on-device gpio-keys */
	{ KEY_VOLUMEUP,     IN_BINDTYPE_PLAYER12, DKEY_UP },
	{ KEY_VOLUMEDOWN,   IN_BINDTYPE_PLAYER12, DKEY_DOWN },
	{ KEY_PREVIOUSSONG, IN_BINDTYPE_PLAYER12, DKEY_LEFT },
	{ KEY_NEXTSONG,     IN_BINDTYPE_PLAYER12, DKEY_RIGHT },
	{ KEY_PLAYPAUSE,    IN_BINDTYPE_PLAYER12, DKEY_CROSS },
	{ KEY_BACK,         IN_BINDTYPE_PLAYER12, DKEY_CIRCLE },
	{ KEY_MENU,         IN_BINDTYPE_PLAYER12, DKEY_START },
	/* BT gamepad: face + shoulders + a d-pad (key-style, HID BTN_DPAD_*) */
	{ BTN_DPAD_UP,      IN_BINDTYPE_PLAYER12, DKEY_UP },
	{ BTN_DPAD_DOWN,    IN_BINDTYPE_PLAYER12, DKEY_DOWN },
	{ BTN_DPAD_LEFT,    IN_BINDTYPE_PLAYER12, DKEY_LEFT },
	{ BTN_DPAD_RIGHT,   IN_BINDTYPE_PLAYER12, DKEY_RIGHT },
	{ BTN_A,            IN_BINDTYPE_PLAYER12, DKEY_CROSS },
	{ BTN_B,            IN_BINDTYPE_PLAYER12, DKEY_CIRCLE },
	{ BTN_X,            IN_BINDTYPE_PLAYER12, DKEY_SQUARE },
	{ BTN_Y,            IN_BINDTYPE_PLAYER12, DKEY_TRIANGLE },
	{ BTN_TL,           IN_BINDTYPE_PLAYER12, DKEY_L1 },
	{ BTN_TR,           IN_BINDTYPE_PLAYER12, DKEY_R1 },
	{ BTN_TL2,          IN_BINDTYPE_PLAYER12, DKEY_L2 },
	{ BTN_TR2,          IN_BINDTYPE_PLAYER12, DKEY_R2 },
	{ BTN_SELECT,       IN_BINDTYPE_PLAYER12, DKEY_SELECT },
	{ BTN_START,        IN_BINDTYPE_PLAYER12, DKEY_START },
	{ BTN_MODE,         IN_BINDTYPE_EMU,      SACTION_ENTER_MENU },
	{ 0, 0, 0 }
};

/* menu navigation (PBTN_*) — mirrors the pad so the frontend menu is usable
 * on-device without a keyboard. */
static const struct menu_keymap key_pbtn_map[] = {
	{ KEY_VOLUMEUP,     PBTN_UP },
	{ KEY_VOLUMEDOWN,   PBTN_DOWN },
	{ KEY_PREVIOUSSONG, PBTN_LEFT },
	{ KEY_NEXTSONG,     PBTN_RIGHT },
	{ KEY_PLAYPAUSE,    PBTN_MOK },
	{ KEY_BACK,         PBTN_MBACK },
	{ KEY_MENU,         PBTN_MENU },
	/* BT gamepad */
	{ BTN_DPAD_UP,      PBTN_UP },
	{ BTN_DPAD_DOWN,    PBTN_DOWN },
	{ BTN_DPAD_LEFT,    PBTN_LEFT },
	{ BTN_DPAD_RIGHT,   PBTN_RIGHT },
	{ BTN_A,            PBTN_MOK },
	{ BTN_B,            PBTN_MBACK },
	{ BTN_MODE,         PBTN_MENU },
};

static const struct in_pdata erosq_evdev_pdata = {
	.defbinds  = in_evdev_defbinds,
	.key_map   = key_pbtn_map,
	.kmap_size = sizeof(key_pbtn_map) / sizeof(key_pbtn_map[0]),
};

/* --- video -------------------------------------------------------------- */
static void *shadow_fb;      /* game render target; frontend fills at psx_w x psx_h */
static void *menu_fb;        /* 320x240 menu draw target (g_menuscreen_ptr) */
static uint16_t *drm_buf;    /* mapped 320x240 RGB565 scanout */
static int drm_stride;       /* scanout row stride in pixels */
static int psx_w = 320, psx_h = 240;

/* ponytail: SIGUSR1 -> dump current PSX frame (shadow_fb, RGB565) as a PPM so
 * headless perf runs can SEE the game state. Debug tool; flag set in handler,
 * write happens in the flip path (not signal context). writepng is stubbed
 * (no libpng) in this build, so PPM keeps it dependency-free. */
static volatile sig_atomic_t g_want_shot;
static void shot_handler(int sig) { (void)sig; g_want_shot = 1; }
static void dump_ppm(const uint16_t *src, int w, int h)
{
	FILE *f = fopen("/media/sd/psx/shot.ppm", "wb");
	int i, n = w * h;
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (i = 0; i < n; i++) {
		uint16_t p = src[i];
		unsigned char rgb[3] = {
			(unsigned char)(((p >> 11) & 0x1f) << 3),
			(unsigned char)(((p >> 5)  & 0x3f) << 2),
			(unsigned char)(( p        & 0x1f) << 3),
		};
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	fprintf(stderr, "shot: wrote /media/sd/psx/shot.ppm %dx%d\n", w, h);
}

static void get_layer_pos(int *x, int *y, int *w, int *h)
{
	*x = 0; *y = 0; *w = PANEL_W; *h = PANEL_H;
}

/* nearest-neighbour scale packed RGB565 src(sw x sh) -> panel, then present */
static void present_scaled(const uint16_t *src, int sw, int sh)
{
	int x, y;
	if (sw == PANEL_W && sh == PANEL_H) {
		for (y = 0; y < PANEL_H; y++)
			memcpy(drm_buf + (size_t)y * drm_stride, src + (size_t)y * sw, PANEL_W * 2);
	} else {
		for (y = 0; y < PANEL_H; y++) {
			const uint16_t *s = src + (size_t)(y * sh / PANEL_H) * sw;
			uint16_t *d = drm_buf + (size_t)y * drm_stride;
			for (x = 0; x < PANEL_W; x++)
				d[x] = s[x * sw / PANEL_W];
		}
	}
	drm_present();
}

void plat_init(void)
{
	int shadow_size = 1024 * 512 * 2;   /* max PSX hi-res, matches plat_sdl */

	shadow_fb    = calloc(1, shadow_size);
	menu_fb      = calloc(1, PANEL_W * PANEL_H * 2);
	g_menubg_ptr = calloc(1, PANEL_W * PANEL_H * 2);  /* menu composites bg -> menuscreen */
	if (!shadow_fb || !menu_fb || !g_menubg_ptr) {
		fprintf(stderr, "plat_init: OOM\n");
		exit(1);
	}

	/* must be set before menu_init() runs (main.c calls plat_init then menu_init) */
	g_menuscreen_w  = PANEL_W;
	g_menuscreen_h  = PANEL_H;
	g_menuscreen_pp = PANEL_W;

	if (drm_open() != 0) {
		fprintf(stderr, "plat_init: drm_open failed (is another DRM master running? stop erosq-ui)\n");
		exit(1);
	}
	drm_buf = drm_pixels(&drm_stride);

	signal(SIGUSR1, shot_handler);   /* ponytail: kill -USR1 -> shot.ppm */

	in_evdev_init(&erosq_evdev_pdata);
	in_probe();

	pl_rearmed_cbs.only_16bpp = 1;
	pl_rearmed_cbs.pl_get_layer_pos = get_layer_pos;
}

void plat_finish(void)
{
	drm_close();
	free(shadow_fb);    shadow_fb = NULL;
	free(menu_fb);      menu_fb = NULL;
}

void plat_gvideo_open(int is_pal)
{
}

void *plat_gvideo_set_mode(int *w, int *h, int *bpp)
{
	psx_w = *w;
	psx_h = *h;
	/* frontend renders RGB565 into shadow_fb (pl_plat_* stay NULL); we scale on flip */
	return shadow_fb;
}

void *plat_gvideo_flip(void)
{
	present_scaled(shadow_fb, psx_w, psx_h);
	if (g_want_shot) { g_want_shot = 0; dump_ppm(shadow_fb, psx_w, psx_h); }
	/* EROSQ_VID_LOG=1: confirm the frame path is live without a human watching */
	if (getenv("EROSQ_VID_LOG")) {
		static unsigned long n;
		if ((++n % 60) == 0)
			fprintf(stderr, "[vid] flip %lu  psx=%dx%d\n", n, psx_w, psx_h);
	}
	return shadow_fb;
}

void plat_gvideo_close(void)
{
}

void plat_video_menu_enter(int is_rom_loaded)
{
	pl_vout_buf = shadow_fb;
}

void plat_video_menu_begin(void)
{
	g_menuscreen_ptr = menu_fb;
}

void plat_video_menu_end(void)
{
	const uint16_t *src = menu_fb;   /* 320x240 RGB565 == panel: straight copy */
	int y;
	for (y = 0; y < PANEL_H; y++)
		memcpy(drm_buf + (size_t)y * drm_stride, src + (size_t)y * PANEL_W, PANEL_W * 2);
	drm_present();
	g_menuscreen_ptr = NULL;
}

void plat_video_menu_leave(void)
{
}

void *plat_prepare_screenshot(int *w, int *h, int *bpp)
{
	*w = psx_w;
	*h = psx_h;
	*bpp = 16;
	return shadow_fb;
}

void plat_trigger_vibrate(int pad, int low, int high)
{
}

void plat_minimize(void)
{
}
