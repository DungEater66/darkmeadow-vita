/*
 * Fixes for how the game draws on the Vita.
 *
 * The engine does not build its shaders from source with #define switches: it sends text that was preprocessed when
 * the game was cooked (AllShaders.bin). So the shader fixes here edit that final text.
 */
#include "gfx/gl_fixes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "gfx/glutil.h"
#include "util/logger.h"

extern so_module so_mod;

volatile uint32_t dm_tex_n, dm_tex_us, dm_tex_bytes;
volatile uint32_t dm_fbo_binds;
volatile uint32_t dm_vram_free, dm_vglram_free, dm_vram_min, dm_vglram_min;

/* Replace every occurrence of `find` in `src`. Returns a new string, or NULL if there was none. */
static char *replace_all(const char *src, const char *find, const char *rep) {
    size_t fl = strlen(find), rl = strlen(rep);
    int hits = 0;
    for (const char *p = src; (p = strstr(p, find)) != NULL; p += fl) hits++;
    if (!hits) return NULL;
    char *out = malloc(strlen(src) + (rl > fl ? (rl - fl) * hits : 0) + 1);
    if (!out) return NULL;
    char *w = out;
    const char *p = src, *d;
    while ((d = strstr(p, find)) != NULL) {
        memcpy(w, p, d - p);
        w += d - p;
        memcpy(w, rep, rl);
        w += rl;
        p = d + fl;
    }
    strcpy(w, p);
    return out;
}

/* Does the source switch `name` on ("#define NAME 1", any spacing)? */
static int defines_on(const char *src, const char *name) {
    size_t nl = strlen(name);
    for (const char *p = src; (p = strstr(p, "#define")) != NULL; p += 7) {
        const char *q = p + 7;
        while (*q == ' ' || *q == '\t') q++;
        if (strncmp(q, name, nl) || (q[nl] != ' ' && q[nl] != '\t')) continue;
        q += nl;
        while (*q == ' ' || *q == '\t') q++;
        return *q == '1';
    }
    return 0;
}

/* ---- the depth pre-pass ----
 * The engine draws some objects twice: first with a depth-only program (its pixel shader writes vec4(0,0,0,0)) to
 * fill the depth buffer, then with the material, depth test LessEqual. The depth-only vertex shaders mostly compute
 * LocalToProjection * Position, while materials that need the world position (environment-mapped metal, dynamically
 * lit characters) compute ViewProjection * (LocalToWorld * Position). On the Vita the two differ in the last bits,
 * the material's depth comes out a hair behind, fails LessEqual, and the object stays black. The pre-pass is pushed
 * back by a tiny amount so the material always wins. */
#define DEPTH_PREPASS_BIAS 0.0002f
#define NSHADERS 4096

static char *vs_src[NSHADERS];          /* vertex shader sources, kept until their program is paired */
static uint8_t is_fragment[NSHADERS];
static uint8_t is_depth_only[NSHADERS];

static void fix_depth_prepass(GLuint prog, GLuint vs, GLuint fs) {
    char *src = vs_src[vs % NSHADERS];
    if (!src) return;
    vs_src[vs % NSHADERS] = NULL;
    char *end = strrchr(src, '}');   /* the end of main() */
    if (is_depth_only[fs % NSHADERS] && !strstr(src, "ProjectedPosition") && end) {
        char add[64];
        snprintf(add, sizeof(add), "gl_Position.z += %.7f * gl_Position.w;\n", (double)DEPTH_PREPASS_BIAS);
        size_t head = end - src, al = strlen(add);
        char *n = malloc(strlen(src) + al + 1);
        if (n) {
            memcpy(n, src, head);
            memcpy(n + head, add, al);
            strcpy(n + head + al, end);
            gl_load_shader(vs, n, strlen(n));
            static int logged;
            if (logged++ < 3) l_info("[gl] depth pre-pass program %u: depth pushed back", (unsigned)prog);
            free(n);
        }
    }
    free(src);
}

void gl_fixes_attach(GLuint prog, GLuint shader) {
    static GLuint last_prog, last_shader;
    unsigned sh = shader % NSHADERS;
    if (last_prog == prog && last_shader && is_fragment[last_shader % NSHADERS] != is_fragment[sh]) {
        if (is_fragment[sh]) fix_depth_prepass(prog, last_shader, shader);
        else fix_depth_prepass(prog, shader, last_shader);
        last_prog = last_shader = 0;
    } else {
        last_prog = prog;
        last_shader = shader;
    }
}

