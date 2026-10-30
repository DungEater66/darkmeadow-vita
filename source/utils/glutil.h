/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  glutil.h
 * @brief OpenGL API initializer, related functions.
 */

#ifndef SOLOADER_GLUTIL_H
#define SOLOADER_GLUTIL_H

#include <vitaGL.h>

#ifdef __cplusplus
extern "C" {
#endif

void gl_init();

void gl_preload();

void gl_swap();

/* glGetString that reports only the PVRTC compressed texture format (see glutil.c). */
const GLubyte *glGetString_soloader(GLenum name);

void glCompileShader_soloader(GLuint shader);

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length);

/* glGetAttribLocation / glGetUniformLocation that log names the program lacks (once per name). */
GLint glGetAttribLocation_soloader(GLuint prog, const GLchar *name);
GLint glGetUniformLocation_soloader(GLuint prog, const GLchar *name);
void glLinkProgram_soloader(GLuint prog);
void glUseProgram_soloader(GLuint prog);
void glAttachShader_soloader(GLuint prog, GLuint shader);
void glVertexAttribPointer_soloader(GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr);
void glVertexAttrib4fv_soloader(GLuint index, const GLfloat *v);
void glVertexAttrib4f_soloader(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H
