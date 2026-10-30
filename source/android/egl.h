/* The two EGL functions the engine imports (see egl.c). */
#ifndef DM_EGL_H
#define DM_EGL_H

#include <vitaGL.h>

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor);

#endif
