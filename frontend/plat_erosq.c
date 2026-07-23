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
#include <sys/time.h>
#include <ucontext.h>
#include <linux/input.h>
#include <tinyalsa/asoundlib.h>

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

/* ponytail: coarse frame-phase profiler (EROSQ_PROF=1). Accumulators are summed
 * by the GPU plugin (renderer_do_cmd_list) and here (present); plugin_lib prints
 * the per-second split. ns via CLOCK_MONOTONIC. Stripped after the perf hunt. */
#include <time.h>
int g_prof_on;
unsigned long long g_prof_gpu_ns, g_prof_present_ns, g_prof_spu_ns;
unsigned long long prof_now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned long long)t.tv_sec * 1000000000ull + t.tv_nsec;
}

/* pcsxr_sthread_core_count now comes from frontend/pcsxr-threads.c: we build
 * with USE_ASYNC_CDROM=1 (CD prefetch thread hides SD/CHD read latency that
 * caused PaRappa XA-audio underruns); other async options stay off. */

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
	/* in_evdev converts BOTH the analog stick (ABS_X/Y) and the HAT0 d-pad
	 * (ABS_HAT0X/Y) to arrow keycodes; this 8bitdo sends its d-pad as HAT0,
	 * not BTN_DPAD_*. Bind the arrows so stick + d-pad reach the pad. */
	{ KEY_UP,           IN_BINDTYPE_PLAYER12, DKEY_UP },
	{ KEY_DOWN,         IN_BINDTYPE_PLAYER12, DKEY_DOWN },
	{ KEY_LEFT,         IN_BINDTYPE_PLAYER12, DKEY_LEFT },
	{ KEY_RIGHT,        IN_BINDTYPE_PLAYER12, DKEY_RIGHT },
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
	{ KEY_UP,           PBTN_UP },
	{ KEY_DOWN,         PBTN_DOWN },
	{ KEY_LEFT,         PBTN_LEFT },
	{ KEY_RIGHT,        PBTN_RIGHT },
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
static int g_direct;         /* present fusion: frontend renders straight into drm_buf */

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

/* ponytail: EROSQ_SAMPLE=1 -> SIGPROF PC sampler (1kHz). Buckets each sample as
 * JIT (inside the mipsrec code cache = guest-code execution) vs native, and logs
 * native host PCs for offline nm symbolization (binary is non-PIE so PC == link
 * address). Answers "where does core time actually go" — GTE math / mem helpers /
 * emitted block bodies — before investing in any core-body optimization.
 * kill -USR2 dumps /media/sd/psx/samp.bin (serviced in the flip path, not signal ctx). */
extern void mipsrec_code_range(uintptr_t *lo, uintptr_t *hi);
static uintptr_t g_jit_lo, g_jit_hi;
static volatile unsigned long g_smp_jit, g_smp_nat;
#define SMP_MAX 262144
static unsigned g_smp_pc[SMP_MAX];
static volatile unsigned g_smp_n;
static volatile sig_atomic_t g_want_smpdump;
static void prof_sig(int sig, siginfo_t *si, void *ucv)
{
	(void)sig; (void)si;
	uintptr_t pc = (uintptr_t)((ucontext_t *)ucv)->uc_mcontext.pc;
	if (pc >= g_jit_lo && pc < g_jit_hi) {
		g_smp_jit++;
	} else {
		unsigned n = g_smp_n;
		g_smp_nat++;
		if (n < SMP_MAX) { g_smp_pc[n] = (unsigned)pc; g_smp_n = n + 1; }
	}
}
static void smpdump_handler(int sig) { (void)sig; g_want_smpdump = 1; }
static void smp_dump(void)
{
	FILE *f = fopen("/media/sd/psx/samp.bin", "wb");
	if (f) { fwrite(g_smp_pc, 4, g_smp_n, f); fclose(f); }
	fprintf(stderr, "SAMPLE jit=%lu nat=%lu (%.1f%% jit) dumped=%u\n",
		g_smp_jit, g_smp_nat,
		100.0 * g_smp_jit / (g_smp_jit + g_smp_nat + 1), g_smp_n);
}
static void sampler_init(void)
{
	struct sigaction sa;
	struct itimerval it;
	if (!getenv("EROSQ_SAMPLE")) return;
	mipsrec_code_range(&g_jit_lo, &g_jit_hi);
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = prof_sig;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGPROF, &sa, NULL);
	{	/* sigaction, not signal(): repeat dumps must survive whatever
		 * one-shot semantics the libc picks */
		struct sigaction sa2;
		memset(&sa2, 0, sizeof sa2);
		sa2.sa_handler = smpdump_handler;
		sa2.sa_flags = SA_RESTART;
		sigaction(SIGUSR2, &sa2, NULL);
	}
	it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 1000; /* 1 kHz */
	it.it_value = it.it_interval;
	setitimer(ITIMER_PROF, &it, NULL);
	fprintf(stderr, "SAMPLE: on, jit=[%p,%p)\n", (void *)g_jit_lo, (void *)g_jit_hi);
}

