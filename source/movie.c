/*
 * Full-screen movies (JavaCallback_StartVideo) with the Vita's hardware H.264 decoder (SceAvPlayer).
 *
 * The game's movies (UDKGame/Movies/<name>.m4v) are MPEG-4 Part 2, which the Vita cannot decode in hardware, so they are
 * converted once on a PC (tools/convert_movies.sh) to H.264 960x544 + AAC and placed in ux0:data/darkmeadow/movies/
 * as <name>.mp4. A movie without a converted file is skipped as before.
 *
 * On Android the movie plays in a VideoView above the GL surface. Here it is drawn by the rendering thread at every
 * buffer swap (movie_render, called from JavaCallback_swapBuffers), over whatever the engine drew, before the swap.
 * The decoded frames stay in the decoder's memory and are sampled as a YUV texture (the GPU converts to RGB). All GL
 * state touched for that draw is saved and restored, and the draw uses its own vertex array object, so the engine's
 * cached state stays valid. Audio goes to its own audio port. A tap or a button press skips the movie, as on Android.
 * If no frame is swapped for 3 s after a movie was requested, it is given up (the engine is then not rendering).
 */
#include "movie.h"
#include "reimpl/pkg_cache.h"

#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/avplayer.h>
#include <psp2/ctrl.h>
#include <psp2/gxm.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>
#include <vitaGL.h>

#include "engine.h"
#include "utils/logger.h"

#ifndef DATA_PATH
#define DATA_PATH "ux0:data/darkmeadow/"
#endif

#define FRAMES 5
#define PHYCONT_ALIGN (1024 * 1024)
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

enum { M_IDLE, M_REQUESTED, M_PLAYING, M_STOPPING };
static volatile int state = M_IDLE;
static volatile int stop_req;
static char movie_path[256];
static uint64_t requested_at;
static pthread_mutex_t mlock = PTHREAD_MUTEX_INITIALIZER;

static SceAvPlayerHandle player;
static int sysmodule_loaded;
static SceUID audio_thid = -1;
static volatile int audio_run;
static int test_active;   /* the movie playing was started by the loader (startup / test), not by the engine */
static volatile unsigned audio_frames;   /* r58: audio frames the decoder produced (does it decode at all?) */

/* GPU memory handed to the decoder: freed on the rendering thread once the movie is over */
#define MAX_GPU_BLOCKS 32
static struct { void *addr; SceUID blk; } gpu_blocks[MAX_GPU_BLOCKS];
static int gpu_n;

/* drawing */
static GLuint tex[FRAMES], prog, vao, vbo_unused;
static SceGxmTexture *gxm_tex[FRAMES];
static int frame_idx, have_frame, gl_ready;
static float quad[16];

/* ---- decoder memory callbacks ---- */
static void *alloc_cpu(void *p, uint32_t align, uint32_t size) {
    void *r = memalign(align < 16 ? 16 : align, size);
    static int said;
    if (said++ < 8) l_info("[movie] decoder working memory: %u KB, aligned to %u%s", size / 1024, (unsigned)align, r ? "" : " - FAILED");
    return r;
}
static void free_cpu(void *p, void *ptr) { free(ptr); }

/* vitaGL's pools, with the alignment the decoder asks for (vglAlloc only aligns to 16 bytes). r58: r57's movie got
 * its decoder memory from vglAlloc and never produced a frame: the decoder's buffers must sit at the alignment it
 * passes in. vgl_memalign is vitaGL's internal aligned allocator (utils/mem_utils.c). */
extern void *vgl_memalign(size_t alignment, size_t size, vglMemType type);