char *gl_fixes_shader_source(GLuint shader, char *src, size_t *len) {
    unsigned sh = shader % NSHADERS;
    is_fragment[sh] = strstr(src, "gl_FragColor") != NULL;
    if (!is_fragment[sh]) {
        free(vs_src[sh]);
        vs_src[sh] = strdup(src);
    } else {
        is_depth_only[sh] = strstr(src, "gl_FragColor = vec4(0,0,0,0);") != NULL;
    }

    /* A mesh without the vertex colour stream its material asks for (the plane on the stairs to floor 17): the engine
     * compiles the material with USE_FALLBACK_STREAM_COLOR, which paints the whole object magenta (UE3's "missing
     * stream" marker). Draw the material normally instead, with white vertex colours. Only shaders that switch it on
     * are edited: vitaGL's shader cache is keyed by the exact source, and every edited shader is compiled again. */
    if (is_fragment[sh] && strstr(src, "#elif USE_FALLBACK_STREAM_COLOR") && defines_on(src, "USE_FALLBACK_STREAM_COLOR")) {
        char *n = replace_all(src, "#elif USE_FALLBACK_STREAM_COLOR", "#elif 0 /* USE_FALLBACK_STREAM_COLOR drawn normally */");
        if (n) {
            free(src);
            src = n;
            static int logged;
            if (logged++ < 5) l_info("[gl] shader %u: mesh without vertex colours drawn normally instead of magenta", (unsigned)shader);
        }
    }
    *len = strlen(src);
    return src;
}

/* ---- draws ----
 * vitaGL draws glDrawArrays through one built-in index buffer of 0xC000 entries; a larger count reads past it and
 * gives garbage geometry. Such draws are split (0xC000 is a multiple of 2, 3 and 4, so lists stay intact; strips and
 * fans cannot be split and are drawn as they are). */
#define MAX_ARRAY_DRAW 0xC000

void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count) {
    if (count > MAX_ARRAY_DRAW && (mode == GL_TRIANGLES || mode == GL_LINES || mode == GL_POINTS)) {
        while (count > 0) {
            GLsizei c = count > MAX_ARRAY_DRAW ? MAX_ARRAY_DRAW : count;
            glDrawArrays(mode, first, c);
            first += c;
            count -= c;
        }
        return;
    }
    if (count > MAX_ARRAY_DRAW) {
        static int warned;
        if (!warned++) l_warn("[gl] glDrawArrays with %d vertices (mode 0x%x) is over vitaGL's limit", (int)count, (unsigned)mode);
    }
    glDrawArrays(mode, first, count);
}

/* ---- texture uploads ----
 * vitaGL ignores GL_UNPACK_ALIGNMENT and always reads rows tightly packed. The engine uploads with an alignment of
 * min(width, 8), so rows whose size is not a multiple of it are repacked before the upload. */
static int unpack_align = 4, unpack_row;

void glPixelStorei_soloader(GLenum pname, GLint param) {
    if (pname == GL_UNPACK_ALIGNMENT) {
        unpack_align = param > 0 ? param : 4;
        return;
    }
    if (pname == GL_UNPACK_ROW_LENGTH) unpack_row = param;
    glPixelStorei(pname, param);
}

static int bytes_per_pixel(GLenum format, GLenum type) {
    int comps;
    switch (format) {
    case GL_ALPHA: case GL_LUMINANCE: comps = 1; break;
    case GL_LUMINANCE_ALPHA: comps = 2; break;
    case GL_RGB: comps = 3; break;
    case GL_RGBA: case 0x80E1 /* GL_BGRA */: comps = 4; break;
    default: return 0;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE: return comps;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1: return 2;
    case GL_FLOAT: return comps * 4;
    case 0x8D61 /* GL_HALF_FLOAT_OES */: return comps * 2;
    default: return 0;
    }
}

