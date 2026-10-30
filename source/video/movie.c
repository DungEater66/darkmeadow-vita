/*
 * Full-screen movies (JavaCallback_StartVideo) with the Vita's hardware H.264 decoder (SceAvPlayer).
 *
 * The game's movies (UDKGame/Movies/<name>.m4v) are MPEG-4 Part 2, which the Vita cannot decode in hardware, so they
 * are converted once on a PC (tools/convert_movies.sh) to H.264 960x544 + AAC and placed in ux0:data/darkmeadow/movies/
 * as <name>.mp4. A movie without a converted file is skipped.
 *
 * On Android the movie plays in a VideoView above the GL surface. Here the rendering thread draws it at every buffer
 * swap (movie_render, from JavaCallback_swapBuffers), over whatever the engine drew. Decoded frames stay in the
 * decoder's memory and are sampled as a YUV texture (the GPU converts to RGB). Every piece of GL state the draw
 * touches is saved and restored, and it uses its own vertex array object, so the engine's cached state stays valid.
 * Audio goes to its own audio port. A tap or Cross / Circle / Start skips the movie, as on Android.
 *
 * The opening movie (Startup) is played by the loader itself when the title screen is first drawn (on Android the
 * Java side plays it while the engine starts; the engine never asks for it). It holds the rendering thread, swapping
 * its own frames, while the engine goes on loading behind it. DM_STARTUP_MOVIE <name> | off in config.txt.
 *
 * Memory: the decoder's frame buffers come from a physically contiguous block of their own, which gl_init keeps out
 * of vitaGL (MOVIE_PHYCONT_MB). The package prefetch borrows that memory while no movie plays (pkg_cache_movie).
 */
#include "video/movie.h"

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

#include "android/engine.h"
#include "config.h"
#include "gfx/glutil.h"
#include "hooks/game_state.h"
#include "storage/pkg_cache.h"
#include "util/logger.h"

#define FRAMES 5                          /* decoder output buffers */
#define PHYCONT_ALIGN (1024 * 1024)
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define START_TIMEOUT_US 3000000ull       /* requested, but no buffer swap to show it on */
#define FIRST_FRAME_TIMEOUT_US 5000000ull /* playing, but nothing decoded */
#define NO_SKIP_US 1000000ull             /* a button still held from before does not skip the first second */

enum { M_IDLE, M_REQUESTED, M_STARTING, M_PLAYING };
static volatile int state = M_IDLE;
static volatile int stop_requested;
static int loader_owned;   /* the movie was started by the loader (the opening movie), not by the engine */
static char movie_path[256];
static uint64_t requested_at;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static char opening_movie[64];
static int opening_checked;

static SceAvPlayerHandle player;
static SceUID audio_thid = -1;
static volatile int audio_run;
static volatile unsigned audio_frames;

/* ---- decoder memory ---- */
#define MAX_GPU_BLOCKS 32
static struct { void *addr; SceUID blk; } gpu_blocks[MAX_GPU_BLOCKS];   /* blk < 0: from vitaGL's pools */
static int gpu_n;

extern void *vgl_memalign(size_t alignment, size_t size, vglMemType type);   /* vitaGL's aligned allocator */

static void *alloc_cpu(void *p, uint32_t align, uint32_t size) {
    (void)p;
    return memalign(align < 16 ? 16 : align, size);
}

static void free_cpu(void *p, void *ptr) {
    (void)p;
    free(ptr);
}

/* Frame buffers: a physically contiguous block mapped for the GPU, as vitaGL's video_playback sample does; vitaGL's
 * own pools only if that fails. The buffers must sit at the alignment the decoder asks for. */
