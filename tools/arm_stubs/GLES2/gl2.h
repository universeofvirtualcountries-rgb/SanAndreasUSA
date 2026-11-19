#pragma once
// Compile-only declarations; the Android NDK supplies the actual GLES header.
using GLenum = unsigned int;
using GLint = int;
#define GL_VIEWPORT 0x0BA2
extern "C" void glGetIntegerv(GLenum, GLint *);
