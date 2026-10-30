/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"
#include "reimpl/tex_cache.h"
#include "reimpl/pkg_cache.h"
#include "utils/gldiag.h"
#include "dm_config.h"

#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <stdint.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

extern char vgl_preproc_error[512];   /* lib/vitagl/source/utils/preprocessor/preprocessor.cpp */
extern volatile int vgl_preproc_errors;
/* render-thread work, reported with the [gpu] line (utils/watchdog.c) */
volatile uint32_t dm_link_n, dm_link_us, dm_link_max_us;

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);

void gl_preload() {
    if (!file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }

#ifdef USE_GLSL_SHADERS
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
#endif
}

void gl_init() {
    // UE3 renders its own post-processing; multisampling would only cost fill rate and memory on the Vita.
    // vitaGL's built-in shader cache (HAVE_SHADER_CACHE): compiled shaders are kept in DATA_PATH "shader_cache" so
    // they are only compiled once. It creates its own subfolders. Must be set before vglInit*.
    vglSetShaderCachePath(DATA_PATH "shader_cache");
    /* DM_MSAA 0 / 2 / 4 in config.txt: multisampled screen (r63: default 4). DM_FPS_LIMIT 30: present every second
     * refresh. */
    int msaa = 4, fps = 60, movie_cdram = 0, movie_phycont = 32;
    FILE *f = fopen(DATA_PATH "config.txt", "r");
    if (f) {
        char line[128], k[64];
        int v;
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "%63s %d", k, &v) == 2) {
                if (!strcmp(k, "DM_MSAA")) msaa = v;
                else if (!strcmp(k, "DM_FPS_LIMIT")) fps = v;
                else if (!strcmp(k, "DM_MOVIE_CDRAM_MB")) movie_cdram = v;
                else if (!strcmp(k, "DM_MOVIE_PHYCONT_MB")) movie_phycont = v;
            }
        fclose(f);
    }
    SceGxmMultisampleMode ms = msaa >= 4 ? SCE_GXM_MULTISAMPLE_4X : msaa >= 2 ? SCE_GXM_MULTISAMPLE_2X : SCE_GXM_MULTISAMPLE_NONE;
#ifndef SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE
#define SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE 0x8C6000   /* vitaGL's utils/mem_utils.h (not a public header) */
#endif
    /* r63: the movie decoder (sceAvPlayer) needs its frame memory in physically contiguous RAM blocks of its own, as in
     * vitaGL's samples/video_playback: vitaGL is told to leave that memory alone (DM_MOVIE_PHYCONT_MB, default 32 =
     * all of it). vglInitExtended took everything, and the decoder never started (r60-r62). DM_MOVIE_CDRAM_MB still
     * leaves video memory aside too (default 0). */
    vglInitWithCustomThreshold(0, 960, 544, 6 * 1024 * 1024, movie_cdram << 20, movie_phycont << 20,
                               SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE, ms);
    l_info("[gl] physically contiguous memory left for movies: up to %d MB (DM_MOVIE_PHYCONT_MB); video memory: %d MB "
           "(DM_MOVIE_CDRAM_MB)", movie_phycont, movie_cdram);
    tex_cache_attach_vram();   /* texture-data cache block in video memory, before the rendering thread exists */
    pkg_cache_attach_spill();  /* r71: spill block for prefetched packages during movies, likewise */
    if (fps > 0 && fps <= 30) eglSwapInterval(0, 2);
    l_info("[gl] screen: MSAA %s, frame rate limit %d fps (DM_MSAA, DM_FPS_LIMIT)",
           ms == SCE_GXM_MULTISAMPLE_4X ? "4x" : ms == SCE_GXM_MULTISAMPLE_2X ? "2x" : "off", fps > 0 && fps <= 30 ? 30 : 60);
}

/*
 * The engine picks its content folder from the compressed texture formats the driver advertises: it looks for
 * " GL_EXT_texture_compression_s3tc ", " GL_IMG_texture_compression_pvrtc ", " GL_AMD_compressed_ATC_texture " /
 * " GL_ATI_texture_compression_atitc " and " GL_OES_compressed_ETC1_RGB8_texture " in GL_EXTENSIONS, and uses the
 * first one found in the order DXT, PVRTC, ATITC, ETC (appAndroidGetCookedDirectoryName). vitaGL advertises several of
 * them, so the engine chose "CookedAndroid_DXT", which is empty in the PowerVR build's expansion file. Hide all but
 * PVRTC so it looks in "CookedAndroid_PVRTC".
 */
