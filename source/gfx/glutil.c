/*
 * vitaGL start-up and the OpenGL ES calls the loader handles before vitaGL sees them.
 * Based on the so-loader boilerplate (Andy Nguyen, Rinnegatamante, Volodymyr Atamanenko; MIT).
 */
#include "gfx/glutil.h"

#include <pthread.h>

#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>

#include "config.h"
#include "gfx/gl_fixes.h"
#include "storage/pkg_cache.h"
#include "storage/tex_cache.h"
#include "util/fatal.h"
#include "util/logger.h"
#include "util/utils.h"

/* vitaGL's GLSL preprocessor reports failures here (lib/vitagl/source/utils/preprocessor/preprocessor.cpp) */
extern char vgl_preproc_error[512];
extern volatile int vgl_preproc_errors;

volatile uint32_t dm_link_n, dm_link_us, dm_link_max_us;

/* vitaGL's size for the memory the system message dialogs need (utils/mem_utils.h, not a public header) */
#ifndef SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE
#define SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE 0x8C6000
#endif

/* Physically contiguous memory vitaGL leaves alone: the movie player (sceAvPlayer) needs its frame memory there, in
 * blocks of its own, as in vitaGL's samples/video_playback. Between movies the package prefetch borrows it. */
#define MOVIE_PHYCONT_MB 32

void gl_preload(void) {
    if (!file_exists("ur0:/data/libshacccg.suprx") && !file_exists("ur0:/data/external/libshacccg.suprx"))
        fatal_error("Error: libshacccg.suprx is not installed. Google \"ShaRKBR33D\" for quick installation.");
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
}

static void gl_init_once(void) {
    /* compiled shaders are kept here, so each one is only compiled once (must be set before vglInit*) */
    vglSetShaderCachePath(DATA_PATH "shader_cache");

    int msaa = config_int("DM_MSAA", 4);
    int fps = config_int("DM_FPS_LIMIT", 60);
    SceGxmMultisampleMode ms = msaa >= 4 ? SCE_GXM_MULTISAMPLE_4X : msaa >= 2 ? SCE_GXM_MULTISAMPLE_2X
                                                                              : SCE_GXM_MULTISAMPLE_NONE;
    vglInitWithCustomThreshold(0, 960, 544, 6 * 1024 * 1024, 0, MOVIE_PHYCONT_MB << 20,
                               SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE, ms);

    /* blocks taken from vitaGL's pools before the rendering thread exists (vitaGL's allocator is not shared) */
    tex_cache_attach_vram();
    pkg_cache_attach_spill();

    if (fps > 0 && fps <= 30) eglSwapInterval(0, 2);
    l_info("[gl] screen: MSAA %s, frame rate limit %d fps",
           ms == SCE_GXM_MULTISAMPLE_4X ? "4x" : ms == SCE_GXM_MULTISAMPLE_2X ? "2x" : "off", fps > 0 && fps <= 30 ? 30 : 60);
}

/* Both JavaCallback_initEGL and eglInitialize may ask; vitaGL starts once. */
void gl_init(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, gl_init_once);
}

void gl_swap(void) {
    vglSwapBuffers(GL_FALSE);
}

/* ---- the extension string ----
 * The engine picks its content folder from the compressed texture formats in GL_EXTENSIONS, preferring DXT, then
 * PVRTC, ATITC and ETC (appAndroidGetCookedDirectoryName). vitaGL lists several, but this edition of the game only has
 * data in CookedAndroid_PVRTC, so every other compressed format is hidden. */
static const char *const hidden_extensions[] = {
    "GL_EXT_texture_compression_s3tc",
    "GL_EXT_texture_compression_dxt1",
    "GL_EXT_texture_compression_dxt3",
    "GL_EXT_texture_compression_dxt5",
    "GL_AMD_compressed_ATC_texture",
    "GL_ATI_texture_compression_atitc",
    "GL_OES_compressed_ETC1_RGB8_texture",
};
#define PVRTC_EXTENSION "GL_IMG_texture_compression_pvrtc"

static int is_hidden(const char *name, size_t len) {
    for (size_t i = 0; i < sizeof(hidden_extensions) / sizeof(hidden_extensions[0]); i++)
        if (strlen(hidden_extensions[i]) == len && !strncmp(name, hidden_extensions[i], len)) return 1;
    return 0;
}

