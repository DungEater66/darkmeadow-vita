/*
 * Fixes for how the game draws on the Vita (see gl_fixes.c).
 */
#ifndef DM_GL_FIXES_H
#define DM_GL_FIXES_H

#include <stddef.h>
#include <stdint.h>
#include <vitaGL.h>

/* Edit a shader's source before it is compiled. Returns `src`, or a new malloc'd string (then `src` was freed);
 * `*len` follows. */
char *gl_fixes_shader_source(GLuint shader, char *src, size_t *len);
/* The engine attaches a program's two shaders one after the other: pairs them for the depth pre-pass fix. */
void gl_fixes_attach(GLuint prog, GLuint shader);

/* From so_patch(): answer the engine's graphics-profile question. */
void gl_fixes_install_hooks(void);
/* Once per frame on the rendering thread: samples vitaGL's free memory for the [mem] log line. */
void gl_fixes_frame(void);

/* Statistics for the watchdog's [gpu] and [mem] lines. */
extern volatile uint32_t dm_tex_n, dm_tex_us, dm_tex_bytes;   /* compressed texture uploads */
extern volatile uint32_t dm_fbo_binds;                        /* binds of the engine's own framebuffers */
extern volatile uint32_t dm_vram_free, dm_vglram_free, dm_vram_min, dm_vglram_min;

/* Replacements handed to the engine (android/dynlib.c). */
void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count);
void glPixelStorei_soloader(GLenum pname, GLint param);
void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                           GLint border, GLenum format, GLenum type, const void *pixels);
void glCompressedTexImage2D_soloader(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLint border, GLsizei imageSize, const void *data);
void glPolygonOffset_soloader(GLfloat factor, GLfloat units);
void glBindFramebuffer_soloader(GLenum target, GLuint fb);

#endif
