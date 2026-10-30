/*
 * EGL. On Android the Java side creates the GL context; the engine only imports eglGetDisplay (vitaGL's) and
 * eglInitialize. vitaGL has exactly one context, started once (gl_init).
 */
#include "android/egl.h"

#include "gfx/glutil.h"

EGLBoolean eglInitialize_soloader(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    (void)dpy;
    gl_init();
    if (major) *major = 2;
    if (minor) *minor = 2;
    return EGL_TRUE;
}
