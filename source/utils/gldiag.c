/*
 * Tools for the props that render solid black (bedpan, bed frame, cabinet, folding stand) and the missing glow.
 *
 * The engine does not compile its shaders from the .msf sources with "#define USE_xxx" switches: it sends text that
 * was preprocessed when the game was cooked (MobileUsePreprocessedShaders; UDKGame/CookedAndroid_PVRTC/
 * PreprocessedShadersInfo.bin indexes 964 of them inside AllShaders.bin). So material features can only be changed by
 * editing that final text. Two tools:
 *
 * 1. Test switches in ux0:data/darkmeadow/config.txt (one per line, "NAME 1"), each replacing one piece of every
 *    pixel shader so a cause can be ruled in or out on the console without rebuilding:
 *        DM_TEST_WHITE        every surface pure white: shows whether the black props are drawn at all
 *        DM_TEST_BASE_ONLY    only the base (colour) texture, no lighting at all: black = the texture itself
 *        DM_TEST_NO_LIGHTMAP  light maps replaced by mid grey: static props lit by a broken light map recover
 *        DM_TEST_NO_LIGHTING  dynamic lighting (sky + directional light) skipped
 *        DM_TEST_NO_ENV       environment (reflection) texture replaced by black
 *    A line "NAME VALUE" with any other upper-case name still replaces "#define NAME" (only matters for the few
 *    shaders compiled from source). Shaders are compiled again once after a change (the shader cache is keyed by the
 *    source text).
 *
 * 2. Diagnostics: for each program, at its first real draw (the engine first draws every program once without
 *    textures while it loads, to warm the shaders up; those draws are skipped), log the textures it samples (size and
 *    format; "NO TEXTURE" is a red flag) and the lighting uniforms it was given.
 *
 * Also: GL_UNPACK_ALIGNMENT. vitaGL ignores it and always reads rows tightly packed. The engine uploads with an
 * alignment of min(width, 8), so rows whose size is not a multiple of that are repacked here before the upload.
 */
#include "utils/gldiag.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <sys/stat.h>

#include "utils/logger.h"
#include "utils/pvrtc.h"

#ifndef DATA_PATH
#define DATA_PATH "ux0:data/darkmeadow/"
#endif

#define NPROG 4096

/* ---- config.txt ---- */

typedef struct { char name[48]; char value[16]; int used; } override_t;
static override_t ovr[24];
static int novr = -1;

typedef struct {
    const char *name;
    const char *find, *replace;
    const char *needs;          /* only in shaders containing this (NULL: all pixel shaders) */
    int on, used;
} test_t;
static test_t tests[] = {
    { "DM_TEST_WHITE", "gl_FragColor =  PolyColor ;", "gl_FragColor = vec4(1.0, 1.0, 1.0, PolyColor.a) ;", NULL, 0, 0 },
    { "DM_TEST_BASE_ONLY", "gl_FragColor =  PolyColor ;", "gl_FragColor = vec4(BaseColor.rgb, PolyColor.a) ;",
      "lowp vec4 BaseColor =", 0, 0 },
    { "DM_TEST_NO_LIGHTMAP", "texture2D( TextureLightmap, UVLightmap )", "vec4(0.5, 0.5, 0.5, 1.0)", NULL, 0, 0 },
    { "DM_TEST_NO_LIGHTING", "PolyColor.rgb *= TotalDiffuseLight;", ";", NULL, 0, 0 },
    { "DM_TEST_NO_ENV", "texture2D( TextureEnvironment, EnvironmentTexCoord )", "vec4(0.0)", NULL, 0, 0 },
    /* texture coordinates as colour (red = U, green = V, repeating every 1.0): a surface that comes out one flat
     * colour gets broken texture coordinates */
    { "DM_TEST_SHOW_UV", "gl_FragColor =  PolyColor ;", "gl_FragColor = vec4(fract(UVBase.x), fract(UVBase.y), 0.0, 1.0) ;",
      "TotalDiffuseLight", 0, 0 },
};
#define NTESTS (int)(sizeof(tests) / sizeof(tests[0]))

/* The depth pre-pass (see fix_depth_prepass) */
static float depth_bias = 0.0002f;
static int no_prepass;
/* texture test switches (see the texture section) */
static int no_mipmaps, pvrtc_software;
static int false_colour;
static int fallback_fix = 1;   /* DM_FALLBACK_STREAM_FIX, see gldiag_shader_source */
static int dump_textures;
static void write_preview(GLint id, const uint8_t *rgba, int w, int h, const char *tag);
static void trace_upload(int compressed, GLint level, int w, int h, unsigned fmt, int bytes, const void *data);
static void trace_full(GLint id, const uint8_t *rgba, int w, int h, const char *tag);
static int tex_tint;
static int trace_tex = -1;   /* DM_TEST_TRACE_TEXTURE <id> */
static float bias_scale = 6.0f;
static float decal_slope = 1.0f;
static char settings_section[64] = "SystemSettingsAndroid_Samsung";
static void min_filter_no_mips(void);