static void *alloc_gpu(void *p, uint32_t align, uint32_t size) {
    void *res = NULL;
    SceUID blk = -1;
    const char *from = "";
    uint32_t a = align < 256 ? 256 : align;
    /* r63: as vitaGL's samples/video_playback does it - a physically contiguous RAM block of its own (1 MB aligned),
     * mapped for the GPU. gl_init leaves that memory outside vitaGL (DM_MOVIE_PHYCONT_MB). */
    uint32_t sz = ALIGN(size, PHYCONT_ALIGN);
    blk = sceKernelAllocMemBlock("dm_movie", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, sz, NULL);
    if (blk >= 0) {
        sceKernelGetMemBlockBase(blk, &res);
        sceGxmMapMemory(res, sz, SCE_GXM_MEMORY_ATTRIB_RW);
        from = "physically contiguous block";
    } else {
        static int said;
        if (said++ < 4) l_info("[movie] no physically contiguous block of %u KB: 0x%08x", sz / 1024, (unsigned)blk);
        /* then a video-memory block of its own (only with DM_MOVIE_CDRAM_MB) */
        uint32_t ca = a < 256 * 1024 ? 256 * 1024 : a;
        uint32_t csz = ALIGN(size, ca);
        SceKernelAllocMemBlockOpt opt;
        memset(&opt, 0, sizeof(opt));
        opt.size = sizeof(opt);
        opt.attr = 0x00000004;   /* alignment given */
        opt.alignment = ca;
        blk = sceKernelAllocMemBlock("dm_movie_cdram", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, csz, &opt);
        if (blk >= 0) {
            sceKernelGetMemBlockBase(blk, &res);
            sceGxmMapMemory(res, csz, SCE_GXM_MEMORY_ATTRIB_RW);
            from = "video-memory block";
        }
    }
    if (blk < 0) {   /* last resort: vitaGL's pools (r60-r62: the decoder did not start with these) */
        res = vgl_memalign(a, ALIGN(size, a), VGL_MEM_VRAM);
        from = "vitaGL video memory";
        if (!res) { res = vgl_memalign(a, ALIGN(size, a), VGL_MEM_RAM); from = "vitaGL RAM pool"; }
    }
    pthread_mutex_lock(&mlock);
    if (res && gpu_n < MAX_GPU_BLOCKS) { gpu_blocks[gpu_n].addr = res; gpu_blocks[gpu_n].blk = blk; gpu_n++; }
    pthread_mutex_unlock(&mlock);
    static int said;
    if (said++ < 8)
        l_info("[movie] decoder memory: %u KB, aligned to %u, from %s at %p%s", size / 1024, (unsigned)align, from, res,
               res ? "" : " - FAILED");
    return res;
}

static void free_gpu(void *p, void *addr) { (void)p; (void)addr; /* released in release_gpu_blocks() */ }

static void release_gpu_blocks(void) {
    glFinish();
    pthread_mutex_lock(&mlock);
    for (int i = 0; i < gpu_n; i++) {
        if (gpu_blocks[i].blk >= 0) {
            sceGxmUnmapMemory(gpu_blocks[i].addr);
            sceKernelFreeMemBlock(gpu_blocks[i].blk);
        } else {
            vglFree(gpu_blocks[i].addr);
        }
    }
    gpu_n = 0;
    pthread_mutex_unlock(&mlock);
}

/* r61: the player's own events (state changes, warnings, errors), to see why a movie does not start */
static void av_event(void *p, int32_t id, int32_t source, void *data) {
    (void)p;
    static int n;
    if (n++ < 40)
        l_info("[movie] player event 0x%x (source %d%s%08x)", (unsigned)id, (int)source, data ? ", data " : "",
               data ? (unsigned)*(int32_t *)data : 0u);
}

/* ---- audio ---- */
static int audio_thread(SceSize args, void *argp) {
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1024, 48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) l_warn("[movie] no audio port: 0x%08x", port);
    while (audio_run) {
        SceAvPlayerFrameInfo f;
        if (port >= 0 && player && sceAvPlayerIsActive(player) && sceAvPlayerGetAudioData(player, &f)) {
            sceAudioOutSetConfig(port, 1024, f.details.audio.sampleRate,
                                 f.details.audio.channelCount == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
            sceAudioOutOutput(port, f.pData);
            audio_frames++;
        } else {
            sceKernelDelayThread(1000);
        }
    }
    if (port >= 0) sceAudioOutReleasePort(port);
    return sceKernelExitDeleteThread(0);
}

/* ---- GL resources (rendering thread) ---- */
static const char *vsh =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
static const char *fsh =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "void main() { gl_FragColor = vec4(texture2D(uTex, vUV).rgb, 1.0); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    GLint len = (GLint)strlen(src);
    glShaderSource(s, 1, &src, &len);
    glCompileShader(s);
    return s;
}