static const char *const hidden_extensions[] = {
    "GL_EXT_texture_compression_s3tc",
    "GL_EXT_texture_compression_dxt1",
    "GL_EXT_texture_compression_dxt3",
    "GL_EXT_texture_compression_dxt5",
    "GL_AMD_compressed_ATC_texture",
    "GL_ATI_texture_compression_atitc",
    "GL_OES_compressed_ETC1_RGB8_texture",
};

const GLubyte *glGetString_soloader(GLenum name) {
    const GLubyte *orig = glGetString(name);
    if (name != GL_EXTENSIONS || !orig) return orig;

    static char *filtered = NULL;
    if (filtered) return (const GLubyte *)filtered;

    const char *src = (const char *)orig;
    char *out = malloc(strlen(src) + 64);
    out[0] = 0;
    size_t n = 0;
    int has_pvrtc = 0;
    const char *p = src;
    while (*p) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        size_t len = (size_t)(e - p);
        if (len) {
            int hide = 0;
            for (unsigned i = 0; i < sizeof(hidden_extensions) / sizeof(hidden_extensions[0]); i++)
                if (strlen(hidden_extensions[i]) == len && strncmp(p, hidden_extensions[i], len) == 0) hide = 1;
            if (!hide) {
                if (len == strlen("GL_IMG_texture_compression_pvrtc") && strncmp(p, "GL_IMG_texture_compression_pvrtc", len) == 0) has_pvrtc = 1;
                memcpy(out + n, p, len);
                n += len;
                out[n++] = ' ';
            }
        }
        p = e;
    }
    if (!has_pvrtc) {
        const char *pv = "GL_IMG_texture_compression_pvrtc ";
        memcpy(out + n, pv, strlen(pv));
        n += strlen(pv);
    }
    out[n] = 0;
    filtered = out;
    l_info("GL_EXTENSIONS filtered to report PVRTC as the only compressed texture format.");
    return (const GLubyte *)filtered;
}

void gl_swap() {
    vglSwapBuffers(GL_FALSE);
}

/* ---- shader feature flags, to tell which programs light props with vertex light maps ---- */
#define SF_VERTEX_LIGHTMAP 1
#define SF_LIGHTMAP        2
#define SF_DYN_LIGHT       4
#define SF_VERTEX          8
static uint8_t shader_flags[4096];
static uint8_t prog_flags[4096];
static int8_t prog_lm_loc[4096];

static void note_shader_flags(GLuint shader, const char *src) {
    uint8_t f = 0;
    if (strstr(src, "USE_VERTEX_LIGHTMAP 1")) f |= SF_VERTEX_LIGHTMAP;
    if (strstr(src, "USE_LIGHTMAP 1")) f |= SF_LIGHTMAP;
    if (strstr(src, "USE_DYNAMIC_DIRECTIONAL_LIGHT 1")) f |= SF_DYN_LIGHT;
    if (strstr(src, "gl_Position")) f |= SF_VERTEX;
    shader_flags[shader % 4096] = f;
}

void glAttachShader_soloader(GLuint prog, GLuint shader) {
    uint8_t f = shader_flags[shader % 4096];
    if (f & SF_VERTEX) { prog_flags[prog % 4096] = f; prog_lm_loc[prog % 4096] = -2; }
    gldiag_attach(prog, shader);
    glAttachShader(prog, shader);
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glShaderSource<%p>(shader: %i, count: %i, string: %p, length: %p)\n", __builtin_return_address(0), shader, count, string, _length);
#endif
    if (!string) {
        l_error("<%p> Shader source string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    } else if (!*string) {
        l_error("<%p> Shader source *string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    }

    size_t total_length = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            total_length += strlen(string[i]);
        } else {
            total_length += _length[i];
        }
    }

#if DM_SAFE_POW
    /* see dm_config.h: pow(0, 0) and pow(0, x) give NaN (drawn black) on the Vita's shader compiler */
    static const char pow_fix[] = "#define pow(a, b) pow(max((a), 0.0000001), (b))\n";
    const size_t prefix = sizeof(pow_fix) - 1;
#else
    const size_t prefix = 0;
#endif
    total_length += prefix;
    char * str = malloc(total_length+1);
    size_t l = 0;
#if DM_SAFE_POW
    memcpy(str, pow_fix, prefix);
    l = prefix;
#endif

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            memcpy(str + l, string[i], strlen(string[i]));
            l += strlen(string[i]);
        } else {
            memcpy(str + l, string[i], _length[i]);
            l += _length[i];
        }
    }
    str[total_length] = '\0';

    str = gldiag_shader_source(shader, str, &total_length);   /* config.txt switches, material features */
    note_shader_flags(shader, str);
    load_shader(shader, str, total_length);

    free(str);
}