static void load_config(void) {
    novr = 0;
    FILE *f = fopen(DATA_PATH "config.txt", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char name[64], value[32];
        if (sscanf(line, "%63s %31s", name, value) != 2) continue;
        if (!strcmp(name, "DM_DEPTH_PREPASS_BIAS")) {
            depth_bias = (float)atof(value);
            l_info("[gl] config.txt: depth pre-pass pushed back by %g", (double)depth_bias);
            continue;
        }
        if (!strcmp(name, "DM_TEST_FALSE_COLOUR") || !strcmp(name, "DM_TEST_FALSE_COLOR")) {
            false_colour = atoi(value) != 0;
            l_info("[gl] config.txt: DM_TEST_FALSE_COLOUR %s", value);
            continue;
        }
        if (!strcmp(name, "DM_FALLBACK_STREAM_FIX")) {
            fallback_fix = atoi(value) != 0;
            l_info("[gl] config.txt: DM_FALLBACK_STREAM_FIX %s", value);
            continue;
        }
        if (!strcmp(name, "DM_DECAL_BIAS_SCALE")) {
            bias_scale = (float)atof(value);
            l_info("[gl] config.txt: DM_DECAL_BIAS_SCALE %s", value);
            continue;
        }
        if (!strcmp(name, "DM_DECAL_SLOPE")) {
            decal_slope = (float)atof(value);
            l_info("[gl] config.txt: DM_DECAL_SLOPE %s", value);
            continue;
        }
        if (!strcmp(name, "DM_SETTINGS_SECTION")) {
            snprintf(settings_section, sizeof(settings_section), "%s", value);
            l_info("[gl] config.txt: DM_SETTINGS_SECTION %s", value);
            continue;
        }
        if (!strcmp(name, "DM_TEST_TEX_TINT")) {
            tex_tint = atoi(value) != 0;
            l_info("[gl] config.txt: DM_TEST_TEX_TINT %s (base textures replaced by a colour encoding their number)", value);
            continue;
        }
        if (!strcmp(name, "DM_TEST_TRACE_TEXTURE")) {
            trace_tex = atoi(value);
            l_info("[gl] config.txt: DM_TEST_TRACE_TEXTURE %d (its uploads, creation and deletion logged; full-size images "
                   "in " DATA_PATH "texdump/)", trace_tex);
            continue;
        }
        if (!strcmp(name, "DM_TEST_DUMP_TEXTURES")) {
            dump_textures = atoi(value) != 0;
            l_info("[gl] config.txt: DM_TEST_DUMP_TEXTURES %s (previews in " DATA_PATH "texdump/)", value);
            continue;
        }
        if (!strcmp(name, "DM_TEST_NO_MIPMAPS")) {
            no_mipmaps = atoi(value) != 0;
            l_info("[gl] config.txt: DM_TEST_NO_MIPMAPS %s (textures sample only their full-size level)", value);
            continue;
        }
        if (!strcmp(name, "DM_TEST_PVRTC_SOFTWARE")) {
            pvrtc_software = atoi(value) != 0;
            l_info("[gl] config.txt: DM_TEST_PVRTC_SOFTWARE %s (PVRTC decoded by the loader, uploaded as RGBA)", value);
            continue;
        }
        if (!strcmp(name, "DM_TEST_NO_DEPTH_PREPASS")) {
            no_prepass = atoi(value) != 0;
            l_info("[gl] config.txt: DM_TEST_NO_DEPTH_PREPASS %s", value);
            continue;
        }
        if (strncmp(name, "DM_TEST_", 8) == 0) {
            int known = 0;
            for (int i = 0; i < NTESTS; i++)
                if (!strcmp(name, tests[i].name)) { tests[i].on = atoi(value) != 0; known = 1; }
            if (known) l_info("[gl] config.txt: %s %s", name, value);
            else l_warn("[gl] config.txt: unknown test switch %s", name);
            continue;
        }
        if (!strncmp(name, "DM_", 3)) continue;   /* the loader's own settings (e.g. DM_DEVICE_MANUFACTURER) */
        if (!isupper((unsigned char)name[0]) || !strchr(name, '_')) continue;   /* not a shader #define */
        if (novr >= (int)(sizeof(ovr) / sizeof(ovr[0]))) continue;
        snprintf(ovr[novr].name, sizeof(ovr[novr].name), "%s", name);
        snprintf(ovr[novr].value, sizeof(ovr[novr].value), "%s", value);
        l_info("[gl] config.txt: shaders compiled from source will use #define %s %s", name, value);
        novr++;
    }
    fclose(f);
}

/* Replace every occurrence of `find`. Returns a new string, or NULL if there was none. */
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
        memcpy(w, p, d - p); w += d - p;
        memcpy(w, rep, rl); w += rl;
        p = d + fl;
    }
    strcpy(w, p);
    return out;
}

/* Replace the value in "#define NAME <value>" (every occurrence). Returns a new string, or NULL if nothing changed. */
static char *apply_override(const char *src, const override_t *o) {
    size_t nl = strlen(o->name);
    const char *p = src;
    size_t extra = 0;
    int hits = 0;
    while ((p = strstr(p, "#define ")) != NULL) {
        const char *q = p + 8;
        while (*q == ' ' || *q == '\t') q++;
        if (strncmp(q, o->name, nl) == 0 && (q[nl] == ' ' || q[nl] == '\t')) { hits++; extra += strlen(o->value); }
        p = q;
    }
    if (!hits) return NULL;
    char *out = malloc(strlen(src) + extra + 1);
    if (!out) return NULL;
    char *w = out;
    p = src;
    const char *d;
    while ((d = strstr(p, "#define ")) != NULL) {
        const char *q = d + 8;
        while (*q == ' ' || *q == '\t') q++;
        if (strncmp(q, o->name, nl) == 0 && (q[nl] == ' ' || q[nl] == '\t')) {
            const char *v = q + nl;
            while (*v == ' ' || *v == '\t') v++;
            const char *e = v;
            while (*e && !isspace((unsigned char)*e)) e++;   /* the old value */
            memcpy(w, p, v - p); w += v - p;
            size_t vl = strlen(o->value);
            memcpy(w, o->value, vl); w += vl;
            p = e;
        } else {
            memcpy(w, p, q - p); w += q - p;
            p = q;
        }
    }
    strcpy(w, p);
    return out;
}

/* ---- per program: samplers and lighting uniforms ---- */

#define MAXSMP 8
#define MAXUNI 8
static const char *const tracked_uniforms[] = {
    "EnvironmentColorScale", "EnvironmentParameters", "DirectionalLightColor", "UpperSkyColor", "LowerSkyColor",
    "LightmapCoordinateScaleBias", "FadeColorAndAmount", "LightDirectionAndbDirectional",
};
#define NTRACKED (int)(sizeof(tracked_uniforms) / sizeof(tracked_uniforms[0]))

typedef struct {
    uint8_t logged;
    uint16_t draws;
    int nsmp;
    GLint smp_loc[MAXSMP];        /* vitaGL's locations are pointer-sized values, often negative; only -1 is "none" */
    int8_t smp_unit[MAXSMP];      /* -1: never set (GL default: unit 0) */
    char smp_name[MAXSMP][24];
    GLint uni_loc[NTRACKED];
    float uni_val[NTRACKED][4];
    uint8_t uni_n[NTRACKED];      /* components set (0: never set) */
} progdiag_t;

static progdiag_t *pd[NPROG];
static uint8_t is_frag[NPROG];
static GLuint cur_prog;