static void *alloc_gpu(void *p, uint32_t align, uint32_t size) {
    (void)p;
    void *res = NULL;
    const char *from = "physically contiguous block";
    uint32_t sz = ALIGN(size, PHYCONT_ALIGN);
    SceUID blk = sceKernelAllocMemBlock("dm_movie", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, sz, NULL);
    if (blk >= 0) {
        sceKernelGetMemBlockBase(blk, &res);
        sceGxmMapMemory(res, sz, SCE_GXM_MEMORY_ATTRIB_RW);
    } else {
        uint32_t a = align < 256 ? 256 : align;
        res = vgl_memalign(a, ALIGN(size, a), VGL_MEM_VRAM);
        from = "vitaGL video memory";
        if (!res) {
            res = vgl_memalign(a, ALIGN(size, a), VGL_MEM_RAM);
            from = "vitaGL RAM";
        }
    }
    pthread_mutex_lock(&lock);
    if (res && gpu_n < MAX_GPU_BLOCKS) {
        gpu_blocks[gpu_n].addr = res;
        gpu_blocks[gpu_n].blk = blk;
        gpu_n++;
    }
    pthread_mutex_unlock(&lock);
    static int logged;
    if (logged++ < 8)
        l_info("[movie] decoder memory: %u KB from %s%s", (unsigned)(size / 1024), from, res ? "" : " - FAILED");
    return res;
}

static void free_gpu(void *p, void *addr) {
    (void)p;
    (void)addr;   /* all released together once the movie is over (release_gpu_blocks) */
}

static void release_gpu_blocks(void) {
    glFinish();
    pthread_mutex_lock(&lock);
    for (int i = 0; i < gpu_n; i++) {
        if (gpu_blocks[i].blk >= 0) {
            sceGxmUnmapMemory(gpu_blocks[i].addr);
            sceKernelFreeMemBlock(gpu_blocks[i].blk);
        } else {
            vglFree(gpu_blocks[i].addr);
        }
    }
    gpu_n = 0;
    pthread_mutex_unlock(&lock);
}

/* the player's own events (state changes, warnings, errors): why a movie does not start */
static void player_event(void *p, int32_t id, int32_t source, void *data) {
    (void)p;
    static int logged;
    if (logged++ < 40)
        l_info("[movie] player event 0x%x (source %d%s%08x)", (unsigned)id, (int)source, data ? ", data " : "",
               data ? (unsigned)*(int32_t *)data : 0u);
}

/* ---- audio ---- */
static int audio_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1024, 48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) l_warn("[movie] no audio port: 0x%08x", (unsigned)port);
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

/* ---- drawing (rendering thread) ---- */
static GLuint textures[FRAMES], program, vao;
static SceGxmTexture *gxm_textures[FRAMES];
static int frame_idx, have_frame, gl_ready;

/* full screen, two triangles as a strip: x, y, u, v */
static const float quad[16] = { -1, 1, 0, 0, 1, 1, 1, 0, -1, -1, 0, 1, 1, -1, 1, 1 };

static const char *const vertex_shader =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
static const char *const fragment_shader =
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
    program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, vertex_shader));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment_shader));
    glBindAttribLocation(program, 0, "aPos");
    glBindAttribLocation(program, 1, "aUV");
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        l_warn("[movie] video shader did not link");
        return 0;
    }
    GLint cur = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &cur);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "uTex"), 0);
    glUseProgram((GLuint)cur);

    /* textures whose data pointer is set to each decoded frame */
    GLint active = 0, bound = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glGenTextures(FRAMES, textures);
    for (int i = 0; i < FRAMES; i++) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        gxm_textures[i] = vglGetGxmTexture(GL_TEXTURE_2D);
        vglFree(vglGetTexDataPointer(GL_TEXTURE_2D));
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
    glActiveTexture((GLenum)active);

    glGenVertexArrays(1, &vao);
    gl_ready = 1;
    return 1;
}

