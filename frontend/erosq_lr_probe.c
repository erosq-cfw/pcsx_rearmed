/* Milestone-1 headless Lightrec proof harness (throwaway, not part of the port).
 * dlopen()s the pcsx_rearmed libretro core, boots real content through the
 * Lightrec dynarec with dummy A/V/input, runs N frames, and reports whether the
 * emulated CPU produced live, changing video output without crashing.
 *
 * Signal-free by design: a crash in JITted code kills the process with a signal;
 * the launch script reports exit status (128+sig). Progress is line-flushed so
 * partial output survives a crash and shows how far Lightrec got.
 *
 * Build (host cross): mipsel-...-gcc erosq_lr_probe.c -o erosq_lr_probe -ldl
 * Run (device):       ./erosq_lr_probe <content> [system_dir] [frames]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include "libretro.h"

/* diagnostic crash handler: report signal + fault addr + PC, and whether PC is
 * in a loaded object (core C code) or anonymous JIT memory (Lightrec-emitted) */
static void crash_handler(int sig, siginfo_t *si, void *uc)
{
    void *pc = NULL;
#if defined(__mips__)
    ucontext_t *u = uc;
    pc = (void *)(uintptr_t)u->uc_mcontext.pc;
#endif
    Dl_info di; memset(&di, 0, sizeof di);
    int in_obj = dladdr(pc, &di);
    fprintf(stderr,
        "\n*** CRASH sig=%d fault_addr=%p pc=%p ***\n", sig, si->si_addr, pc);
    if (in_obj && di.dli_fname)
        fprintf(stderr, "pc in object: %s  sym=%s+%ld\n", di.dli_fname,
                di.dli_sname ? di.dli_sname : "?",
                di.dli_saddr ? (long)((char*)pc - (char*)di.dli_saddr) : 0L);
    else
        fprintf(stderr, "pc NOT in any loaded object -> JIT-emitted code (Lightrec codegen)\n");
#if defined(__mips__)
    /* dump guest-host register file: which reg holds the garbage base? */
    static const char *rn[32] = {"zero","at","v0","v1","a0","a1","a2","a3",
        "t0","t1","t2","t3","t4","t5","t6","t7","s0","s1","s2","s3","s4","s5",
        "s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"};
    for (int i = 0; i < 32; i++)
        fprintf(stderr, "%s=%08lx%s", rn[i],
                (unsigned long)u->uc_mcontext.gregs[i], (i%4==3)?"\n":" ");
    /* dump code around PC to a file for offline objdump */
    if (pc) {
        int fd = open("/media/sd/psx/crashdump.bin", O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd >= 0) {
            char *base = (char *)pc - 128;
            write(fd, base, 256);
            close(fd);
            fprintf(stderr, "dumped 256B @ pc-128 (=%p) to crashdump.bin\n", base);
        }
    }
#endif
    fflush(stderr);
    _exit(120 + sig);
}

#ifdef DIRECT_LINK
/* Unused CD-drive-enumeration symbols the core .so normally leaves for the
 * libretro frontend to provide. We load file content, never a physical drive,
 * so these are never called — stub to satisfy the static link. */
void *dir_list_new(void) { return 0; }
char *strcasestr_retro__(void) { return 0; }
#endif

static const char *g_sysdir = "/media/sd/psx";
static unsigned g_vid_calls = 0, g_distinct = 0;
static uint32_t g_last_hash = 0;

/* cheap frame signature: sample-sum, enough to tell "changing" from "frozen/black" */
static uint32_t frame_hash(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data) return g_last_hash;               /* dupe frame */
    const uint8_t *p = data;
    uint32_t acc = 2166136261u;
    for (unsigned y = 0; y < h; y += 8)
        for (unsigned x = 0; x < w * 2; x += 16)
            acc = (acc ^ p[y * pitch + x]) * 16777619u;
    return acc;
}

