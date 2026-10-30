/*
 * vitaGL start-up and the OpenGL ES calls the loader handles before vitaGL sees them: shaders and the extension
 * string. The fixes for the game's own drawing are in gl_fixes.h.
 */
#ifndef DM_GLUTIL_H
#define DM_GLUTIL_H

#include <vitaGL.h>

#ifdef __cplusplus
extern "C" {
#endif

void gl_preload(void);   /* before the engine starts: checks libshacccg.suprx is installed */
void gl_init(void);      /* when the engine creates its GL context */
void gl_swap(void);

/* Hand a shader's final source to vitaGL (it compiles at link time, so this can replace a source before linking). */
void gl_load_shader(GLuint shader, const char *src, size_t len);

/* Statistics for the [gpu] log line (util/watchdog.c): programs linked, and time spent linking. */
extern volatile uint32_t dm_link_n, dm_link_us, dm_link_max_us;

/* Replacements handed to the engine (android/dynlib.c). */
const GLubyte *glGetString_soloader(GLenum name);
void glShaderSource_soloader(GLuint shader, GLsizei count, const GLchar **string, const GLint *length);
void glCompileShader_soloader(GLuint shader);
void glAttachShader_soloader(GLuint prog, GLuint shader);
void glLinkProgram_soloader(GLuint prog);

#ifdef __cplusplus
}
#endif

#endif