/* Draw the latest frame over the screen, leaving every piece of GL state as the engine had it. */
static void draw_frame(void) {
    GLint cur_prog, active, bound, array_buf, fb, vp[4], depth_mask;
    GLboolean color_mask[4];
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE),
              scissor = glIsEnabled(GL_SCISSOR_TEST), stencil = glIsEnabled(GL_STENCIL_TEST),
              offset = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    glGetIntegerv(GL_CURRENT_PROGRAM, &cur_prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buf);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_DEPTH_WRITEMASK, &depth_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);

    if (fb) glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 960, 544);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(program);
    glBindTexture(GL_TEXTURE_2D, textures[frame_idx]);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(vao);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, &quad[0]);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, &quad[2]);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)array_buf);
    glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
    glActiveTexture((GLenum)active);
    glUseProgram((GLuint)cur_prog);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    glDepthMask((GLboolean)depth_mask);
    if (blend) glEnable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (stencil) glEnable(GL_STENCIL_TEST);
    if (offset) glEnable(GL_POLYGON_OFFSET_FILL);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (fb) glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
}

/* Black, while the opening movie waits for its first frame. */
static void clear_black(void) {
    GLint fb = 0, vp[4];
    GLfloat clear_color[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (fb) glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, 960, 544);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (fb) glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
}

/* ---- skipping ---- */
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

/* ---- start and end ---- */
static void movie_done(void) {
    state = M_IDLE;
    pkg_cache_movie(0);   /* the package prefetch may take its memory back */
    if (loader_owned) loader_owned = 0;   /* the engine never asked for it */
    else engine_movie_finished();
}

static void finish(const char *why) {
    /* the audio thread first: it must not be inside sceAvPlayerGetAudioData when the player closes */
    audio_run = 0;
    if (audio_thid >= 0) {
        sceKernelWaitThreadEnd(audio_thid, NULL, NULL);
        audio_thid = -1;
    }
    if (player) {
        sceAvPlayerStop(player);
        sceAvPlayerClose(player);
        player = 0;
    }
    release_gpu_blocks();
    have_frame = 0;
    l_info("[movie] %s", why);
    movie_done();
}

static int start_player(void) {
    static int module_loaded;
    if (!module_loaded) {
        sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
        module_loaded = 1;
    }
    if (!gl_setup()) return 0;
    SceAvPlayerInitData init;
    memset(&init, 0, sizeof(init));
    init.memoryReplacement.allocate = alloc_cpu;
    init.memoryReplacement.deallocate = free_cpu;
    init.memoryReplacement.allocateTexture = alloc_gpu;
    init.memoryReplacement.deallocateTexture = free_gpu;
    init.eventReplacement.eventCallback = player_event;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = FRAMES;
    init.autoStart = 1;
    player = sceAvPlayerInit(&init);
    if (!player) {
        l_warn("[movie] sceAvPlayerInit failed");
        return 0;
    }
    audio_run = 1;
    audio_frames = 0;
    audio_thid = sceKernelCreateThread("dm_movie_audio", audio_thread, 0x10000100 - 10, 0x4000, 0, 0, NULL);
    if (audio_thid >= 0) sceKernelStartThread(audio_thid, 0, NULL);
    int r = sceAvPlayerAddSource(player, movie_path);
    if (r < 0) {
        l_warn("[movie] sceAvPlayerAddSource: 0x%08x", (unsigned)r);
        finish("could not open the movie file");
        return -1;
    }
    frame_idx = 0;
    have_frame = 0;
    skip_pressed();   /* take in what is held now */
    requested_at = sceKernelGetProcessTimeWide();
    return 1;
}