const GLubyte *glGetString_soloader(GLenum name) {
    const GLubyte *orig = glGetString(name);
    if (name != GL_EXTENSIONS || !orig) return orig;

    static char *filtered;
    if (filtered) return (const GLubyte *)filtered;

    const char *src = (const char *)orig;
    char *out = malloc(strlen(src) + sizeof(PVRTC_EXTENSION) + 2);
    if (!out) return orig;
    size_t n = 0;
    int has_pvrtc = 0;
    for (const char *p = src; *p;) {
        while (*p == ' ') p++;
        const char *e = p + strcspn(p, " ");
        size_t len = (size_t)(e - p);
        if (len && !is_hidden(p, len)) {
            if (len == strlen(PVRTC_EXTENSION) && !strncmp(p, PVRTC_EXTENSION, len)) has_pvrtc = 1;
            memcpy(out + n, p, len);
            n += len;
            out[n++] = ' ';
        }
        p = e;
    }
    if (!has_pvrtc) {
        memcpy(out + n, PVRTC_EXTENSION " ", sizeof(PVRTC_EXTENSION));
        n += sizeof(PVRTC_EXTENSION);
    }
    out[n] = 0;
    filtered = out;
    l_info("[gl] GL_EXTENSIONS filtered to report PVRTC as the only compressed texture format");
    return (const GLubyte *)filtered;
}

/* ---- shaders ---- */

/* pow(0, y) is NaN on the Vita's shader compiler (exp2(y * log2(x))), which the game's specular and fresnel terms hit:
 * those surfaces came out black. Every shader gets this line first. */
static const char pow_fix[] = "#define pow(a, b) pow(max((a), 0.0000001), (b))\n";

static int skip_next_compile;

void glShaderSource_soloader(GLuint shader, GLsizei count, const GLchar **string, const GLint *length) {
    if (!string || !*string) {
        l_error("[gl] shader %u: no source (count %d)", (unsigned)shader, (int)count);
        skip_next_compile = 1;
        return;
    }
    size_t total = sizeof(pow_fix) - 1;
    for (GLsizei i = 0; i < count; i++) total += length ? (size_t)length[i] : strlen(string[i]);

    char *src = malloc(total + 1);
    if (!src) return;
    size_t at = sizeof(pow_fix) - 1;
    memcpy(src, pow_fix, at);
    for (GLsizei i = 0; i < count; i++) {
        size_t l = length ? (size_t)length[i] : strlen(string[i]);
        memcpy(src + at, string[i], l);
        at += l;
    }
    src[total] = 0;

    src = gl_fixes_shader_source(shader, src, &total);
    gl_load_shader(shader, src, total);
    free(src);
}

void gl_load_shader(GLuint shader, const char *src, size_t len) {
    GLint l = (GLint)len;
    glShaderSource(shader, 1, &src, &l);
}

void glCompileShader_soloader(GLuint shader) {
    if (skip_next_compile) {
        skip_next_compile = 0;
        return;
    }
    int errors = vgl_preproc_errors;
    glCompileShader(shader);
    if (vgl_preproc_errors != errors) {
        static int n;
        if (n++ < 20)
            l_error("[gl] shader %u: vitaGL's GLSL preprocessor failed (%s); a blank shader was used instead", (unsigned)shader,
                    vgl_preproc_error);
    }
}

void glAttachShader_soloader(GLuint prog, GLuint shader) {
    gl_fixes_attach(prog, shader);
    glAttachShader(prog, shader);
}

/* The engine links every program twice (again after binding its attributes); vitaGL ignores the second link and only
 * logs a warning, so it is skipped here. */
void glLinkProgram_soloader(GLuint prog) {
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (linked) return;

    int errors = vgl_preproc_errors;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    glLinkProgram(prog);
    uint32_t dt = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    __sync_fetch_and_add(&dm_link_n, 1);
    __sync_fetch_and_add(&dm_link_us, dt);
    if (dt > dm_link_max_us) dm_link_max_us = dt;
    if (vgl_preproc_errors != errors) {
        static int n;
        if (n++ < 20)
            l_error("[gl] program %u: vitaGL's GLSL preprocessor failed (%s); a blank shader was used instead", (unsigned)prog,
                    vgl_preproc_error);
    }
}

/* vitaGL calls this vitaShaRK function from vglSetShaderAssociationPath(), which is never used here; older
 * libvitashark.a builds lack it and the link fails. A weak no-op satisfies the linker. */
__attribute__((weak)) void shark_set_shader_association_path(const char *path) {
    (void)path;
}