/* nearest-neighbour scale packed RGB565 src(sw x sh) -> panel, then present.
 * The naive form did an integer divide per output pixel (PANEL_W*PANEL_H =
 * 76800 divides/frame, ~30 cyc each on this MIPS core). Precompute the src
 * column/row index tables instead — rebuilt only when the source resolution
 * changes (rare: at mode set), so the inner loop is a pure gather. */
static int16_t xmap[PANEL_W], ymap[PANEL_H];
static int xmap_sw, ymap_sh;
static void present_scaled(const uint16_t *src, int sw, int sh)
{
	int x, y;
	unsigned long long _t0 = g_prof_on ? prof_now_ns() : 0;
	if (sw == PANEL_W && sh == PANEL_H) {
		/* 1:1 copy. musl memcpy is tuned for cached memory and runs ~3x slower
		 * than explicit word stores against the uncached scanout; copy 32 bits
		 * (two pixels) at a time instead. */
		for (y = 0; y < PANEL_H; y++) {
			const uint32_t *s32 = (const uint32_t *)(src + (size_t)y * sw);
			uint32_t *d32 = (uint32_t *)(drm_buf + (size_t)y * drm_stride);
			for (x = 0; x < PANEL_W / 2; x++)
				d32[x] = s32[x];
		}
	} else {
		if (sw != xmap_sw) {
			for (x = 0; x < PANEL_W; x++) xmap[x] = (int16_t)(x * sw / PANEL_W);
			xmap_sw = sw;
		}
		if (sh != ymap_sh) {
			for (y = 0; y < PANEL_H; y++) ymap[y] = (int16_t)(y * sh / PANEL_H);
			ymap_sh = sh;
		}
		/* drm_buf is an uncached dumb-buffer scanout: 16-bit stores cost a
		 * full bus cycle each. Pack two output pixels into one 32-bit store
		 * to halve write transactions (PANEL_W is even). */
		if (sw == 512 && PANEL_W == 320) {
			/* ponytail: the overwhelmingly common hi-res case (Crash et al).
			 * 512->320 is a fixed 8:5 ratio, so xmap[10k+j] == 16k+off[j] with
			 * off={0,1,3,4,6,8,9,11,12,14} (verified bit-identical to x*512/320
			 * for all 320 cols). Hardcoding the offsets kills the xmap[x] load
			 * that feeds the gather address — on this load-use-stall core that
			 * load->load dependency is the inner-loop cost. Output is identical
			 * to the general path below. 32 groups of 10-out/16-in tile exactly. */
			for (y = 0; y < PANEL_H; y++) {
				const uint16_t *s = src + (size_t)ymap[y] * sw;
				uint32_t *d32 = (uint32_t *)(drm_buf + (size_t)y * drm_stride);
				int g;
				for (g = 0; g < PANEL_W / 10; g++) {
					d32[0] = (uint32_t)s[0]  | ((uint32_t)s[1]  << 16);
					d32[1] = (uint32_t)s[3]  | ((uint32_t)s[4]  << 16);
					d32[2] = (uint32_t)s[6]  | ((uint32_t)s[8]  << 16);
					d32[3] = (uint32_t)s[9]  | ((uint32_t)s[11] << 16);
					d32[4] = (uint32_t)s[12] | ((uint32_t)s[14] << 16);
					d32 += 5; s += 16;
				}
			}
		} else
		for (y = 0; y < PANEL_H; y++) {
			const uint16_t *s = src + (size_t)ymap[y] * sw;
			uint32_t *d32 = (uint32_t *)(drm_buf + (size_t)y * drm_stride);
			for (x = 0; x < PANEL_W; x += 2)
				d32[x >> 1] = (uint32_t)s[xmap[x]] | ((uint32_t)s[xmap[x + 1]] << 16);
		}
	}
	drm_present();
	if (g_prof_on) g_prof_present_ns += prof_now_ns() - _t0;
}

/* ponytail: pin the CPU governor to 'performance' for the emulator's lifetime.
 * Measured: ondemand drops cpu0 to 504MHz (half clock) on ~30% of capped frames
 * because it reads the per-frame present/vsync wait as idle, stuttering heavy
 * scenes. Save the prior governor, restore on exit (plat_finish + atexit backstop
 * for hard exits). EROSQ_NOGOV=1 disables. */