/* One frame of movie: start the player, or take the next decoded frame and draw it. */
static void movie_step(void) {
    if (state == M_REQUESTED) {
        pthread_mutex_lock(&lock);
        int go = state == M_REQUESTED;
        if (go) state = M_STARTING;   /* the watchdog no longer gives it up */
        pthread_mutex_unlock(&lock);
        if (!go) return;
        int r = start_player();
        if (r == 0) movie_done();
        if (r == 1) state = M_PLAYING;
        return;
    }
    if (state != M_PLAYING) return;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (stop_requested) {
        finish("stopped by the game");
        return;
    }
    if (skip_pressed() && have_frame && now - requested_at > NO_SKIP_US) {
        finish("skipped");
        return;
    }
    if (sceAvPlayerIsActive(player)) {
        SceAvPlayerFrameInfo f;
        if (sceAvPlayerGetVideoData(player, &f)) {
            frame_idx = (frame_idx + 1) % FRAMES;
            sceGxmTextureInitLinear(gxm_textures[frame_idx], f.pData, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1,
                                    f.details.video.width, f.details.video.height, 0);
            sceGxmTextureSetMinFilter(gxm_textures[frame_idx], SCE_GXM_TEXTURE_FILTER_LINEAR);
            sceGxmTextureSetMagFilter(gxm_textures[frame_idx], SCE_GXM_TEXTURE_FILTER_LINEAR);
            if (!have_frame)
                l_info("[movie] first frame %ux%u", (unsigned)f.details.video.width, (unsigned)f.details.video.height);
            have_frame = 1;
        }
    } else if (have_frame || now - requested_at > FIRST_FRAME_TIMEOUT_US) {
        if (!have_frame)
            l_warn("[movie] decoder not active, %u audio frames, no video frame after %u ms", audio_frames,
                   (unsigned)((now - requested_at) / 1000));
        finish(have_frame ? "finished" : "no frame decoded within 5 s - is it H.264?");
        return;
    }
    if (have_frame) draw_frame();
}

/* ---- API ---- */
int movie_request(const char *name) {
    if (!name || !*name) return 0;
    l_info("[movie] engine asked for \"%s\"", name);
    /* a bare name, a name with its .m4v extension, or a path with / or \ separators */
    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    char stem[128];
    snprintf(stem, sizeof(stem), "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    char path[256];
    snprintf(path, sizeof(path), DATA_PATH "movies/%s.mp4", stem);
    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0) {
        l_info("[movie] %s: no converted file (%s), skipped", stem, path);
        return 0;
    }
    if (state != M_IDLE) {
        l_warn("[movie] %s requested while another movie is playing, skipped", stem);
        return 0;
    }
    pkg_cache_movie(1);   /* the decoder needs the memory the package prefetch borrows */
    snprintf(movie_path, sizeof(movie_path), "%s", path);
    stop_requested = 0;
    requested_at = sceKernelGetProcessTimeWide();
    state = M_REQUESTED;
    l_info("[movie] %s: playing %s", stem, path);
    return 1;
}

int movie_playing(void) { return state != M_IDLE; }
int movie_engine_playing(void) { return state != M_IDLE && !loader_owned; }

/* the opening movie is not the engine's to stop */
void movie_stop(void) {
    if (state != M_IDLE && !loader_owned) stop_requested = 1;
}

void movie_check_stalled(void) {
    int gave_up = 0;
    pthread_mutex_lock(&lock);
    if (state == M_REQUESTED && sceKernelGetProcessTimeWide() - requested_at > START_TIMEOUT_US) {
        state = M_IDLE;
        gave_up = 1;
    }
    pthread_mutex_unlock(&lock);
    if (gave_up) {
        l_warn("[movie] no frame was presented within 3 s of the request: movie given up");
        movie_done();
    }
}

static void read_opening_movie(void) {
    if (opening_checked) return;
    opening_checked = 1;
    const char *v = config_str("DM_STARTUP_MOVIE", "Startup");
    snprintf(opening_movie, sizeof(opening_movie), "%s", strcmp(v, "off") ? v : "");
}

int movie_opening_pending(void) {
    read_opening_movie();
    return opening_movie[0] != 0;
}

void movie_render(void) {
    read_opening_movie();
    if (opening_movie[0] && game_state.menu_drawn_us && state == M_IDLE) {   /* the title screen is up */
        l_info("[movie] opening movie \"%s\" before the title screen", opening_movie);
        if (movie_request(opening_movie)) loader_owned = 1;
        opening_movie[0] = 0;
    }
    if (state == M_IDLE) return;
    movie_step();

    /* The opening movie keeps this swap until it ends, swapping its own frames. The engine's game thread goes on
     * loading meanwhile; if it needs the rendering thread it waits until the movie is over. */
    if (loader_owned) {
        while (state != M_IDLE) {
            if (!have_frame) clear_black();
            gl_swap();
            engine_frames++;
            movie_step();
        }
    }
}