void glCompileShader_soloader(GLuint shader) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glCompileShader<%p>(shader: %i)\n", __builtin_return_address(0), shader);
#endif

#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        int errs = vgl_preproc_errors;
        glCompileShader(shader);
        if (vgl_preproc_errors != errs) {
            static int n;
            if (n++ < 20)
                l_error("[gl] shader %u: vitaGL's GLSL preprocessor failed (%s); a blank shader was used instead", shader,
                        vgl_preproc_error);
        }
#ifdef DUMP_COMPILED_SHADERS
        void *bin = vglMalloc(32 * 1024);
        GLsizei len;
        vglGetShaderBinary(shader, 32 * 1024, &len, bin);
        file_save(next_shader_fname, bin, len);
        vglFree(bin);
#endif
    }
    skip_next_compile = GL_FALSE;
#endif
}

#if defined(USE_GLSL_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else {
        glShaderSource(shader, 1, &string, &length);
        strcpy(next_shader_fname, gxp_path);
    }

    free(sha_name);
}
#elif defined(USE_GLSL_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    glShaderSource(shader, 1, &string, &length);
}
#elif defined(USE_CG_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    char cg_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);
    snprintf(cg_path, sizeof(cg_path), DATA_PATH"cg/%s.cg", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else if (file_exists(cg_path)) {
        char *buffer;
        size_t size;

        file_load(cg_path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);
        strcpy(next_shader_fname, gxp_path);

        free(buffer);
        skip_next_compile = GL_FALSE;
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }

        skip_next_compile = GL_FALSE;
    }

    free(sha_name);
}
#elif defined(USE_CG_SHADERS) || defined(USE_GXP_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char path[256];
#ifdef USE_CG_SHADERS
    snprintf(path, sizeof(path), DATA_PATH"cg/%s.cg", sha_name);
#else
    snprintf(path, sizeof(path), DATA_PATH"gxp/%s.gxp", sha_name);
#endif

    if (file_exists(path)) {
#ifdef USE_CG_SHADERS
        char *buffer;
        size_t size;

        file_load(path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);

        free(buffer);
#else
        uint8_t *buffer;
        size_t size;

        file_load(path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
#endif
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }
    }

    free(sha_name);
}
#else
#error "Define one of (USE_GLSL_SHADERS, USE_CG_SHADERS, USE_GXP_SHADERS)"
#endif

/*
 * The vitaGL in lib/vitagl (master) calls this vitaShaRK function from vglSetShaderAssociationPath(), which this
 * project never calls. Older libvitashark.a builds do not provide it, and that alone fails the link
 * ("undefined reference to shark_set_shader_association_path"). A weak no-op satisfies the linker; a newer
 * libvitashark.a would override it.
 */
__attribute__((weak)) void shark_set_shader_association_path(const char *path) {
    (void)path;
}

/* Report (once per name) attribute and uniform names the engine looks up that the compiled program does not have.
 * Uniforms that the compiler optimised away are normal; a missing vertex attribute such as TangentZ (the normal) or
 * a missing lighting uniform is a lead for meshes that render black. */
static void report_missing(const char *kind, const char *name) {
    static char seen[256][48];
    static int n;
    for (int i = 0; i < n; i++) if (strncmp(seen[i], name, 47) == 0) return;
    if (n >= 256) return;
    strncpy(seen[n], name, 47);
    seen[n][47] = 0;
    n++;
    l_debug("[gl] %s \"%s\" not found in the program", kind, name);
}

static void remember_attr(GLuint prog, GLint loc, const char *name);

GLint glGetAttribLocation_soloader(GLuint prog, const GLchar *name) {
    GLint r = glGetAttribLocation(prog, name);
    if (r < 0 && name) report_missing("attribute", name);
    if (r >= 0 && name) remember_attr(prog, r, name);
    if (name && strcmp(name, "LightMapA") == 0 && (prog_flags[prog % 4096] & SF_VERTEX_LIGHTMAP)) {
        static int n;
        prog_lm_loc[prog % 4096] = (int8_t)r;
        if (n++ < 20)
            l_debug("[gl] program %u uses vertex light maps: LightMapA is at location %d%s", prog, r,
                    r < 0 ? "  <-- MISSING: these props would render black" : "");
    }
    return r;
}

GLint glGetUniformLocation_soloader(GLuint prog, const GLchar *name) {
    GLint r = glGetUniformLocation(prog, name);
    /* vitaGL's uniform locations are pointer-sized and often negative: only -1 means "not in the program" */
    if (r == -1 && name) report_missing("uniform", name);
    gldiag_uniform_location(prog, name, r);
    return r;
}