/* ---- the depth pre-pass ----
 * The engine draws some objects twice: first with a "depth-only" program (pixel shader output vec4(0,0,0,0), no colour
 * mask: it really writes black) to fill the depth buffer, then with the material, depth test LessEqual (the engine's
 * only depth states are LessEqual and Always). The depth-only vertex shaders mostly compute the position as
 * LocalToProjection * Position, while every material that needs the world position - environment-mapped metal,
 * dynamically lit characters - computes ViewProjection * (LocalToWorld * Position). On the Vita the two results differ
 * in the last bits, the material's depth comes out a hair behind the pre-pass, fails LessEqual, and the object stays
 * the pre-pass black: the black bedpan, bed frame, cabinet and stand. Push the pre-pass depth back by a tiny amount
 * (DM_DEPTH_PREPASS_BIAS in config.txt, default 0.0002 of the clip range; 0 = off) so the material always wins.
 * DM_TEST_NO_DEPTH_PREPASS 1 instead moves the pre-pass geometry out of view (no pre-pass at all). */
extern void load_shader(GLuint shader, const char *string, size_t length);
static char *vs_src[NPROG];          /* vertex shader sources, until their program is paired */
static uint8_t fs_depthonly[NPROG];

/* Called with a program's two shaders (the engine attaches them one right after the other). */
static void fix_depth_prepass(GLuint prog, GLuint vs, GLuint fs) {
    char *src = vs_src[vs % NPROG];
    if (!src) return;
    vs_src[vs % NPROG] = NULL;
    if (fs_depthonly[fs % NPROG] && !strstr(src, "ProjectedPosition") && (depth_bias != 0.f || no_prepass)) {
        char *end = strrchr(src, '}');   /* the end of main() */
        if (end) {
            char add[96];
            if (no_prepass) snprintf(add, sizeof(add), "gl_Position = vec4(0.0, 0.0, 2.0, 1.0);\n");
            else snprintf(add, sizeof(add), "gl_Position.z += %.7f * gl_Position.w;\n", (double)depth_bias);
            size_t head = end - src, al = strlen(add);
            char *n = malloc(strlen(src) + al + 1);
            if (n) {
                memcpy(n, src, head);
                memcpy(n + head, add, al);
                strcpy(n + head + al, end);
                load_shader(vs, n, strlen(n));   /* vitaGL compiles at link time, so this replaces the source */
                static int logged;
                if (logged++ < 3) l_info("[gl] depth pre-pass program %u: %s", prog, no_prepass ? "removed" : "depth pushed back");
                free(n);
            }
        }
    }
    free(src);
}

static void ensure_config(void) { if (novr < 0) load_config(); }

/* DM_TEST_FALSE_COLOUR: paint each kind of world-material shader in its own colour (lightly mixed with its base
 * texture so the room stays readable), to see which kind of draw a broken object is:
 *   red = depth-only pre-pass (normally black)   magenta = environment-mapped (metal)   cyan = emissive
 *   green = light-mapped   blue = dynamically lit   yellow = other material
 * UI, particles and post-processing keep their normal look. */
static char *apply_false_colour(char *src) {
    char *n = NULL;
    const char *what;
    if (strstr(src, "gl_FragColor = vec4(0,0,0,0);")) {
        n = replace_all(src, "gl_FragColor = vec4(0,0,0,0);", "gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);");
        what = "depth-only";
    } else if ((strstr(src, "TotalDiffuseLight") || strstr(src, "EnvironmentSpecular")) &&
               strstr(src, "gl_FragColor =  PolyColor ;")) {
        const char *c;
        if (strstr(src, "TextureEnvironment")) { c = "1.0, 0.0, 1.0"; what = "environment-mapped"; }
        else if (strstr(src, "TextureEmissive")) { c = "0.0, 1.0, 1.0"; what = "emissive"; }
        else if (strstr(src, "TextureLightmap")) { c = "0.0, 1.0, 0.0"; what = "light-mapped"; }
        else if (strstr(src, "PolyColor.rgb *= TotalDiffuseLight;")) { c = "0.0, 0.3, 1.0"; what = "dynamically lit"; }
        else { c = "1.0, 1.0, 0.0"; what = "other material"; }
        char rep[160];
        if (strstr(src, "lowp vec4 BaseColor ="))
            snprintf(rep, sizeof(rep), "gl_FragColor = vec4(mix(vec3(%s), BaseColor.rgb, 0.3), 1.0) ;", c);
        else
            snprintf(rep, sizeof(rep), "gl_FragColor = vec4(%s, 1.0) ;", c);
        n = replace_all(src, "gl_FragColor =  PolyColor ;", rep);
    } else
        return src;
    if (!n) return src;
    static int logged;
    if (logged++ < 1) l_info("[gl] DM_TEST_FALSE_COLOUR is painting shaders (first: %s)", what);
    free(src);
    return n;
}

/* Does the source switch `name` on ("#define NAME 1", any spacing)? Only those shaders are edited: vitaGL's shader
 * cache is keyed by the exact source text, so touching every shader that merely contains the #elif made r51 compile
 * 18 programs again at about 1.1 s each, in the middle of play. */
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

char *gldiag_shader_source(GLuint shader, char *src, size_t *len) {
    ensure_config();
    for (int i = 0; i < novr; i++) {
        char *n = apply_override(src, &ovr[i]);
        if (n) {
            free(src);
            src = n;
            if (!ovr[i].used) { ovr[i].used = 1; l_info("[gl] config.txt switch %s=%s applied", ovr[i].name, ovr[i].value); }
        }
    }
    unsigned sh = shader % NPROG;
    is_frag[sh] = strstr(src, "gl_FragColor") != NULL;
    if (!is_frag[sh]) {
        free(vs_src[sh]);
        vs_src[sh] = strdup(src);
    } else
        fs_depthonly[sh] = strstr(src, "gl_FragColor = vec4(0,0,0,0);") != NULL;
    /* The magenta plane on the stairs to floor 17: a mesh without the vertex colour stream its material asks for. The
     * engine then compiles the material with USE_FALLBACK_STREAM_COLOR: the vertex shader uses white for the missing
     * colours, and the pixel shader paints the whole object magenta - UE3's "missing stream" marker. Let the pixel
     * shader draw the material normally instead (with the white vertex colour). DM_FALLBACK_STREAM_FIX 0 = magenta. */
    if (is_frag[sh] && fallback_fix && strstr(src, "#elif USE_FALLBACK_STREAM_COLOR") && defines_on(src, "USE_FALLBACK_STREAM_COLOR")) {
        char *n = replace_all(src, "#elif USE_FALLBACK_STREAM_COLOR", "#elif 0 /* DM: USE_FALLBACK_STREAM_COLOR drawn normally */");
        if (n) {
            free(src);
            src = n;
            static int logged;
            if (logged++ < 5)
                l_info("[gl] shader %u: material for a mesh missing its vertex colours, drawn normally instead of magenta", shader);
        }
    }
    if (is_frag[sh] && false_colour) src = apply_false_colour(src);
    if (is_frag[sh]) {
        for (int i = 0; i < NTESTS; i++) {
            if (!tests[i].on || (tests[i].needs && !strstr(src, tests[i].needs))) continue;
            char *n = replace_all(src, tests[i].find, tests[i].replace);
            if (n) {
                free(src);
                src = n;
                tests[i].used++;
                if (tests[i].used == 1) l_info("[gl] %s is changing pixel shaders", tests[i].name);
            }
        }
    }
    *len = strlen(src);
    return src;
}