#define GOV_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
static char g_saved_gov[32];
static void gov_write(const char *g)
{
	FILE *f = fopen(GOV_PATH, "w");
	if (!f) return;
	fputs(g, f);
	fclose(f);
}
static void gov_restore(void)
{
	if (g_saved_gov[0]) { gov_write(g_saved_gov); g_saved_gov[0] = 0; }
}
static void gov_pin_performance(void)
{
	FILE *f;
	char *nl;
	if (getenv("EROSQ_NOGOV")) return;
	f = fopen(GOV_PATH, "r");
	if (!f) return;
	if (fgets(g_saved_gov, sizeof g_saved_gov, f)) {
		nl = strchr(g_saved_gov, '\n'); if (nl) *nl = 0;
	}
	fclose(f);
	if (strcmp(g_saved_gov, "performance") == 0) { g_saved_gov[0] = 0; return; }
	gov_write("performance");
	atexit(gov_restore);
	fprintf(stderr, "gov: pinned performance (was '%s')\n", g_saved_gov);
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
	g_prof_on = getenv("EROSQ_PROF") ? 1 : 0;
	sampler_init();
	gov_pin_performance();

	in_evdev_init(&erosq_evdev_pdata);
	in_probe();

	pl_rearmed_cbs.only_16bpp = 1;
	pl_rearmed_cbs.pl_get_layer_pos = get_layer_pos;
}

void plat_finish(void)
{
	gov_restore();
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
	/* Present fusion: when the emulator's output already matches the panel
	 * (every NTSC 320x240 game, and every hi-res game once scale_hires's
	 * downscale blit reduces it to 320-wide), hand the frontend drm_buf so its
	 * (already fully-correct: bgr24, hi-res, wrap, HUD) blit lands straight in
	 * the scanout — deleting the shadow_fb round-trip and the scale-copy pass.
	 * Needs an unpadded 320-wide scanout so the frontend's dstride(=pl_vout_w=
	 * PANEL_W) equals drm_stride. Other modes (PAL 256h, non-scale_hires
	 * hi-res) keep the proven scale-on-flip path via shadow_fb. */
	g_direct = (*w == PANEL_W && *h == PANEL_H && drm_stride == PANEL_W);
	return g_direct ? (void *)drm_buf : shadow_fb;
}

void *plat_gvideo_flip(void)
{
	void *frame = g_direct ? (void *)drm_buf : shadow_fb;
	if (g_direct)
		drm_present();               /* frame is already in the scanout */
	else
		present_scaled(shadow_fb, psx_w, psx_h);
	if (g_want_shot) { g_want_shot = 0;
		dump_ppm(frame, g_direct ? PANEL_W : psx_w, g_direct ? PANEL_H : psx_h); }
	if (g_want_smpdump) { g_want_smpdump = 0; smp_dump(); }
	/* EROSQ_VID_LOG=1: confirm the frame path is live without a human watching */
	if (getenv("EROSQ_VID_LOG")) {
		static unsigned long n;
		if ((++n % 60) == 0)
			fprintf(stderr, "[vid] flip %lu  psx=%dx%d%s\n", n, psx_w, psx_h,
				g_direct ? " direct" : "");
	}
	return frame;
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
	*bpp = 16;
	if (g_direct) {   /* live frame is the scanout, not shadow_fb */
		*w = PANEL_W;
		*h = PANEL_H;
		return drm_buf;
	}
	*w = psx_w;
	*h = psx_h;
	return shadow_fb;
}

void plat_trigger_vibrate(int pad, int low, int high)
{
}

void plat_minimize(void)
{
}

/* DAC hardware volume via tinyalsa, mirroring erosq-ui's ui_mixer.c: card 0
 * control "Playback Volume", range 0..255, 2 channels, higher = louder. Read
 * live so the menu slider reflects whatever the music UI last set (the DAC
 * register persists while erosq-ui is stopped for the emulator). Open/close per
 * call — only hit on menu-open and each slider tick, so lifecycle-free is fine.
 * Exposed to the frontend menu under #ifdef EROSQ. Same code as picoarch. */
#define VOL_CARD 0
#define VOL_CTL  "Playback Volume"
static int vol_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int erosq_vol_get(void)
{
	struct mixer *m = mixer_open(VOL_CARD);
	struct mixer_ctl *c;
	int pct = -1;
	if (!m) return -1;
	c = mixer_get_ctl_by_name(m, VOL_CTL);
	if (c) {
		int lo = mixer_ctl_get_range_min(c), hi = mixer_ctl_get_range_max(c);
		int v = mixer_ctl_get_value(c, 0);
		if (hi > lo)
			pct = ((vol_clampi(v, lo, hi) - lo) * 100 + (hi - lo) / 2) / (hi - lo);
	}
	mixer_close(m);
	return pct;
}

void erosq_vol_set(int pct)
{
	struct mixer *m = mixer_open(VOL_CARD);
	struct mixer_ctl *c;
	if (!m) return;
	c = mixer_get_ctl_by_name(m, VOL_CTL);
	if (c) {
		int lo = mixer_ctl_get_range_min(c), hi = mixer_ctl_get_range_max(c);
		unsigned i, n = mixer_ctl_get_num_values(c);
		int v = lo + (vol_clampi(pct, 0, 100) * (hi - lo) + 50) / 100;
		for (i = 0; i < n; i++)
			mixer_ctl_set_value(c, i, v);
	}
	mixer_close(m);
}