/* ---- which vertex data feeds which shader input (for the meshes that render black) ----
 * Logged once per distinct combination: attribute name (as the engine looked it up), size, type, normalisation,
 * stride, and whether it comes from a buffer object. Static props lit by vertex light maps read LightMapA; if it is
 * never supplied as an array the props are black. */
static GLuint cur_prog;
static char attr_names[64][16][24];   /* [program % 64][location] */

/* The engine links every program twice (once more after binding its attributes). vitaGL ignores the second link
 * anyway and only writes a "re-linked" warning to its log, about 340 times while the game loads; skip it here. */
void glLinkProgram_soloader(GLuint prog) {
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (linked) {
        static int n;
        if (!n++) l_debug("[gl] program %u linked again by the engine: skipped (vitaGL ignores re-links)", prog);
        return;
    }
    int errs = vgl_preproc_errors;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    glLinkProgram(prog);
    uint32_t dt = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    __sync_fetch_and_add(&dm_link_n, 1);
    __sync_fetch_and_add(&dm_link_us, dt);
    if (dt > dm_link_max_us) dm_link_max_us = dt;
    if (vgl_preproc_errors != errs) {
        static int n;
        if (n++ < 20)
            l_error("[gl] program %u: vitaGL's GLSL preprocessor failed (%s); a blank shader was used instead%s", prog,
                    vgl_preproc_error, n <= 5 ? ", source saved as shader_error_N.txt" : "");
    }
}

void glUseProgram_soloader(GLuint prog) {
    cur_prog = prog;
    gldiag_use_program(prog);
    glUseProgram(prog);
}

static void remember_attr(GLuint prog, GLint loc, const char *name) {
    if (loc < 0 || loc >= 16) return;
    strncpy(attr_names[prog % 64][loc], name, 23);
    attr_names[prog % 64][loc][23] = 0;
}

static void log_attr_once(const char *what) {
    static char seen[160][96];
    static int n;
    for (int i = 0; i < n; i++) if (strcmp(seen[i], what) == 0) return;
    if (n >= 160) return;
    strncpy(seen[n], what, 95);
    seen[n][95] = 0;
    n++;
    l_debug("[gl] %s", what);
}

void glVertexAttribPointer_soloader(GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr) {
    /* fast path: this is called for every draw; only format changes per (program, location) are looked at */
    static uint64_t last_key[64][16];
    uint64_t key = ((uint64_t)(cur_prog + 1) << 40) | ((uint64_t)(type & 0xffff) << 24) | ((uint64_t)(size & 7) << 21) |
                   ((uint64_t)(norm != 0) << 20) | (uint64_t)(stride & 0xfffff);
    if (index >= 16 || last_key[cur_prog % 64][index] == key) {
        glVertexAttribPointer(index, size, type, norm, stride, ptr);
        return;
    }
    last_key[cur_prog % 64][index] = key;
    if ((prog_flags[cur_prog % 4096] & SF_VERTEX_LIGHTMAP) && prog_lm_loc[cur_prog % 4096] == (int)index) {
        static int n;
        if (n++ < 20) {
            GLint vb = 0;
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vb);
            l_debug("[gl] vertex light map data for program %u: %d x 0x%x%s stride %d %s, pointer %p", cur_prog, size,
                    type, norm ? " normalized" : "", stride, vb ? "(buffer)" : "(client memory)", ptr);
        }
    }
    GLint vbo = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vbo);
    char b[96];
    const char *name = index < 16 ? attr_names[cur_prog % 64][index] : "";
    snprintf(b, sizeof(b), "attrib %u \"%s\": %d x 0x%x%s stride %d %s", index, name[0] ? name : "?", size, type,
             norm ? " normalized" : "", stride, vbo ? "(buffer)" : "(client memory)");
    log_attr_once(b);
    glVertexAttribPointer(index, size, type, norm, stride, ptr);
}

void glVertexAttrib4fv_soloader(GLuint index, const GLfloat *v) {
    char b[96];
    const char *name = index < 16 ? attr_names[cur_prog % 64][index] : "";
    snprintf(b, sizeof(b), "attrib %u \"%s\": constant value (no vertex array)", index, name[0] ? name : "?");
    log_attr_once(b);
    glVertexAttrib4fv(index, v);
}

void glVertexAttrib4f_soloader(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    GLfloat v[4] = { x, y, z, w };
    glVertexAttrib4fv_soloader(index, v);
}
