/*
 * The cursor of the physical controls, drawn over whatever the engine drew, at every buffer swap
 * (JavaCallback_swapBuffers, rendering thread), the way movie.c draws movie frames: every piece of GL state touched is
 * saved and restored, and the draw uses its own vertex array object.
 */
#include "input/padinput.h"
#include "video/movie.h"
#include "util/logger.h"

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <vitaGL.h>

#define CUR_SIZE 32

static GLuint prog, tex, vao;
static int ready, failed;

static const char *vsh =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
static const char *fsh =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "void main() { gl_FragColor = texture2D(uTex, vUV); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    GLint len = (GLint)strlen(src);
    glShaderSource(s, 1, &src, &len);
    glCompileShader(s);
    return s;
}

/* a ring with a dot in the middle: white with a dark edge, so it shows on light and dark screens */
static void make_cursor(uint8_t *px) {
    const float c = (CUR_SIZE - 1) * 0.5f;
    for (int y = 0; y < CUR_SIZE; y++)
        for (int x = 0; x < CUR_SIZE; x++) {
            float d = sqrtf((x - c) * (x - c) + (y - c) * (y - c));
            uint8_t v = 0, a = 0;
            if (fabsf(d - 11.f) < 1.6f || d < 2.2f) { v = 255; a = 255; }                       /* white ring, dot */
            else if (fabsf(d - 11.f) < 3.0f || d < 3.6f) { v = 0; a = 200; }                    /* dark edge */
            uint8_t *p = px + (y * CUR_SIZE + x) * 4;
            p[0] = p[1] = p[2] = v;
            p[3] = a;
        }
}

static int setup(void) {
    if (ready) return 1;
    if (failed) return 0;
    GLuint v = compile(GL_VERTEX_SHADER, vsh), f = compile(GL_FRAGMENT_SHADER, fsh);
    prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aUV");
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { l_warn("[pad] cursor shader did not link: no cursor"); failed = 1; return 0; }
    GLint cur = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &cur);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
    glUseProgram((GLuint)cur);

    static uint8_t px[CUR_SIZE * CUR_SIZE * 4];
    make_cursor(px);
    GLint act = 0, bound = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, CUR_SIZE, CUR_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
    glActiveTexture((GLenum)act);
    glGenVertexArrays(1, &vao);
    ready = 1;
    return 1;
}

void overlay_render(void) {
    if ((padinput_profile != 0 && padinput_profile != 2 && padinput_profile != 3) || movie_playing() || !setup())
        return;   /* not in sword fights, cutscenes or movies */
    float cx = padinput_cursor_x, cy = padinput_cursor_y, h = CUR_SIZE * 0.5f;
    /* screen pixels -> clip space (y down -> up) */
    float x0 = (cx - h) / 480.f - 1.f, x1 = (cx + h) / 480.f - 1.f;
    float y0 = 1.f - (cy - h) / 272.f, y1 = 1.f - (cy + h) / 272.f;
    float quad[16] = { x0, y0, 0, 0,   x1, y0, 1, 0,   x0, y1, 0, 1,   x1, y1, 1, 1 };

    GLint cur_prog, act, bound, abuf, fb, vp[4], dmask, bsrc, bdst, basrc, badst;
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
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &bdst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &basrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &badst);
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);

    if (fb) glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 960, 544);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST); glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(prog);
    glBindTexture(GL_TEXTURE_2D, tex);
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
    glBlendFuncSeparate((GLenum)bsrc, (GLenum)bdst, (GLenum)basrc, (GLenum)badst);
    if (!blend) glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (stencil) glEnable(GL_STENCIL_TEST);
    if (poff) glEnable(GL_POLYGON_OFFSET_FILL);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (fb) glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
}
