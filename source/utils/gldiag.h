/*
 * Shader switches from config.txt, and diagnostics for the props that render black (see gldiag.c).
 */
#ifndef DM_GLDIAG_H
#define DM_GLDIAG_H

#include <stddef.h>
#include <vitaGL.h>

/* Apply the config.txt shader switches to a shader's source and note its material features. Returns the source to
 * compile: `src` itself, or a new malloc'd string (in which case `src` has been freed). `*len` is updated. */
char *gldiag_shader_source(GLuint shader, char *src, size_t *len);
void gldiag_attach(GLuint prog, GLuint shader);
void gldiag_uniform_location(GLuint prog, const char *name, GLint loc);
void gldiag_use_program(GLuint prog);

void glUniform1i_soloader(GLint loc, GLint v);
void glUniform3fv_soloader(GLint loc, GLsizei count, const GLfloat *v);
void glUniform4fv_soloader(GLint loc, GLsizei count, const GLfloat *v);
void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type, const void *indices);
void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count);
void glPixelStorei_soloader(GLenum pname, GLint param);
void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                           GLint border, GLenum format, GLenum type, const void *pixels);
void glCompressedTexImage2D_soloader(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLint border, GLsizei imageSize, const void *data);

/* Called from so_patch(): replaces the engine's graphics-profile choice (DM_SETTINGS_SECTION). */
void gldiag_install_hooks(void);

/* Once per frame, on the render thread (buffer swap). */
void gldiag_frame(void);

void glPolygonOffset_soloader(GLfloat factor, GLfloat units);
void glBindFramebuffer_soloader(GLenum target, GLuint fb);
void glFramebufferTexture2D_soloader(GLenum target, GLenum attachment, GLenum textarget, GLuint tex, GLint level);
void glFramebufferRenderbuffer_soloader(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb);
void glTexParameteri_soloader(GLenum target, GLenum pname, GLint param);

void glGenTextures_soloader(GLsizei n, GLuint *ids);
void glDeleteTextures_soloader(GLsizei n, const GLuint *ids);

#endif