void gldiag_attach(GLuint prog, GLuint shader) {
    unsigned p = prog % NPROG, sh = shader % NPROG;
    static GLuint last_prog, last_shader;
    if (last_prog == prog && last_shader && is_frag[last_shader % NPROG] != is_frag[sh]) {
        if (is_frag[sh]) fix_depth_prepass(prog, last_shader, shader);
        else fix_depth_prepass(prog, shader, last_shader);
        last_prog = last_shader = 0;
    } else {
        last_prog = prog;
        last_shader = shader;
    }
    if (!is_frag[sh]) return;
    if (!pd[p]) pd[p] = malloc(sizeof(progdiag_t));
    if (!pd[p]) return;
    memset(pd[p], 0, sizeof(progdiag_t));
    for (int i = 0; i < NTRACKED; i++) pd[p]->uni_loc[i] = -1;
}

void gldiag_uniform_location(GLuint prog, const char *name, GLint loc) {
    progdiag_t *d = pd[prog % NPROG];
    if (!d || !name || loc == -1) return;
    for (int i = 0; i < NTRACKED; i++)
        if (!strcmp(name, tracked_uniforms[i])) { d->uni_loc[i] = loc; return; }
    if (strstr(name, "Texture") && strcmp(name, "TextureTransform") && d->nsmp < MAXSMP) {
        for (int i = 0; i < d->nsmp; i++) if (d->smp_loc[i] == loc) return;
        d->smp_loc[d->nsmp] = loc;
        d->smp_unit[d->nsmp] = -1;
        snprintf(d->smp_name[d->nsmp], sizeof(d->smp_name[0]), "%s", name);
        d->nsmp++;
    }
}

void gldiag_use_program(GLuint prog) { cur_prog = prog; }

void glUniform1i_soloader(GLint loc, GLint v) {
    progdiag_t *d = pd[cur_prog % NPROG];
    if (d && loc != -1)
        for (int i = 0; i < d->nsmp; i++)
            if (d->smp_loc[i] == loc) { d->smp_unit[i] = (int8_t)v; break; }
    glUniform1i(loc, v);
}

static void note_vec(GLint loc, const GLfloat *v, int n) {
    progdiag_t *d = pd[cur_prog % NPROG];
    if (!d || d->logged || loc == -1 || !v) return;
    for (int i = 0; i < NTRACKED; i++)
        if (d->uni_loc[i] == loc) { memcpy(d->uni_val[i], v, n * sizeof(float)); d->uni_n[i] = (uint8_t)n; return; }
}

void glUniform3fv_soloader(GLint loc, GLsizei count, const GLfloat *v) {
    note_vec(loc, v, 3);
    glUniform3fv(loc, count, v);
}

void glUniform4fv_soloader(GLint loc, GLsizei count, const GLfloat *v) {
    note_vec(loc, v, 4);
    glUniform4fv(loc, count, v);
}

/* ---- textures: remember sizes and formats, for the log ---- */

typedef struct { GLuint id; uint16_t w, h; uint32_t fmt; uint8_t compressed; int8_t first_level; uint32_t levels; } texinfo_t;
static texinfo_t texinfo[8192];

static GLint current_texture(void) {
    GLint id = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
    return id;
}

/* Remember every upload: size and format of level 0, which levels arrived, and in which order. */
static void note_texture(GLint level, GLsizei w, GLsizei h, uint32_t fmt, int compressed) {
    GLint id = current_texture();
    texinfo_t *t = &texinfo[(unsigned)id % 8192];
    if (t->id != (GLuint)id || (level == 0 && (t->levels & 1))) {   /* a new texture, or level 0 uploaded again */
        memset(t, 0, sizeof(*t));
        t->id = (GLuint)id;
        t->first_level = (int8_t)level;
        if (level > 0) {
            static int n;
            if (n++ < 20)
                l_warn("[tex] texture %d: first upload is mip level %d (%dx%d, format 0x%x) - level 0 was never uploaded", id,
                       level, w, h, (unsigned)fmt);
        }
    }
    if (level == 0) { t->w = (uint16_t)w; t->h = (uint16_t)h; t->fmt = fmt; t->compressed = (uint8_t)compressed; }
    if (level < 32) t->levels |= 1u << level;
}

static const char *pvrtc_name(uint32_t f) {
    switch (f) {
    case 0x8C00: return "PVRTC 4bpp RGB";
    case 0x8C01: return "PVRTC 2bpp RGB";
    case 0x8C02: return "PVRTC 4bpp RGBA";
    case 0x8C03: return "PVRTC 2bpp RGBA";
    default: return NULL;
    }
}

static void describe_unit(int unit, char *out, size_t n) {
    if (unit < 0) { snprintf(out, n, "unit ?"); return; }
    GLint active = 0, id = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0 + unit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
    glActiveTexture(active);
    const texinfo_t *t = &texinfo[(unsigned)id % 8192];
    if (!id) snprintf(out, n, "unit %d: NO TEXTURE", unit);
    else if (t->id == (GLuint)id && t->w) {
        int nl = 0;
        for (uint32_t m = t->levels; m; m >>= 1) nl += m & 1;
        const char *pn = pvrtc_name(t->fmt);
        if (pn) snprintf(out, n, "unit %d: tex %d %ux%u %s, %d levels", unit, id, t->w, t->h, pn, nl);
        else snprintf(out, n, "unit %d: tex %d %ux%u %s 0x%x, %d levels", unit, id, t->w, t->h,
                      t->compressed ? "compressed" : "format", (unsigned)t->fmt, nl);
    }
    else snprintf(out, n, "unit %d: tex %d (never uploaded)", unit, id);
}