static int gl_setup(void) {
    if (gl_ready) return 1;
    GLuint v = compile(GL_VERTEX_SHADER, vsh), f = compile(GL_FRAGMENT_SHADER, fsh);
    prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aUV");
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { l_warn("[movie] video shader did not link"); return 0; }
    GLint cur = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &cur);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
    glUseProgram((GLuint)cur);

    GLint act = 0, bound = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glGenTextures(FRAMES, tex);
    for (int i = 0; i < FRAMES; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        gxm_tex[i] = vglGetGxmTexture(GL_TEXTURE_2D);
        vglFree(vglGetTexDataPointer(GL_TEXTURE_2D));   /* the decoder's frame memory is used instead */
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
    glActiveTexture((GLenum)act);

    /* full screen, two triangles as a strip: x, y, u, v */
    static const float q[16] = { -1, 1, 0, 0,   1, 1, 1, 0,   -1, -1, 0, 1,   1, -1, 1, 1 };
    memcpy(quad, q, sizeof(q));
    glGenVertexArrays(1, &vao);
    (void)vbo_unused;
    gl_ready = 1;
    return 1;
}

/* Draw the latest frame over the screen, leaving every piece of GL state as the engine had it. */
static void draw_frame(void) {
    GLint cur_prog, act, bound, abuf, fb, vp[4], dmask;
    GLboolean cmask[4];
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE),
              scissor = glIsEnabled(GL_SCISSOR_TEST), stencil = glIsEnabled(GL_STENCIL_TEST),
              poff = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    glGetIntegerv(GL_CURRENT_PROGRAM, &cur_prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_DEPTH_WRITEMASK, &dmask);
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);

    if (fb) glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 960, 544);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST); glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(prog);
    glBindTexture(GL_TEXTURE_2D, tex[frame_idx]);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(vao);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, &quad[0]);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, &quad[2]);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)abuf);
    glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
    glActiveTexture((GLenum)act);
    glUseProgram((GLuint)cur_prog);
    glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    glDepthMask((GLboolean)dmask);
    if (blend) glEnable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (stencil) glEnable(GL_STENCIL_TEST);
    if (poff) glEnable(GL_POLYGON_OFFSET_FILL);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (fb) glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
}

/* ---- skip input ---- */
static int skip_pressed(void) {
    static uint32_t old_buttons = 0xffffffff;   /* ignore what is already held when the movie starts */
    static int old_touch = 1;
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t mask = SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_START;
    int pressed = (pad.buttons & mask) && !(old_buttons & pad.buttons & mask);
    old_buttons = pad.buttons;
    SceTouchData t;
    memset(&t, 0, sizeof(t));
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1);
    int touching = t.reportNum > 0;
    if (touching && !old_touch) pressed = 1;
    old_touch = touching;
    return pressed;
}

static void finish(const char *why) {
    /* r59: stop the audio thread before closing the player: r57/r58 closed it first, and the audio thread, still in
     * sceAvPlayerGetAudioData on the closed handle, crashed (data abort at 0x50 in dm_movie_audio) */
    audio_run = 0;
    if (audio_thid >= 0) { sceKernelWaitThreadEnd(audio_thid, NULL, NULL); audio_thid = -1; }
    if (player) {
        sceAvPlayerStop(player);
        sceAvPlayerClose(player);
        player = 0;
    }
    release_gpu_blocks();
    have_frame = 0;
    l_info("[movie] %s", why);
    state = M_IDLE;
    pkg_cache_movie(0);   /* r64: the package prefetch may take its window again */
    if (test_active) test_active = 0;   /* the engine never asked for it */
    else engine_movie_finished();
}