static void cb_video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    g_vid_calls++;
    uint32_t hh = frame_hash(data, w, h, pitch);
    if (hh != g_last_hash) { g_distinct++; g_last_hash = hh; }
}
static void cb_audio_sample(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t cb_audio_batch(const int16_t *d, size_t frames) { (void)d; return frames; }
static void cb_input_poll(void) {}
static int16_t cb_input_state(unsigned p, unsigned d, unsigned i, unsigned id)
{ (void)p;(void)d;(void)i;(void)id; return 0; }

static void core_log(enum retro_log_level lvl, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[core] "); vfprintf(stderr, fmt, ap); va_end(ap);
}

static bool cb_env(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        /* accept whatever; core defaults to RGB565 which is our panel format */
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = g_sysdir;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        if (v->key && !strcmp(v->key, "pcsx_rearmed_drc")) {
            const char *e = getenv("PSX_DRC");
            v->value = (e && !strcmp(e, "disabled")) ? "disabled" : "enabled";
            return true;
        }
        return false;                       /* everything else: core defaults */
    }
    default:
        /* SET_* and unknowns: claim success where harmless, else ignore */
        if (cmd == RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS ||
            cmd == RETRO_ENVIRONMENT_SET_VARIABLES ||
            cmd == RETRO_ENVIRONMENT_SET_CONTROLLER_INFO ||
            cmd == RETRO_ENVIRONMENT_SET_GEOMETRY ||
            cmd == RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO)
            return true;
        return false;
    }
}

#define SYM(h,n) do { *(void**)(&n) = dlsym(h, #n); \
    if (!n) { fprintf(stderr, "missing symbol %s\n", #n); return 3; } } while (0)

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <content> [sysdir] [frames]\n", argv[0]); return 2; }
    const char *content = argv[1];
    if (argc >= 3) g_sysdir = argv[2];
    int frames = (argc >= 4) ? atoi(argv[3]) : 600;

#ifndef DIRECT_LINK
    void *h = dlopen("./pcsx_rearmed_libretro.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }

    void (*retro_set_environment)(retro_environment_t);
    void (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_audio_sample)(retro_audio_sample_t);
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t);
    void (*retro_set_input_state)(retro_input_state_t);
    void (*retro_init)(void);
    void (*retro_deinit)(void);
    bool (*retro_load_game)(const struct retro_game_info *);
    void (*retro_unload_game)(void);
    void (*retro_run)(void);
    void (*retro_get_system_av_info)(struct retro_system_av_info *);

    SYM(h, retro_set_environment);
    SYM(h, retro_set_video_refresh);
    SYM(h, retro_set_audio_sample);
    SYM(h, retro_set_audio_sample_batch);
    SYM(h, retro_set_input_poll);
    SYM(h, retro_set_input_state);
    SYM(h, retro_init);
    SYM(h, retro_deinit);
    SYM(h, retro_load_game);
    SYM(h, retro_unload_game);
    SYM(h, retro_run);
    SYM(h, retro_get_system_av_info);
#endif /* !DIRECT_LINK */

    retro_set_environment(cb_env);
    retro_set_video_refresh(cb_video);
    retro_set_audio_sample(cb_audio_sample);
    retro_set_audio_sample_batch(cb_audio_batch);
    retro_set_input_poll(cb_input_poll);
    retro_set_input_state(cb_input_state);

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);

    setvbuf(stdout, NULL, _IOLBF, 0);
    fprintf(stderr, "probe: init... (drc=%s)\n",
            (getenv("PSX_DRC") && !strcmp(getenv("PSX_DRC"),"disabled")) ? "disabled":"enabled");
    retro_init();

    struct retro_game_info info;
    memset(&info, 0, sizeof info);
    info.path = content;                 /* path-based load; core reads via VFS */
    fprintf(stderr, "probe: load_game(%s)...\n", content);
    if (!retro_load_game(&info)) { fprintf(stderr, "LOAD FAILED\n"); return 4; }

    struct retro_system_av_info av; memset(&av, 0, sizeof av);
    retro_get_system_av_info(&av);
    printf("loaded: base %ux%u max %ux%u fps %.2f\n",
           av.geometry.base_width, av.geometry.base_height,
           av.geometry.max_width, av.geometry.max_height,
           av.timing.fps);

    for (int f = 1; f <= frames; f++) {
        retro_run();
        if (f % 30 == 0)
            printf("frame %d  vid_calls=%u  distinct_frames=%u\n",
                   f, g_vid_calls, g_distinct);
    }

    printf("SUCCESS: ran %d frames, vid_calls=%u, distinct_frames=%u\n",
           frames, g_vid_calls, g_distinct);

    /* RAM high-water */
    FILE *st = fopen("/proc/self/status", "r");
    if (st) {
        char line[256];
        while (fgets(line, sizeof line, st))
            if (!strncmp(line, "VmHWM:", 6)) { fputs(line, stdout); break; }
        fclose(st);
    }

    retro_unload_game();
    retro_deinit();
    return 0;
}