static GLint bound_on_unit(int unit) {
    GLint active = 0, id = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0 + unit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
    glActiveTexture(active);
    return id;
}

/* Report each program once, at its first real draw: the first one with a texture on every sampler it uses (the
 * shader warm-up draws during loading have none), or its 60th draw if that never happens. Up to 400 programs. */
static void maybe_report(progdiag_t *d) {
    if (d->draws < 60) {
        d->draws++;
        int all = d->nsmp > 0;
        for (int i = 0; i < d->nsmp && all; i++) all = bound_on_unit(d->smp_unit[i] >= 0 ? d->smp_unit[i] : 0) != 0;
        if (!all && d->draws < 60) return;
    }
    d->logged = 1;
    static int n;
    if (n++ >= 400) return;
    char line[700];
    size_t k = 0;
    line[0] = 0;
    for (int i = 0; i < d->nsmp && k < sizeof(line) - 120; i++) {
        char u[100];
        describe_unit(d->smp_unit[i] >= 0 ? d->smp_unit[i] : 0, u, sizeof(u));
        k += snprintf(line + k, sizeof(line) - k, "%s%s %s%s", i ? " | " : "", d->smp_name[i], u,
                      d->smp_unit[i] < 0 ? " (unit never set)" : "");
    }
    l_debug("[gl] program %u draws (after %u): %s", cur_prog, d->draws, d->nsmp ? line : "no textures");
    k = 0;
    line[0] = 0;
    for (int i = 0; i < NTRACKED && k < sizeof(line) - 80; i++) {
        if (d->uni_loc[i] == -1) continue;
        if (!d->uni_n[i]) k += snprintf(line + k, sizeof(line) - k, " %s=never set", tracked_uniforms[i]);
        else if (d->uni_n[i] == 3)
            k += snprintf(line + k, sizeof(line) - k, " %s=(%.2f %.2f %.2f)", tracked_uniforms[i], d->uni_val[i][0],
                          d->uni_val[i][1], d->uni_val[i][2]);
        else
            k += snprintf(line + k, sizeof(line) - k, " %s=(%.2f %.2f %.2f %.2f)", tracked_uniforms[i], d->uni_val[i][0],
                          d->uni_val[i][1], d->uni_val[i][2], d->uni_val[i][3]);
    }
    if (k) l_debug("[gl]   %s", line + 1);
}

/* The first time each texture is drawn with (on the program's TextureBase unit): its size, format and mip levels,
 * and a warning if it was never uploaded. Up to 400 textures. */
static void note_draw_texture(progdiag_t *d) {
    if (!d || !d->nsmp) return;
    int unit = d->smp_unit[0] >= 0 ? d->smp_unit[0] : 0;    /* the first sampler the engine looks up is TextureBase */
    GLint id = bound_on_unit(unit);
    static uint8_t seen[8192];
    static int n;
    if (id <= 0 || seen[id % 8192] || n >= 400) return;
    seen[id % 8192] = 1;
    n++;
    char u[120];
    describe_unit(unit, u, sizeof(u));
    const texinfo_t *t = &texinfo[(unsigned)id % 8192];
    int never = !(t->id == (GLuint)id && (t->levels & 1));
    if (never) l_warn("[draw] program %u draws with a texture that has no level 0: %s", cur_prog, u);
    else l_debug("[draw] program %u first use of %s", cur_prog, u);
}

/* DM_TEST_TEX_TINT: while a program draws, its TextureBase is swapped for a 1x1 texture whose colour encodes the real
 * texture's number: red = (n % 8) * 36, green = ((n / 8) % 8) * 36, blue = ((n / 64) % 8) * 36. With DM_TEST_BASE_ONLY
 * the colour reaches the screen unlit, so a screenshot tells which texture an object is drawn with. */
static GLuint tint_tex[512];
static int tint_begin(progdiag_t *d, GLint *active, GLint *orig) {
    if (!tex_tint || !d || !d->nsmp) return -1;
    int unit = d->smp_unit[0] >= 0 ? d->smp_unit[0] : 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, active);
    glActiveTexture(GL_TEXTURE0 + unit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, orig);
    if (*orig <= 0) { glActiveTexture(*active); return -1; }
    unsigned n = (unsigned)*orig % 512;
    if (!tint_tex[n]) {
        uint8_t px[4] = { (uint8_t)((n % 8) * 36), (uint8_t)(((n / 8) % 8) * 36), (uint8_t)(((n / 64) % 8) * 36), 255 };
        glGenTextures(1, &tint_tex[n]);
        glBindTexture(GL_TEXTURE_2D, tint_tex[n]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        l_debug("[tint] texture %d is colour (%d, %d, %d)", (int)*orig, px[0], px[1], px[2]);
    } else
        glBindTexture(GL_TEXTURE_2D, tint_tex[n]);
    return unit;
}
static void tint_end(int unit, GLint active, GLint orig) {
    if (unit < 0) return;
    glBindTexture(GL_TEXTURE_2D, (GLuint)orig);
    glActiveTexture(active);
}

void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    progdiag_t *d = pd[cur_prog % NPROG];
#if defined(DEBUG_SOLOADER) && !defined(DM_PROFILE)   /* per-draw diagnostics: Debug builds only */
    if (d && !d->logged) maybe_report(d);
    if (d && d->logged) note_draw_texture(d);
#endif
    GLint ta = 0, to = 0;
    int tu = tint_begin(d, &ta, &to);
    glDrawElements(mode, count, type, indices);
    tint_end(tu, ta, to);
}

/* vitaGL draws glDrawArrays through one built-in index buffer of 0xC000 entries: a larger count reads past it and
 * gives garbage geometry without any error. Split such draws (0xC000 is a multiple of 2, 3 and 4, so lists of lines,
 * triangles and quads stay intact; strips and fans cannot be split this way and are drawn as they are). */