/* ---- API ---- */
int movie_request(const char *name) {
    if (!name || !*name) return 0;
    l_info("[movie] engine asked for \"%s\"", name);
    /* the engine may pass a bare name, a name with its .m4v extension, or a path with / or \ separators */
    const char *base = name;
    for (const char *p = name; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    char stem[128];
    snprintf(stem, sizeof(stem), "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    base = stem;
    char path[256];
    snprintf(path, sizeof(path), DATA_PATH "movies/%s.mp4", base);
    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0) {
        l_info("[movie] %s: no converted file (%s), skipped", base, path);
        return 0;
    }
    if (state != M_IDLE) { l_warn("[movie] %s requested while another movie is playing, skipped", base); return 0; }
    pkg_cache_movie(1);   /* r64: the decoder needs the physically contiguous RAM the package prefetch borrows */
    snprintf(movie_path, sizeof(movie_path), "%s", path);
    stop_req = 0;
    requested_at = sceKernelGetProcessTimeWide();
    state = M_REQUESTED;
    l_info("[movie] %s: playing %s", base, path);
    return 1;
}

int movie_playing(void) { return state != M_IDLE; }
/* r61: what JavaCallback_IsVideoPlaying reports: only movies the engine asked for (not the opening movie) */
int movie_engine_playing(void) { return state != M_IDLE && !test_active; }

/* r62: a movie the loader started (the opening movie) is not the engine's to stop: r61's opening movie was stopped
 * by the engine's StopVideo a moment after it began */
void movie_stop(void) { if (state != M_IDLE && !test_active) stop_req = 1; }

/* No swap for a while after a request: the engine is not rendering, so the movie cannot be shown. */
void movie_check_stalled(void) {
    int gave_up = 0;
    pthread_mutex_lock(&mlock);
    if (state == M_REQUESTED && sceKernelGetProcessTimeWide() - requested_at > 3000000ull) { state = M_IDLE; gave_up = 1; }
    pthread_mutex_unlock(&mlock);
    if (gave_up) {
        l_warn("[movie] no frame was presented within 3 s of the request: movie given up");
        if (test_active) test_active = 0; else engine_movie_finished();
    }
}

/* r61: movies the loader plays by itself, without the engine knowing (it is not told when they end):
 *   the opening movie: Startup, as soon as the engine draws its first frame (on Android the Java side plays it while
 *   the engine starts; the engine never asks for it). DM_STARTUP_MOVIE <name> | off in config.txt.
 *   DM_TEST_MOVIE <name>: that movie about 10 s after the game starts drawing, for testing the player. */
static char test_movie[64], startup_movie[64] = "Startup";
static int test_checked, hold_startup = 1;
static void movie_step(void);
extern void gl_swap(void);   /* utils/glutil.c */

static void clear_black(void) {
    GLint fb = 0, vp[4];
    GLfloat cc[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    if (fb) glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, 960, 544);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (sc) glEnable(GL_SCISSOR_TEST);
    if (fb) glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
}
extern volatile unsigned engine_frames;
extern volatile uint64_t ui_menu_us;   /* reimpl/ue3_trace.c: a menu screen was drawn */

static void read_movie_config(void) {
    if (!test_checked) {
        test_checked = 1;
        FILE *f = fopen(DATA_PATH "config.txt", "r");
        if (f) {
            char line[128], k[64], v[64];
            while (fgets(line, sizeof(line), f))
                if (sscanf(line, "%63s %63s", k, v) == 2) {
                    if (!strcmp(k, "DM_TEST_MOVIE")) snprintf(test_movie, sizeof(test_movie), "%s", v);
                    else if (!strcmp(k, "DM_STARTUP_MOVIE")) snprintf(startup_movie, sizeof(startup_movie), "%s", strcmp(v, "off") ? v : "");
                    else if (!strcmp(k, "DM_STARTUP_MOVIE_HOLD")) hold_startup = atoi(v) != 0;
                }
            fclose(f);
        }
    }
}

/* r67: the opening movie is still to come (pkg_cache.c keeps its memory free until then) */
int movie_opening_pending(void) {
    read_movie_config();
    return startup_movie[0] != 0;
}

void movie_render(void) {
    read_movie_config();
    /* r64: the opening movie plays when the title screen is first drawn (r63: at the first frame - it ended long
     * before the engine had loaded, and skipping it only meant a longer wait for the title) */
    if (startup_movie[0] && ui_menu_us && state == M_IDLE) {
        l_info("[movie] opening movie \"%s\" before the title screen (played by the loader; DM_STARTUP_MOVIE)", startup_movie);
        if (movie_request(startup_movie)) test_active = 1;
        startup_movie[0] = 0;
    }
    if (test_movie[0] && engine_frames > 600 && state == M_IDLE) {
        l_info("[movie] DM_TEST_MOVIE: playing \"%s\" now", test_movie);
        if (movie_request(test_movie)) test_active = 1;
        test_movie[0] = 0;
    }
    if (state == M_IDLE) return;
    movie_step();
    /* r63: the loader's movie holds this swap (r64: at the title screen, which waits behind it), and the movie runs on the rendering thread with its
     * own buffer swaps until it ends or is skipped. The engine's game thread goes on loading meanwhile; if it needs
     * the rendering thread it waits, at most until the movie is over. DM_STARTUP_MOVIE_HOLD 0: draw only at the
     * engine's own swaps. */
    if (test_active && hold_startup) {
        static int said;
        if (!said++) l_info("[movie] the opening movie holds the rendering thread while the engine loads");
        while (state != M_IDLE) {
            if (!have_frame) clear_black();
            gl_swap();
            engine_frames++;
            movie_step();
        }
    }
}

/* one frame of movie: start the player, or take the next decoded frame and draw it */
static void movie_step(void) {
    if (state == M_REQUESTED) {
        pthread_mutex_lock(&mlock);
        int go = state == M_REQUESTED;
        if (go) state = M_STOPPING;   /* "starting": the watchdog no longer gives it up */
        pthread_mutex_unlock(&mlock);
        if (!go) return;
        if (!sysmodule_loaded) { sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER); sysmodule_loaded = 1; }
        if (!gl_setup()) { state = M_IDLE; pkg_cache_movie(0); if (test_active) test_active = 0; else engine_movie_finished(); return; }
        SceAvPlayerInitData init;
        memset(&init, 0, sizeof(init));
        init.memoryReplacement.allocate = alloc_cpu;
        init.memoryReplacement.deallocate = free_cpu;
        init.memoryReplacement.allocateTexture = alloc_gpu;
        init.memoryReplacement.deallocateTexture = free_gpu;
        init.eventReplacement.eventCallback = av_event;
        init.debugLevel = 0;
        init.basePriority = 0xA0;
        init.numOutputVideoFrameBuffers = FRAMES;
        init.autoStart = 1;
        player = sceAvPlayerInit(&init);
        if (!player) { l_warn("[movie] sceAvPlayerInit failed"); state = M_IDLE; pkg_cache_movie(0); if (test_active) test_active = 0; else engine_movie_finished(); return; }
        audio_run = 1;
        audio_thid = sceKernelCreateThread("dm_movie_audio", audio_thread, 0x10000100 - 10, 0x4000, 0, 0, NULL);
        if (audio_thid >= 0) sceKernelStartThread(audio_thid, 0, NULL);
        int as = sceAvPlayerAddSource(player, movie_path);
        if (as < 0) { l_warn("[movie] sceAvPlayerAddSource: 0x%08x", (unsigned)as); finish("could not open the movie file"); return; }
        audio_frames = 0;
        frame_idx = 0;
        have_frame = 0;
        skip_pressed();   /* prime the button/touch state */
        requested_at = sceKernelGetProcessTimeWide();   /* r63: the 5 s wait for a first frame starts now */
        state = M_PLAYING;
        return;
    }
    if (state != M_PLAYING) return;
    if (stop_req) { finish("stopped by the game"); return; }
    if (skip_pressed() && have_frame && sceKernelGetProcessTimeWide() - requested_at > 1000000ull) { finish("skipped"); return; }   /* r70: not in its first second (r68: a button still held from the title skipped it at once) */
    if (sceAvPlayerIsActive(player)) {
        SceAvPlayerFrameInfo f;
        if (sceAvPlayerGetVideoData(player, &f)) {
            frame_idx = (frame_idx + 1) % FRAMES;
            sceGxmTextureInitLinear(gxm_tex[frame_idx], f.pData, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1,
                                    f.details.video.width, f.details.video.height, 0);
            sceGxmTextureSetMinFilter(gxm_tex[frame_idx], SCE_GXM_TEXTURE_FILTER_LINEAR);
            sceGxmTextureSetMagFilter(gxm_tex[frame_idx], SCE_GXM_TEXTURE_FILTER_LINEAR);
            if (!have_frame) l_info("[movie] first frame %ux%u", f.details.video.width, f.details.video.height);
            have_frame = 1;
        }
    } else if (have_frame || sceKernelGetProcessTimeWide() - requested_at > 5000000ull) {
        if (!have_frame)
            l_warn("[movie] decoder %s, %u audio frames, no video frame after %u ms",
                   sceAvPlayerIsActive(player) ? "active" : "not active", audio_frames,
                   (unsigned)((sceKernelGetProcessTimeWide() - requested_at) / 1000));
        finish(have_frame ? "finished" : "no frame decoded within 5 s - is it H.264?");
        return;
    }
    if (have_frame) {
        draw_frame();
        static int checked;
        if (!checked) {   /* r70: did the first frame draw? (r69: the opening movie played its full length, unseen) */
            checked = 1;
            GLenum err = glGetError();
            GLint fbo = 0, vp[4] = {0};
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
            glGetIntegerv(GL_VIEWPORT, vp);
            l_info("[movie] first frame drawn: GL error 0x%x, framebuffer %d, viewport %d,%d %dx%d, texture %u", (unsigned)err,
                   (int)fbo, vp[0], vp[1], vp[2], vp[3], (unsigned)tex[frame_idx]);
        }
    }
}
