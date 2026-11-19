#pragma once
// Compile-only declarations; the Android NDK supplies the actual EGL header.
using EGLDisplay = void *;
using EGLSurface = void *;
using EGLContext = void *;
using EGLint = int;
using EGLBoolean = unsigned int;
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_TRUE 1
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_DRAW 0x3059
extern "C" EGLContext eglGetCurrentContext();
extern "C" EGLDisplay eglGetCurrentDisplay();
extern "C" EGLSurface eglGetCurrentSurface(EGLint);
extern "C" EGLBoolean eglQuerySurface(EGLDisplay, EGLSurface, EGLint, EGLint *);