#define MAX_ARRAY_DRAW 0xC000
void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count) {
    progdiag_t *d = pd[cur_prog % NPROG];
#if defined(DEBUG_SOLOADER) && !defined(DM_PROFILE)   /* per-draw diagnostics: Debug builds only */
    if (d && !d->logged) maybe_report(d);
    if (d && d->logged) note_draw_texture(d);
#endif
    if (count > MAX_ARRAY_DRAW && (mode == GL_TRIANGLES || mode == GL_LINES || mode == GL_POINTS)) {
        static int warned;
        if (!warned++) l_warn("[gl] glDrawArrays with %d vertices: split into pieces of %d (vitaGL limit)", count, MAX_ARRAY_DRAW);
        while (count > 0) {
            GLsizei c = count > MAX_ARRAY_DRAW ? MAX_ARRAY_DRAW : count;
            glDrawArrays(mode, first, c);
            first += c;
            count -= c;
        }
        return;
    }
    if (count > MAX_ARRAY_DRAW) {
        static int warned2;
        if (!warned2++) l_warn("[gl] glDrawArrays with %d vertices (mode 0x%x) is over vitaGL's limit of %d", count, mode, MAX_ARRAY_DRAW);
    }
    glDrawArrays(mode, first, count);
}

/* ---- GL_UNPACK_ALIGNMENT ---- */

static int unpack_align = 4, unpack_row = 0;

void glPixelStorei_soloader(GLenum pname, GLint param) {
    if (pname == GL_UNPACK_ALIGNMENT) { unpack_align = param > 0 ? param : 4; return; }   /* vitaGL rejects it */
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
    ensure_config();
    note_texture(level, width, height, (uint32_t)internalformat, 0);
    {
        static int logged;
        if (level == 0 && logged++ < 60)
            l_debug("[tex] texture %d: glTexImage2D %dx%d internal 0x%x format 0x%x type 0x%x%s", current_texture(), width,
                    height, (unsigned)internalformat, (unsigned)format, (unsigned)type, pixels ? "" : " (no data)");
    }
    int bpp = bytes_per_pixel(format, type);
    size_t row = (size_t)width * bpp;
    if (pixels && bpp && !unpack_row && unpack_align > 1 && row % unpack_align && height > 1) {
        size_t stride = (row + unpack_align - 1) / unpack_align * unpack_align;
        uint8_t *tight = malloc(row * height);
        if (tight) {
            static int n;
            if (n++ < 10) l_debug("[gl] repacking a %dx%d texture upload (rows padded to %d bytes)", width, height, unpack_align);
            for (GLsizei y = 0; y < height; y++) memcpy(tight + y * row, (const uint8_t *)pixels + y * stride, row);
            glTexImage2D(target, level, internalformat, width, height, border, format, type, tight);
            free(tight);
            if (no_mipmaps && level == 0) min_filter_no_mips();
            return;
        }
    }
    trace_upload(0, level, width, height, (unsigned)internalformat, width * height * bytes_per_pixel(format, type), pixels);
    if (dump_textures && pixels && level == 0 && target == GL_TEXTURE_2D && type == GL_UNSIGNED_BYTE &&
        (format == GL_RGBA || format == GL_RGB) && width > 0 && height > 0) {
        uint8_t *rgba = malloc((size_t)width * height * 4);
        if (rgba) {
            const uint8_t *src = pixels;
            int sb = format == GL_RGBA ? 4 : 3;
            for (size_t k = 0; k < (size_t)width * height; k++) {
                rgba[4 * k] = src[sb * k]; rgba[4 * k + 1] = src[sb * k + 1]; rgba[4 * k + 2] = src[sb * k + 2];
                rgba[4 * k + 3] = sb == 4 ? src[sb * k + 3] : 255;
            }
            write_preview(current_texture(), rgba, width, height, sb == 4 ? "rgba" : "rgb");
            free(rgba);
        }
    }
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    if (no_mipmaps && level == 0) min_filter_no_mips();
}

/* DM_TEST_DUMP_TEXTURES: write a 64x64 (at most) preview of every PVRTC level 0 the engine uploads, decoded by the
 * loader, as an uncompressed TGA: texdump/tex<id>_<w>x<h>_<n>.tga (n counts uploads, as level 0 can be uploaded again).
 * The texture numbers match the [tex] / [draw] lines in the log. Since r50: also uncompressed RGB/RGBA uploads, identical
 * images (a texture loaded again at every battle) are written once, and up to 3000 files (r49: 600, reached before
 * floor 17). */
static void write_preview(GLint id, const uint8_t *rgba, int w, int h, const char *tag) {
    static int n, made_dir, skipped;
    static uint32_t seen[4096];
    static int nseen;
    if (n >= 3000) return;
    int ow = w > 64 ? 64 : w, oh = h > 64 ? 64 : h;
    uint8_t *img = malloc((size_t)ow * oh * 4);
    if (!img) return;
    uint32_t hash = 2166136261u ^ (uint32_t)(w * 31 + h);
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            const uint8_t *p = rgba + 4 * ((size_t)(y * h / oh) * w + (x * w / ow));
            uint8_t *q = img + 4 * (y * ow + x);
            q[0] = p[2]; q[1] = p[1]; q[2] = p[0]; q[3] = p[3];   /* TGA is BGRA */
            hash = (hash ^ p[0] ^ (p[1] << 8) ^ (p[2] << 16) ^ ((uint32_t)p[3] << 24)) * 16777619u;
        }
    for (int k = 0; k < nseen; k++)
        if (seen[k] == hash) { free(img); if (skipped++ < 3) l_debug("[tex] texture %d: same image as one already dumped", id); return; }
    if (nseen < 4096) seen[nseen++] = hash;
    uint8_t hdr[18] = { 0 };
    hdr[2] = 2;                                   /* uncompressed true colour */
    hdr[12] = (uint8_t)ow; hdr[13] = (uint8_t)(ow >> 8);
    hdr[14] = (uint8_t)oh; hdr[15] = (uint8_t)(oh >> 8);
    hdr[16] = 32; hdr[17] = 0x28;                 /* 32 bpp, 8 alpha bits, top-left origin */
    if (!made_dir) { made_dir = 1; mkdir(DATA_PATH "texdump", 0777); }
    char path[160];
    snprintf(path, sizeof(path), DATA_PATH "texdump/tex%d_%dx%d_%s_%d.tga", (int)id, w, h, tag, n);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(hdr, 1, 18, f); fwrite(img, 4, (size_t)ow * oh, f); fclose(f); n++; }
    free(img);
}

/* DM_TEST_TRACE_TEXTURE: one texture number followed through the session - every upload to it (with a full-size image
 * of each level 0, first 40), and when the engine creates or deletes it, so a texture number that is reused for other
 * content while something still draws with it shows up. */