void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                           GLint border, GLenum format, GLenum type, const void *pixels) {
    int bpp = bytes_per_pixel(format, type);
    size_t row = (size_t)width * bpp;
    if (pixels && bpp && !unpack_row && unpack_align > 1 && row % unpack_align && height > 1) {
        size_t stride = (row + unpack_align - 1) / unpack_align * unpack_align;
        uint8_t *tight = malloc(row * height);
        if (tight) {
            for (GLsizei y = 0; y < height; y++) memcpy(tight + y * row, (const uint8_t *)pixels + y * stride, row);
            glTexImage2D(target, level, internalformat, width, height, border, format, type, tight);
            free(tight);
            return;
        }
    }
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}

void glCompressedTexImage2D_soloader(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLint border, GLsizei imageSize, const void *data) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
    __sync_fetch_and_add(&dm_tex_us, (uint32_t)(sceKernelGetProcessTimeWide() - t0));
    __sync_fetch_and_add(&dm_tex_n, 1);
    __sync_fetch_and_add(&dm_tex_bytes, (uint32_t)imageSize);
}

/* ---- decal depth bias ----
 * Decals (rubble, stains) are drawn over the floor with glPolygonOffset(slope, units). The engine passes small
 * fractions; vitaGL hands both to sceGxmSetFrontDepthBias as integers, so -0.6 became 0 and decals flickered against
 * the floor. Both are scaled by 6 (the best value found on the console) and rounded away from zero, so a requested
 * offset never disappears. The engine asks for no slope term, and a constant offset alone cannot cover a floor seen
 * at a grazing angle, so a slope of 1 in the same direction is added when only units are given. */
#define DECAL_BIAS_SCALE 6.0f

static float scale_bias(float v) {
    if (v == 0.0f) return 0.0f;
    v *= DECAL_BIAS_SCALE;
    return v < 0.0f ? -(float)(int)(-v + 0.999f) : (float)(int)(v + 0.999f);
}

void glPolygonOffset_soloader(GLfloat factor, GLfloat units) {
    float f = scale_bias(factor), u = scale_bias(units);
    if (factor == 0.0f && units != 0.0f) f = units < 0.0f ? -1.0f : 1.0f;
    glPolygonOffset(f, u);
}

/* Binds of the engine's own framebuffers, for the [gpu] line (vitaGL's MSAA only covers drawing to the screen). */
void glBindFramebuffer_soloader(GLenum target, GLuint fb) {
    if (fb) __sync_fetch_and_add(&dm_fbo_binds, 1);
    glBindFramebuffer(target, fb);
}

/* ---- graphics profile ----
 * The engine picks its graphics settings section from the device it thinks it runs on
 * (appGetMobileSystemSettingsSectionName, which asks IsSupportedSamsungDevice and friends). Anything but the known
 * devices gets [SystemSettingsAndroid], which turns off specular, reflections, normal maps, rim light and fog: metal
 * that gets its look from reflections renders black. The function is replaced so it always answers
 * SystemSettingsAndroid_Samsung. */
#define SETTINGS_SECTION "SystemSettingsAndroid_Samsung"

static uint32_t section_w[64];   /* the engine's wchar_t is 4 bytes */

static const uint32_t *settings_section_name(void) {
    return section_w;
}

void gl_fixes_install_hooks(void) {
    uintptr_t f = (uintptr_t)so_symbol(&so_mod, "_Z37appGetMobileSystemSettingsSectionNamev");
    if (!f) {
        l_warn("[gl] appGetMobileSystemSettingsSectionName not found: graphics profile left to the engine");
        return;
    }
    const char *s = SETTINGS_SECTION;
    size_t i = 0;
    for (; s[i] && i < 63; i++) section_w[i] = (unsigned char)s[i];
    section_w[i] = 0;
    hook_addr(f, (uintptr_t)settings_section_name);
    l_info("[gl] graphics profile: [%s]", SETTINGS_SECTION);
}

/* vitaGL's pools may only be looked at from the rendering thread: sampled here, read by the watchdog. */
void gl_fixes_frame(void) {
    static unsigned tick;
    if (tick++ % 30) return;
    uint32_t v = (uint32_t)vglMemFree(VGL_MEM_VRAM), r = (uint32_t)vglMemFree(VGL_MEM_RAM);
    dm_vram_free = v;
    dm_vglram_free = r;
    if (!dm_vram_min || v < dm_vram_min) dm_vram_min = v;
    if (!dm_vglram_min || r < dm_vglram_min) dm_vglram_min = r;
}
