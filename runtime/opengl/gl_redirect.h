// andropkg: force-included into the framework's GL JNI (android.opengl.*, gles_jni.*).
// EGL is ours (egl.m, bionic_egl*); the GL entry points are the platform's, except the ones egl.m
// wraps: framebuffer 0 is the current surface's FBO, and on macOS the GLES 2.0 stand-in
// (OpenGL 2.1) needs its emulated entry points. (The AAPCS64 stack-slot wrappers are for
// Android callers only and are not redirected.)
#pragma once
#include <TargetConditionals.h>

#define eglBindAPI bionic_eglBindAPI
#define eglBindTexImage bionic_eglBindTexImage
#define eglChooseConfig bionic_eglChooseConfig
#define eglCopyBuffers bionic_eglCopyBuffers
#define eglCreateContext bionic_eglCreateContext
#define eglCreatePbufferFromClientBuffer bionic_eglCreatePbufferFromClientBuffer
#define eglCreatePbufferSurface bionic_eglCreatePbufferSurface
#define eglCreatePixmapSurface bionic_eglCreatePixmapSurface
#define eglCreateWindowSurface bionic_eglCreateWindowSurface
#define eglDestroyContext bionic_eglDestroyContext
#define eglDestroySurface bionic_eglDestroySurface
#define eglDupNativeFenceFDANDROID bionic_eglDupNativeFenceFDANDROID
#define eglGetConfigAttrib bionic_eglGetConfigAttrib
#define eglGetConfigs bionic_eglGetConfigs
#define eglGetCurrentContext bionic_eglGetCurrentContext
#define eglGetCurrentDisplay bionic_eglGetCurrentDisplay
#define eglGetCurrentSurface bionic_eglGetCurrentSurface
#define eglGetDisplay bionic_eglGetDisplay
#define eglGetError bionic_eglGetError
#define eglGetProcAddress bionic_eglGetProcAddress
#define eglInitialize bionic_eglInitialize
#define eglMakeCurrent bionic_eglMakeCurrent
#define eglPresentationTimeANDROID bionic_eglPresentationTimeANDROID
#define eglQueryAPI bionic_eglQueryAPI
#define eglQueryContext bionic_eglQueryContext
#define eglQueryString bionic_eglQueryString
#define eglQuerySurface bionic_eglQuerySurface
#define eglReleaseTexImage bionic_eglReleaseTexImage
#define eglReleaseThread bionic_eglReleaseThread
#define eglSurfaceAttrib bionic_eglSurfaceAttrib
#define eglSwapBuffers bionic_eglSwapBuffers
#define eglSwapInterval bionic_eglSwapInterval
#define eglTerminate bionic_eglTerminate
#define eglWaitClient bionic_eglWaitClient
#define eglWaitGL bionic_eglWaitGL
#define eglWaitNative bionic_eglWaitNative

#define glBindFramebuffer bionic_glBindFramebuffer
#if TARGET_OS_IPHONE
#define glInvalidateFramebuffer bionic_glInvalidateFramebuffer
#define glDiscardFramebufferEXT bionic_glDiscardFramebufferEXT
#else
#define glGetString bionic_glGetString
#define glGetIntegerv bionic_glGetIntegerv
#define glClearDepthf bionic_glClearDepthf
#define glDepthRangef bionic_glDepthRangef
#define glReleaseShaderCompiler bionic_glReleaseShaderCompiler
#define glShaderBinary bionic_glShaderBinary
#define glGetShaderPrecisionFormat bionic_glGetShaderPrecisionFormat
#define glShaderSource bionic_glShaderSource
#define glTexImage2D bionic_glTexImage2D
#endif