static void trace_full(GLint id, const uint8_t *rgba, int w, int h, const char *tag) {
    static int n;
    if (n >= 40 || w > 2048 || h > 2048) return;
    uint8_t *img = malloc((size_t)w * h * 4);
    if (!img) return;
    for (int k = 0; k < w * h; k++) { img[4*k] = rgba[4*k+2]; img[4*k+1] = rgba[4*k+1]; img[4*k+2] = rgba[4*k]; img[4*k+3] = rgba[4*k+3]; }
    uint8_t hdr[18] = { 0 };
    hdr[2] = 2;
    hdr[12] = (uint8_t)w; hdr[13] = (uint8_t)(w >> 8);
    hdr[14] = (uint8_t)h; hdr[15] = (uint8_t)(h >> 8);
    hdr[16] = 32; hdr[17] = 0x28;
    mkdir(DATA_PATH "texdump", 0777);
    char path[160];
    snprintf(path, sizeof(path), DATA_PATH "texdump/trace%d_%dx%d_%s_%d.tga", (int)id, w, h, tag, n);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(hdr, 1, 18, f); fwrite(img, 4, (size_t)w * h, f); fclose(f); n++; }
    free(img);
    l_info("[trace] texture %d: full-size level 0 saved as %s", (int)id, path);
}

void glGenTextures_soloader(GLsizei n, GLuint *ids) {
    glGenTextures(n, ids);
    if (trace_tex >= 0)
        for (GLsizei i = 0; i < n; i++)
            if ((int)ids[i] == trace_tex) l_info("[trace] texture %d created (glGenTextures)", trace_tex);
}

void glDeleteTextures_soloader(GLsizei n, const GLuint *ids) {
    if (trace_tex >= 0)
        for (GLsizei i = 0; i < n; i++)
            if ((int)ids[i] == trace_tex) l_info("[trace] texture %d deleted (glDeleteTextures)", trace_tex);
    glDeleteTextures(n, ids);
}

static void trace_upload(int compressed, GLint level, int w, int h, unsigned fmt, int bytes, const void *data) {
    if (trace_tex < 0 || current_texture() != trace_tex) return;
    static int n;
    uint32_t sum = 0;
    if (data) for (int k = 0; k < bytes && k < 4096; k++) sum = sum * 31 + ((const uint8_t *)data)[k];
    if (n++ < 400)
        l_info("[trace] texture %d: %s level %d %dx%d format 0x%x, %d bytes%s (checksum of first 4 KB %08x)", trace_tex,
               compressed ? "glCompressedTexImage2D" : "glTexImage2D", level, w, h, fmt, bytes, data ? "" : " WITHOUT data",
               (unsigned)sum);
}

static void dump_texture(GLint id, const void *data, int w, int h, int bpp) {
    uint8_t *rgba = pvrtc_decode(data, w, h, bpp);
    if (!rgba) return;
    write_preview(id, rgba, w, h, bpp == 2 ? "pvrtc2" : "pvrtc4");
    free(rgba);
}

/* Expected size of one PVRTC 1 level (the format has a minimum of 8x8 pixels for 4 bpp, 16x8 for 2 bpp). */
static int pvrtc_size(uint32_t f, int w, int h) {
    int bpp = (f == 0x8C01 || f == 0x8C03) ? 2 : 4;
    int mw = bpp == 2 ? 16 : 8;
    return (w < mw ? mw : w) * (h < 8 ? 8 : h) * bpp / 8;
}

static void min_filter_no_mips(void) {
    /* applies to the texture just uploaded (bound on the active unit) */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

volatile uint32_t dm_tex_n, dm_tex_us, dm_tex_bytes;   /* reported with the [gpu] line */

void glCompressedTexImage2D_soloader(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLint border, GLsizei imageSize, const void *data) {
    ensure_config();
    note_texture(level, width, height, internalformat, 1);
    const char *pn = pvrtc_name(internalformat);
    if (pn) {
        static int logged, odd;
        int exp = pvrtc_size(internalformat, width, height);
        if (level == 0 && logged < 150) {
            logged++;
            l_debug("[tex] texture %d: %s %dx%d%s", current_texture(), pn, width, height,
                    width != height ? " (not square)" : "");
        }
        if (imageSize != exp && odd++ < 20)
            l_warn("[tex] texture %d level %d (%dx%d %s): %d bytes, expected %d", current_texture(), level, width, height,
                   pn, imageSize, exp);
        if (!data) {
            static int empty;
            if (empty++ < 40)
                l_warn("[tex] texture %d level %d (%dx%d %s) uploaded WITHOUT data", current_texture(), level, width, height, pn);
        }
        if (dump_textures && data && level == 0 && imageSize >= exp)
            dump_texture(current_texture(), data, width, height, (internalformat == 0x8C01 || internalformat == 0x8C03) ? 2 : 4);
        if (trace_tex >= 0 && current_texture() == trace_tex) {
            trace_upload(1, level, width, height, internalformat, imageSize, data);
            if (data && level == 0 && imageSize >= exp) {
                uint8_t *rgba = pvrtc_decode(data, width, height, (internalformat == 0x8C01 || internalformat == 0x8C03) ? 2 : 4);
                if (rgba) { trace_full(trace_tex, rgba, width, height, (internalformat == 0x8C01 || internalformat == 0x8C03) ? "pvrtc2" : "pvrtc4"); free(rgba); }
            }
        }
        if (pvrtc_software && data && target == GL_TEXTURE_2D && imageSize >= exp) {   /* never read past the data */
            uint8_t *rgba = pvrtc_decode(data, width, height, (internalformat == 0x8C01 || internalformat == 0x8C03) ? 2 : 4);
            if (rgba) {
                glTexImage2D(target, level, GL_RGBA, width, height, border, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                free(rgba);
                if (no_mipmaps && level == 0) min_filter_no_mips();
                return;
            }
            l_warn("[tex] PVRTC decode failed (%dx%d), uploading compressed", width, height);
        }
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
    __sync_fetch_and_add(&dm_tex_us, (uint32_t)(sceKernelGetProcessTimeWide() - t0));
    __sync_fetch_and_add(&dm_tex_n, 1);
    __sync_fetch_and_add(&dm_tex_bytes, (uint32_t)imageSize);
    if (no_mipmaps && level == 0) min_filter_no_mips();
}

void glTexParameteri_soloader(GLenum target, GLenum pname, GLint param) {
    ensure_config();
    if (no_mipmaps && pname == GL_TEXTURE_MIN_FILTER) {
        if (param == GL_NEAREST_MIPMAP_NEAREST || param == GL_NEAREST_MIPMAP_LINEAR) param = GL_NEAREST;
        else if (param == GL_LINEAR_MIPMAP_NEAREST || param == GL_LINEAR_MIPMAP_LINEAR) param = GL_LINEAR;
    }
    glTexParameteri(target, pname, param);
}

/* ---- graphics profile ----
 * r26-r28 reported "samsung" to InitModel, yet the settings stayed at the low profile ([SystemSettingsAndroid]):
 * r28's dump of GSystemSettings showed every mobile feature flag off. The engine picks the section in
 * appGetMobileSystemSettingsSectionName() from IsSupportedSamsungDevice() and friends, which cache their answer the
 * first time they are asked. That function is replaced here so it returns DM_SETTINGS_SECTION from config.txt
 * (default SystemSettingsAndroid_Samsung; "engine" leaves the engine's own choice). */
#include <so_util/so_util.h>
extern so_module so_mod;

static uint32_t section_w[64];   /* the engine's wchar_t is 4 bytes */

static const uint32_t *my_section_name(void) {
    static int said;
    if (!said++) l_info("[gl] engine asked for its settings section: answering %s", settings_section);
    return section_w;
}

void gldiag_install_hooks(void) {
    ensure_config();
    if (!strcmp(settings_section, "engine")) {
        l_info("[gl] DM_SETTINGS_SECTION engine: the engine chooses its graphics profile");
        return;
    }
    uintptr_t f = (uintptr_t)so_symbol(&so_mod, "_Z37appGetMobileSystemSettingsSectionNamev");
    if (!f) {
        l_warn("[gl] appGetMobileSystemSettingsSectionName not found: graphics profile left to the engine");
        return;
    }
    size_t i = 0;
    for (; settings_section[i] && i < 63; i++) section_w[i] = (unsigned char)settings_section[i];
    section_w[i] = 0;
    hook_addr(f, (uintptr_t)my_section_name);
    l_info("[gl] graphics profile: [%s] (DM_SETTINGS_SECTION)", settings_section);
}


volatile uint32_t dm_vram_free, dm_vglram_free, dm_vram_min, dm_vglram_min;   /* read by the watchdog's [mem] line */

void gldiag_frame(void) {
    static unsigned memtick;
    if (memtick++ % 30 == 0) {   /* vitaGL pool statistics, sampled on the rendering thread */
        uint32_t v = (uint32_t)vglMemFree(VGL_MEM_VRAM), r = (uint32_t)vglMemFree(VGL_MEM_RAM);
        dm_vram_free = v;
        dm_vglram_free = r;
        if (!dm_vram_min || v < dm_vram_min) dm_vram_min = v;
        if (!dm_vglram_min || r < dm_vglram_min) dm_vglram_min = r;
    }
}

/* ---- decal depth bias ----
 * Decals (the rubble piles, stains) are drawn over the floor with glPolygonOffset(slope, units). The engine
 * (FES2RHI::SetRasterizerStateImmediate) passes small fractions: units = DepthBias * 65535, slope often below 1.
 * vitaGL hands both to sceGxmSetFrontDepthBias as ints, so -0.6 becomes 0 and the decal fights the floor (the
 * flickering edges). Here both are scaled by DM_DECAL_BIAS_SCALE (default 6, the best value found on the console;
 * 0 = pass through unchanged) and any non-zero value is rounded away from zero, so a requested offset never
 * disappears. The engine asks for no slope term (r30 log: glPolygonOffset(0, -3.93)), and a constant offset alone
 * cannot cover a floor seen at a grazing angle, so when units are set and the slope is 0 a slope of DM_DECAL_SLOPE
 * (default 1, same direction as units; 0 = none) is added. */
static float bias_round(float v) {
    if (v == 0.0f) return 0.0f;
    v *= bias_scale;
    float r = v < 0.0f ? -(float)(int)(-v + 0.999f) : (float)(int)(v + 0.999f);
    return r;
}

void glPolygonOffset_soloader(GLfloat factor, GLfloat units) {
    ensure_config();
    static int n;
    static float lf = 1e30f, lu = 1e30f;
    float f2 = bias_scale > 0.0f ? bias_round(factor) : factor;
    float u2 = bias_scale > 0.0f ? bias_round(units) : units;
    if (factor == 0.0f && units != 0.0f && decal_slope > 0.0f) {
        float sl = (float)(int)(decal_slope + 0.5f);
        if (sl < 1.0f) sl = 1.0f;
        f2 = units < 0.0f ? -sl : sl;
    }
    if ((factor != lf || units != lu) && n < 24) {
        n++;
        l_info("[gl] glPolygonOffset(%g, %g) -> (%g, %g)", (double)factor, (double)units, (double)f2, (double)u2);
        lf = factor; lu = units;
    }
    glPolygonOffset(f2, u2);
}

/* ---- offscreen rendering check (for DM_MSAA) ----
 * vitaGL's MSAA only covers drawing to the screen; anything the engine renders into its own framebuffers first is not
 * smoothed. Log the first framebuffer attachments, and count binds of non-screen framebuffers per [cpu] period. */
volatile uint32_t dm_fbo_binds;

void glBindFramebuffer_soloader(GLenum target, GLuint fb) {
    if (fb) __sync_fetch_and_add(&dm_fbo_binds, 1);
    glBindFramebuffer(target, fb);
}

void glFramebufferTexture2D_soloader(GLenum target, GLenum attachment, GLenum textarget, GLuint tex, GLint level) {
    static int n;
    if (n++ < 12) {
        const texinfo_t *t = &texinfo[tex % 8192];
        l_info("[fbo] engine renders into texture %u (%ux%u) as attachment 0x%x", tex, t->w, t->h, attachment);
    }
    glFramebufferTexture2D(target, attachment, textarget, tex, level);
}

void glFramebufferRenderbuffer_soloader(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb) {
    static int n;
    if (n++ < 12) l_info("[fbo] renderbuffer %u attached as 0x%x", rb, attachment);
    glFramebufferRenderbuffer(target, attachment, rbtarget, rb);
}