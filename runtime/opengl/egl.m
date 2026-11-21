// andropkg — EGL 1.4 and the GLES libraries (libEGL, libGLESv1_CM, libGLESv2/v3), for NDK code
// (libandropkg_ndk resolves libEGL/libGLES* imports here: andropkg_gl_symbol) and for the
// framework's GL JNI (gl_redirect.h).
//
// iOS: Apple's OpenGL ES (EAGL contexts; ES 1.1, 2.0 and 3.0). The gl* entry points are Apple's
// own: GLES is a plain C API without variadics, so Android code calls them directly (resolved
// by name, ndk_gl_proc) except where the calling conventions differ (below).
// macOS (development loop): OpenGL 2.1 (legacy CGL profile) stands in for GLES 2.0 — GLSL ES
// 1.00 shaders are rewritten to GLSL 1.20, the few ES-only entry points are emulated.
//
// A window surface is an offscreen framebuffer (color + depth/stencil renderbuffers): the app's
// framebuffer 0 is mapped onto it (glBindFramebuffer), and eglSwapBuffers reads it back into the
// ANativeWindow (window_jni.cc), which posts the frame like any other window content.
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EGL_EGLEXT_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>

#if TARGET_OS_IPHONE
#define GLES_SILENCE_DEPRECATION 1
#import <OpenGLES/EAGL.h>
#include <OpenGLES/ES2/glext.h>
#include <OpenGLES/ES3/gl.h>
#define GL_FRAMEWORK "/System/Library/Frameworks/OpenGLES.framework/OpenGLES"
#else
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#define GL_FRAMEWORK "/System/Library/Frameworks/OpenGL.framework/OpenGL"
#endif

#include <android/log.h>

#define NDK_EXPORT __attribute__((visibility("default")))
#define NDK_LOG_INFO ANDROID_LOG_INFO
#define NDK_LOG_WARN ANDROID_LOG_WARN
#define NDK_LOG_ERROR ANDROID_LOG_ERROR
#define ndk_log(prio, ...) __android_log_print(prio, "andropkg-gl", __VA_ARGS__)

#ifndef GL_RGBA8_OES
#define GL_RGBA8_OES 0x8058
#define GL_RGB8_OES 0x8051
#endif
#ifndef GL_DEPTH24_STENCIL8_OES
#define GL_DEPTH24_STENCIL8_OES 0x88F0
#endif

// ---- the ANativeWindow side (window_jni.cc, in libhwui) -------------------------------------
typedef struct {
    int32_t width, height, stride, format;
    void *bits;
    uint32_t reserved[6];
} NdkWindowBuffer;
static struct {
    int32_t (*getWidth)(void *), (*getHeight)(void *);
    int32_t (*lock)(void *, NdkWindowBuffer *, void *), (*unlockAndPost)(void *);
    int32_t (*setGeometry)(void *, int32_t, int32_t, int32_t);
} W;
static void *g_gl;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void init_once(void) {
    W.getWidth = dlsym(RTLD_DEFAULT, "ANativeWindow_getWidth");
    W.getHeight = dlsym(RTLD_DEFAULT, "ANativeWindow_getHeight");
    W.lock = dlsym(RTLD_DEFAULT, "ANativeWindow_lock");
    W.unlockAndPost = dlsym(RTLD_DEFAULT, "ANativeWindow_unlockAndPost");
    W.setGeometry = dlsym(RTLD_DEFAULT, "ANativeWindow_setBuffersGeometry");
    g_gl = dlopen(GL_FRAMEWORK, RTLD_NOW | RTLD_GLOBAL);
    if (!g_gl) ndk_log(NDK_LOG_ERROR, "GLES: %s", dlerror());
}

/// A GL entry point of the platform (by its GLES name), for symbols of libGLES*.
NDK_EXPORT void *ndk_gl_proc(const char *name) {
    pthread_once(&g_once, init_once);
    return g_gl ? dlsym(g_gl, name) : NULL;
}

/// This library's implementation of an NDK symbol: bionic_<name> (EGL, GL wrappers), or NULL.
static void *g_self;
static void find_self(void) {
    Dl_info d;
    if (dladdr((void *)&ndk_gl_proc, &d)) g_self = dlopen(d.dli_fname, RTLD_NOW | RTLD_NOLOAD);
}
NDK_EXPORT void *andropkg_gl_symbol(const char *name) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, find_self);
    char buf[256];
    if (!g_self || strlen(name) + 8 >= sizeof buf) return NULL;
    snprintf(buf, sizeof buf, "bionic_%s", name);
    return dlsym(g_self, buf);
}

// ---- objects ------------------------------------------------------------------------------------
typedef struct {
    EGLint id, r, g, b, a, depth, stencil, samples, renderable;
} Config;

#if TARGET_OS_IPHONE
#define RENDERABLE (EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT_KHR)
#else
#define RENDERABLE EGL_OPENGL_ES2_BIT
#endif
static const Config kConfigs[] = {
    {1, 8, 8, 8, 8, 24, 8, 0, RENDERABLE}, {2, 8, 8, 8, 8, 0, 0, 0, RENDERABLE},
    {3, 8, 8, 8, 8, 16, 0, 0, RENDERABLE}, {4, 8, 8, 8, 0, 24, 8, 0, RENDERABLE},
    {5, 8, 8, 8, 0, 0, 0, 0, RENDERABLE},  {6, 5, 6, 5, 0, 24, 8, 0, RENDERABLE},
    {7, 5, 6, 5, 0, 16, 0, 0, RENDERABLE}, {8, 5, 6, 5, 0, 0, 0, 0, RENDERABLE},
    // Multisampled configs are offered (apps often insist) but render single-sampled.
    {9, 8, 8, 8, 8, 24, 8, 4, RENDERABLE}, {10, 5, 6, 5, 0, 24, 8, 4, RENDERABLE},
};
#define NCONFIGS ((EGLint)(sizeof kConfigs / sizeof *kConfigs))

typedef struct Context {
    const Config *cfg;
    int version;
#if TARGET_OS_IPHONE
    EAGLContext *eagl;
#else
    CGLContextObj cgl;
#endif
    bool viewport_set;
} Context;

typedef struct Surface {
    const Config *cfg;
    void *window;  // ANativeWindow*, or NULL for a pbuffer
    int w, h;
    Context *fbo_ctx;  // the context the framebuffer objects belong to (FBOs aren't shared)
    GLuint fbo, color, depth;
    int interval;
    uint64_t next_frame_ns;
    uint8_t *readback;
    size_t readback_size;
} Surface;

static __thread EGLint t_error = EGL_SUCCESS;
static __thread Context *t_ctx;
static __thread Surface *t_draw, *t_read;
static __thread EGLenum t_api = EGL_OPENGL_ES_API;

#define DPY ((EGLDisplay)(uintptr_t)1)
static EGLBoolean fail(EGLint e) { t_error = e; return EGL_FALSE; }
static bool dpy_ok(EGLDisplay d) {
    if (d == DPY) return true;
    t_error = EGL_BAD_DISPLAY;
    return false;
}
static const Config *cfg_of(EGLConfig c) {
    intptr_t i = (intptr_t)c;
    return i >= 1 && i <= NCONFIGS ? &kConfigs[i - 1] : NULL;
}

// ---- display, configs -----------------------------------------------------------------------------
NDK_EXPORT EGLint bionic_eglGetError(void) {
    EGLint e = t_error;
    t_error = EGL_SUCCESS;
    return e;
}
NDK_EXPORT EGLDisplay bionic_eglGetDisplay(EGLNativeDisplayType d) { return DPY; }
NDK_EXPORT EGLDisplay bionic_eglGetPlatformDisplay(EGLenum p, void *d, const EGLAttrib *a) { return DPY; }
NDK_EXPORT EGLBoolean bionic_eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor) {
    if (!dpy_ok(d)) return EGL_FALSE;
    pthread_once(&g_once, init_once);
    if (!g_gl || !W.lock) return fail(EGL_NOT_INITIALIZED);
    if (major) *major = 1;
    if (minor) *minor = 4;
    return EGL_TRUE;
}
NDK_EXPORT EGLBoolean bionic_eglTerminate(EGLDisplay d) { return dpy_ok(d); }
NDK_EXPORT const char *bionic_eglQueryString(EGLDisplay d, EGLint name) {
    switch (name) {
    case EGL_VENDOR: return "andropkg";
    case EGL_VERSION: return "1.4 andropkg";
    case EGL_CLIENT_APIS: return "OpenGL_ES";
    case EGL_EXTENSIONS:
        return "EGL_KHR_create_context EGL_KHR_get_all_proc_addresses EGL_KHR_surfaceless_context "
               "EGL_ANDROID_presentation_time EGL_KHR_no_config_context";
    }
    t_error = EGL_BAD_PARAMETER;
    return NULL;
}

static EGLint config_attr(const Config *c, EGLint attr) {
    switch (attr) {
    case EGL_CONFIG_ID: return c->id;
    case EGL_RED_SIZE: return c->r;
    case EGL_GREEN_SIZE: return c->g;
    case EGL_BLUE_SIZE: return c->b;
    case EGL_ALPHA_SIZE: return c->a;
    case EGL_BUFFER_SIZE: return c->r + c->g + c->b + c->a;
    case EGL_DEPTH_SIZE: return c->depth;
    case EGL_STENCIL_SIZE: return c->stencil;
    case EGL_SAMPLES: return c->samples;
    case EGL_SAMPLE_BUFFERS: return c->samples ? 1 : 0;
    case EGL_RENDERABLE_TYPE: case EGL_CONFORMANT: return c->renderable;
    case EGL_SURFACE_TYPE: return EGL_WINDOW_BIT | EGL_PBUFFER_BIT | EGL_SWAP_BEHAVIOR_PRESERVED_BIT;
    case EGL_COLOR_BUFFER_TYPE: return EGL_RGB_BUFFER;
    case EGL_CONFIG_CAVEAT: return EGL_NONE;
    case EGL_NATIVE_RENDERABLE: return EGL_TRUE;
    case EGL_NATIVE_VISUAL_ID: return c->a ? 1 : 2;  // WINDOW_FORMAT_RGBA_8888 / RGBX_8888
    case EGL_NATIVE_VISUAL_TYPE: return 0;
    case EGL_MAX_PBUFFER_WIDTH: case EGL_MAX_PBUFFER_HEIGHT: return 4096;
    case EGL_MAX_PBUFFER_PIXELS: return 4096 * 4096;
    case EGL_MIN_SWAP_INTERVAL: return 0;
    case EGL_MAX_SWAP_INTERVAL: return 4;
    case EGL_BIND_TO_TEXTURE_RGB: case EGL_BIND_TO_TEXTURE_RGBA: return EGL_FALSE;
    case EGL_LUMINANCE_SIZE: case EGL_ALPHA_MASK_SIZE: case EGL_LEVEL: return 0;
    case EGL_TRANSPARENT_TYPE: return EGL_NONE;
    case 0x3142 /* EGL_RECORDABLE_ANDROID */: case 0x3147 /* EGL_FRAMEBUFFER_TARGET_ANDROID */: return EGL_TRUE;
    }
    return -1;
}

NDK_EXPORT EGLBoolean bionic_eglGetConfigs(EGLDisplay d, EGLConfig *configs, EGLint size, EGLint *num) {
    if (!dpy_ok(d)) return EGL_FALSE;
    if (!num) return fail(EGL_BAD_PARAMETER);
    EGLint n = 0;
    for (EGLint i = 0; i < NCONFIGS && (!configs || n < size); i++) {
        if (configs) configs[n] = (EGLConfig)(intptr_t)kConfigs[i].id;
        n++;
    }
    *num = n;
    return EGL_TRUE;
}

NDK_EXPORT EGLBoolean bionic_eglGetConfigAttrib(EGLDisplay d, EGLConfig config, EGLint attr, EGLint *value) {
    if (!dpy_ok(d)) return EGL_FALSE;
    const Config *c = cfg_of(config);
    if (!c) return fail(EGL_BAD_CONFIG);
    EGLint v = config_attr(c, attr);
    if (v == -1) return fail(EGL_BAD_ATTRIBUTE);
    if (value) *value = v;
    return EGL_TRUE;
}

static const EGLint *g_sort_attrs;
static int requested_bits(const Config *c) {
    int sum = 0;
    for (const EGLint *a = g_sort_attrs; a && a[0] != EGL_NONE; a += 2) {
        if (a[1] == 0 || a[1] == EGL_DONT_CARE) continue;
        if (a[0] == EGL_RED_SIZE) sum += c->r;
        if (a[0] == EGL_GREEN_SIZE) sum += c->g;
        if (a[0] == EGL_BLUE_SIZE) sum += c->b;
        if (a[0] == EGL_ALPHA_SIZE) sum += c->a;
    }
    return sum;
}
static int cmp_configs(const void *pa, const void *pb) {  // EGL 1.4 §3.4.1.2, simplified
    const Config *a = cfg_of(*(EGLConfig *)pa), *b = cfg_of(*(EGLConfig *)pb);
    int d;
    if ((d = requested_bits(b) - requested_bits(a))) return d;
    if ((d = (a->r + a->g + a->b + a->a) - (b->r + b->g + b->b + b->a))) return d;
    if ((d = a->samples - b->samples)) return d;
    if ((d = a->depth - b->depth)) return d;
    if ((d = a->stencil - b->stencil)) return d;
    return a->id - b->id;
}

NDK_EXPORT EGLBoolean bionic_eglChooseConfig(EGLDisplay d, const EGLint *attrs, EGLConfig *configs, EGLint size,
                                             EGLint *num) {
    if (!dpy_ok(d)) return EGL_FALSE;
    if (!num) return fail(EGL_BAD_PARAMETER);
    EGLConfig found[NCONFIGS];
    EGLint n = 0;
    for (EGLint i = 0; i < NCONFIGS; i++) {
        const Config *c = &kConfigs[i];
        bool ok = true;  // (no EGL_RENDERABLE_TYPE: every config, whatever the ES 1.x default says)
        for (const EGLint *a = attrs; ok && a && a[0] != EGL_NONE; a += 2) {
            EGLint k = a[0], v = a[1];
            if (v == EGL_DONT_CARE) continue;
            switch (k) {
            case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_ALPHA_SIZE: case EGL_BUFFER_SIZE:
            case EGL_DEPTH_SIZE: case EGL_STENCIL_SIZE: case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS:
                ok = config_attr(c, k) >= v;
                break;
            case EGL_RENDERABLE_TYPE: case EGL_CONFORMANT: case EGL_SURFACE_TYPE:
                ok = (config_attr(c, k) & v) == v;
                break;
            case EGL_CONFIG_ID: ok = c->id == v; break;
            case EGL_COLOR_BUFFER_TYPE: ok = v == EGL_RGB_BUFFER; break;
            case EGL_CONFIG_CAVEAT: ok = v == EGL_NONE; break;
            case EGL_LUMINANCE_SIZE: case EGL_ALPHA_MASK_SIZE: ok = v == 0; break;
            case EGL_NATIVE_VISUAL_ID: ok = config_attr(c, k) == v; break;
            default: break;  // EGL_RECORDABLE_ANDROID, EGL_NATIVE_RENDERABLE, ... : every config
            }
        }
        if (ok) found[n++] = (EGLConfig)(intptr_t)c->id;
    }
    g_sort_attrs = attrs;
    qsort(found, (size_t)n, sizeof *found, cmp_configs);
    if (configs) {
        if (n > size) n = size;
        memcpy(configs, found, (size_t)n * sizeof *found);
    }
    *num = n;
    return EGL_TRUE;
}

// ---- contexts ---------------------------------------------------------------------------------
NDK_EXPORT EGLBoolean bionic_eglBindAPI(EGLenum api) {
    if (api != EGL_OPENGL_ES_API) return fail(EGL_BAD_PARAMETER);
    t_api = api;
    return EGL_TRUE;
}
NDK_EXPORT EGLenum bionic_eglQueryAPI(void) { return t_api; }

NDK_EXPORT EGLContext bionic_eglCreateContext(EGLDisplay d, EGLConfig config, EGLContext share, const EGLint *attrs) {
    if (!dpy_ok(d)) return EGL_NO_CONTEXT;
    const Config *c = cfg_of(config);
    if (!c && config != EGL_NO_CONFIG_KHR) {
        t_error = EGL_BAD_CONFIG;
        return EGL_NO_CONTEXT;
    }
    int version = 1;
    for (const EGLint *a = attrs; a && a[0] != EGL_NONE; a += 2)
        if (a[0] == EGL_CONTEXT_CLIENT_VERSION /* == EGL_CONTEXT_MAJOR_VERSION_KHR */) version = a[1];
    Context *sh = share == EGL_NO_CONTEXT ? NULL : (Context *)share;
    Context *ctx = calloc(1, sizeof *ctx);
    ctx->cfg = c;
    ctx->version = version;
#if TARGET_OS_IPHONE
    if (version < 1 || version > 3) {
        free(ctx);
        t_error = EGL_BAD_MATCH;
        return EGL_NO_CONTEXT;
    }
    EAGLRenderingAPI api = (EAGLRenderingAPI)version;
    EAGLContext *e = sh ? [[EAGLContext alloc] initWithAPI:api sharegroup:sh->eagl.sharegroup]
                        : [[EAGLContext alloc] initWithAPI:api];
    if (!e) {
        free(ctx);
        t_error = EGL_BAD_MATCH;
        return EGL_NO_CONTEXT;
    }
    ctx->eagl = e;  // (manual retain: this file is built without ARC)
#else
    if (version != 2) {  // the macOS stand-in is GLES 2.0 only
        free(ctx);
        t_error = EGL_BAD_MATCH;
        return EGL_NO_CONTEXT;
    }
    CGLPixelFormatAttribute pfa[] = {kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers, kCGLPFAOpenGLProfile,
                                     (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy, 0};
    CGLPixelFormatObj pf = NULL;
    GLint npf = 0;
    if (CGLChoosePixelFormat(pfa, &pf, &npf) != kCGLNoError || !pf ||
        CGLCreateContext(pf, sh ? sh->cgl : NULL, &ctx->cgl) != kCGLNoError) {
        if (pf) CGLDestroyPixelFormat(pf);
        free(ctx);
        t_error = EGL_BAD_ALLOC;
        return EGL_NO_CONTEXT;
    }
    CGLDestroyPixelFormat(pf);
#endif
    ndk_log(NDK_LOG_INFO, "EGL: context GLES %d (config %d)", version, c ? c->id : 0);
    return (EGLContext)ctx;
}

NDK_EXPORT EGLBoolean bionic_eglDestroyContext(EGLDisplay d, EGLContext context) {
    if (!dpy_ok(d)) return EGL_FALSE;
    Context *ctx = (Context *)context;
    if (!ctx) return fail(EGL_BAD_CONTEXT);
    if (ctx == t_ctx) return EGL_TRUE;  // deleted when no longer current (leaked here: rare)
#if TARGET_OS_IPHONE
    [ctx->eagl release];
#else
    CGLDestroyContext(ctx->cgl);
#endif
    free(ctx);
    return EGL_TRUE;
}

NDK_EXPORT EGLBoolean bionic_eglQueryContext(EGLDisplay d, EGLContext context, EGLint attr, EGLint *value) {
    Context *ctx = (Context *)context;
    if (!dpy_ok(d)) return EGL_FALSE;
    if (!ctx) return fail(EGL_BAD_CONTEXT);
    switch (attr) {
    case EGL_CONFIG_ID: *value = ctx->cfg ? ctx->cfg->id : 0; return EGL_TRUE;
    case EGL_CONTEXT_CLIENT_TYPE: *value = EGL_OPENGL_ES_API; return EGL_TRUE;
    case EGL_CONTEXT_CLIENT_VERSION: *value = ctx->version; return EGL_TRUE;
    case EGL_RENDER_BUFFER: *value = EGL_BACK_BUFFER; return EGL_TRUE;
    }
    return fail(EGL_BAD_ATTRIBUTE);
}

// ---- surfaces ---------------------------------------------------------------------------------
static Surface *new_surface(const Config *c, void *window, int w, int h) {
    Surface *s = calloc(1, sizeof *s);
    s->cfg = c;
    s->window = window;
    s->w = w;
    s->h = h;
    s->interval = 1;
    return s;
}

NDK_EXPORT EGLSurface bionic_eglCreateWindowSurface(EGLDisplay d, EGLConfig config, EGLNativeWindowType win,
                                                    const EGLint *attrs) {
    if (!dpy_ok(d)) return EGL_NO_SURFACE;
    const Config *c = cfg_of(config);
    if (!c) { t_error = EGL_BAD_CONFIG; return EGL_NO_SURFACE; }
    if (!win) { t_error = EGL_BAD_NATIVE_WINDOW; return EGL_NO_SURFACE; }
    // Frames go to the window as RGBA (the app's geometry kept; format per the config).
    W.setGeometry(win, W.getWidth(win), W.getHeight(win), c->a ? 1 : 2);
    Surface *s = new_surface(c, win, W.getWidth(win), W.getHeight(win));
    ndk_log(NDK_LOG_INFO, "EGL: window surface %dx%d (config %d)", s->w, s->h, c->id);
    return (EGLSurface)s;
}
NDK_EXPORT EGLSurface bionic_eglCreatePlatformWindowSurface(EGLDisplay d, EGLConfig config, void *win,
                                                            const EGLAttrib *attrs) {
    return bionic_eglCreateWindowSurface(d, config, win, NULL);
}

NDK_EXPORT EGLSurface bionic_eglCreatePbufferSurface(EGLDisplay d, EGLConfig config, const EGLint *attrs) {
    if (!dpy_ok(d)) return EGL_NO_SURFACE;
    const Config *c = cfg_of(config);
    if (!c) { t_error = EGL_BAD_CONFIG; return EGL_NO_SURFACE; }
    int w = 0, h = 0;
    for (const EGLint *a = attrs; a && a[0] != EGL_NONE; a += 2) {
        if (a[0] == EGL_WIDTH) w = a[1];
        if (a[0] == EGL_HEIGHT) h = a[1];
    }
    return (EGLSurface)new_surface(c, NULL, w, h);
}

static void free_fbo(Surface *s) {
    if (!s->fbo) return;
    if (t_ctx == s->fbo_ctx) {
        glDeleteFramebuffers(1, &s->fbo);
        glDeleteRenderbuffers(1, &s->color);
        if (s->depth) glDeleteRenderbuffers(1, &s->depth);
    }
    s->fbo = s->color = s->depth = 0;
    s->fbo_ctx = NULL;
}

NDK_EXPORT EGLBoolean bionic_eglDestroySurface(EGLDisplay d, EGLSurface surface) {
    if (!dpy_ok(d)) return EGL_FALSE;
    Surface *s = (Surface *)surface;
    if (!s) return fail(EGL_BAD_SURFACE);
    if (s == t_draw || s == t_read) {  // still current: dropped from the window, freed later
        s->window = NULL;
        return EGL_TRUE;
    }
    free_fbo(s);
    free(s->readback);
    free(s);
    return EGL_TRUE;
}

NDK_EXPORT EGLBoolean bionic_eglQuerySurface(EGLDisplay d, EGLSurface surface, EGLint attr, EGLint *value) {
    if (!dpy_ok(d)) return EGL_FALSE;
    Surface *s = (Surface *)surface;
    if (!s) return fail(EGL_BAD_SURFACE);
    switch (attr) {
    case EGL_WIDTH: *value = s->w; return EGL_TRUE;
    case EGL_HEIGHT: *value = s->h; return EGL_TRUE;
    case EGL_CONFIG_ID: *value = s->cfg->id; return EGL_TRUE;
    case EGL_RENDER_BUFFER: *value = s->window ? EGL_BACK_BUFFER : EGL_SINGLE_BUFFER; return EGL_TRUE;
    case EGL_SWAP_BEHAVIOR: *value = EGL_BUFFER_PRESERVED; return EGL_TRUE;
    case EGL_HORIZONTAL_RESOLUTION: case EGL_VERTICAL_RESOLUTION: case EGL_PIXEL_ASPECT_RATIO:
        *value = EGL_UNKNOWN;
        return EGL_TRUE;
    case EGL_MULTISAMPLE_RESOLVE: *value = EGL_MULTISAMPLE_RESOLVE_DEFAULT; return EGL_TRUE;
    case EGL_TEXTURE_FORMAT: case EGL_TEXTURE_TARGET: *value = EGL_NO_TEXTURE; return EGL_TRUE;
    }
    return fail(EGL_BAD_ATTRIBUTE);
}
NDK_EXPORT EGLBoolean bionic_eglSurfaceAttrib(EGLDisplay d, EGLSurface s, EGLint attr, EGLint value) {
    return dpy_ok(d);
}

// The surface's framebuffer in the current context (created, or resized, on demand).
static void ensure_fbo(Surface *s) {
    if (s->fbo && s->fbo_ctx != t_ctx) {  // made current with another context: FBOs aren't shared
        s->fbo = s->color = s->depth = 0;
    }
    if (s->w <= 0 || s->h <= 0) return;
    GLint prev_rb = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);
    if (!s->fbo) {
        glGenFramebuffers(1, &s->fbo);
        glGenRenderbuffers(1, &s->color);
        if (s->cfg->depth || s->cfg->stencil) glGenRenderbuffers(1, &s->depth);
        s->fbo_ctx = t_ctx;
    }
    glBindRenderbuffer(GL_RENDERBUFFER, s->color);
    glRenderbufferStorage(GL_RENDERBUFFER, s->cfg->a ? GL_RGBA8_OES : GL_RGB8_OES, s->w, s->h);
    if (s->depth) {
        glBindRenderbuffer(GL_RENDERBUFFER, s->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, s->cfg->stencil ? GL_DEPTH24_STENCIL8_OES : GL_DEPTH_COMPONENT16, s->w,
                              s->h);
    }
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)prev_rb);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s->color);
    if (s->depth) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s->depth);
        if (s->cfg->stencil)
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s->depth);
    }
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) ndk_log(NDK_LOG_WARN, "EGL: surface framebuffer incomplete (0x%x)", st);
}

static bool make_platform_current(Context *ctx) {
#if TARGET_OS_IPHONE
    return [EAGLContext setCurrentContext:ctx ? ctx->eagl : nil];
#else
    return CGLSetCurrentContext(ctx ? ctx->cgl : NULL) == kCGLNoError;
#endif
}

NDK_EXPORT EGLBoolean bionic_eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext context) {
    if (!dpy_ok(d)) return EGL_FALSE;
    Context *ctx = (Context *)context;
    Surface *ds = (Surface *)draw, *rs = (Surface *)read;
    if (!ctx && (ds || rs)) return fail(EGL_BAD_MATCH);
    if (!make_platform_current(ctx)) return fail(EGL_BAD_ACCESS);
    t_ctx = ctx;
    t_draw = ds;
    t_read = rs;
    if (ctx && ds) {
        ensure_fbo(ds);
        glBindFramebuffer(GL_FRAMEBUFFER, ds->fbo);
        if (!ctx->viewport_set) {  // what binding a context to a surface does for the first time
            glViewport(0, 0, ds->w, ds->h);
            glScissor(0, 0, ds->w, ds->h);
            ctx->viewport_set = true;
        }
    }
    return EGL_TRUE;
}

NDK_EXPORT EGLContext bionic_eglGetCurrentContext(void) { return t_ctx ? (EGLContext)t_ctx : EGL_NO_CONTEXT; }
NDK_EXPORT EGLDisplay bionic_eglGetCurrentDisplay(void) { return t_ctx ? DPY : EGL_NO_DISPLAY; }
NDK_EXPORT EGLSurface bionic_eglGetCurrentSurface(EGLint which) {
    Surface *s = which == EGL_READ ? t_read : t_draw;
    return s ? (EGLSurface)s : EGL_NO_SURFACE;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// Reads the surface back into its window (bottom-up rows; alpha forced opaque for the compositor).
static void present(Surface *s) {
    size_t row = (size_t)s->w * 4, need = row * (size_t)s->h;
    if (s->readback_size < need) {
        free(s->readback);
        s->readback = malloc(need);
        s->readback_size = need;
    }
    GLint prev_fb = 0, prev_align = 4;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
    glGetIntegerv(GL_PACK_ALIGNMENT, &prev_align);
#if TARGET_OS_IPHONE
    GLint prev_pbo = 0, prev_row = 0, prev_skip_r = 0, prev_skip_p = 0;
    if (t_ctx->version >= 3) {
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pbo);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &prev_row);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &prev_skip_r);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &prev_skip_p);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    }
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, s->w, s->h, GL_RGBA, GL_UNSIGNED_BYTE, s->readback);
    glPixelStorei(GL_PACK_ALIGNMENT, prev_align);
#if TARGET_OS_IPHONE
    if (t_ctx->version >= 3) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev_pbo);
        glPixelStorei(GL_PACK_ROW_LENGTH, prev_row);
        glPixelStorei(GL_PACK_SKIP_ROWS, prev_skip_r);
        glPixelStorei(GL_PACK_SKIP_PIXELS, prev_skip_p);
    }
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fb);

    NdkWindowBuffer buf;
    if (W.lock(s->window, &buf, NULL) != 0) return;
    int w = buf.width < s->w ? buf.width : s->w, h = buf.height < s->h ? buf.height : s->h;
    for (int y = 0; y < h; y++) {
        const uint32_t *src = (const uint32_t *)(s->readback + (size_t)(s->h - 1 - y) * row);
        uint32_t *dst = (uint32_t *)buf.bits + (size_t)y * (size_t)buf.stride;
        for (int x = 0; x < w; x++) dst[x] = src[x] | 0xFF000000u;
    }
    W.unlockAndPost(s->window);
}

NDK_EXPORT EGLBoolean bionic_eglSwapBuffers(EGLDisplay d, EGLSurface surface) {
    if (!dpy_ok(d)) return EGL_FALSE;
    Surface *s = (Surface *)surface;
    if (!s) return fail(EGL_BAD_SURFACE);
    if (!t_ctx || s != t_draw) return fail(EGL_BAD_SURFACE);
    if (!s->window) return EGL_TRUE;  // pbuffer, or a window already destroyed
    if (s->fbo) present(s);
    // The window may have been resized: the next frame is drawn at its new size.
    int nw = W.getWidth(s->window), nh = W.getHeight(s->window);
    if (nw != s->w || nh != s->h) {
        s->w = nw;
        s->h = nh;
        GLint prev_fb = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
        ensure_fbo(s);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fb);
    }
    // Pace to the display (60 Hz) like a real swap chain: games loop on eglSwapBuffers.
    if (s->interval > 0) {
        uint64_t period = 16666667ull * (uint64_t)s->interval, now = now_ns();
        if (s->next_frame_ns > now) {
            uint64_t wait = s->next_frame_ns - now;
            struct timespec ts = {(time_t)(wait / 1000000000ull), (long)(wait % 1000000000ull)};
            nanosleep(&ts, NULL);
            now = s->next_frame_ns;
        }
        s->next_frame_ns = (s->next_frame_ns && now - s->next_frame_ns < period ? s->next_frame_ns : now) + period;
    }
    return EGL_TRUE;
}
NDK_EXPORT EGLBoolean bionic_eglSwapBuffersWithDamageKHR(EGLDisplay d, EGLSurface s, const EGLint *r, EGLint n) {
    return bionic_eglSwapBuffers(d, s);
}
NDK_EXPORT EGLBoolean bionic_eglSwapInterval(EGLDisplay d, EGLint interval) {
    if (!dpy_ok(d)) return EGL_FALSE;
    if (t_draw) t_draw->interval = interval < 0 ? 0 : interval;
    return EGL_TRUE;
}
NDK_EXPORT EGLBoolean bionic_eglPresentationTimeANDROID(EGLDisplay d, EGLSurface s, EGLnsecsANDROID t) {
    return EGL_TRUE;
}
NDK_EXPORT EGLBoolean bionic_eglWaitGL(void) { if (t_ctx) glFinish(); return EGL_TRUE; }
NDK_EXPORT EGLBoolean bionic_eglWaitClient(void) { return bionic_eglWaitGL(); }
NDK_EXPORT EGLBoolean bionic_eglWaitNative(EGLint engine) { return EGL_TRUE; }
NDK_EXPORT EGLBoolean bionic_eglReleaseThread(void) {
    make_platform_current(NULL);
    t_ctx = NULL;
    t_draw = t_read = NULL;
    return EGL_TRUE;
}
NDK_EXPORT EGLBoolean bionic_eglCopyBuffers(EGLDisplay d, EGLSurface s, EGLNativePixmapType p) {
    return fail(EGL_BAD_NATIVE_PIXMAP);
}
NDK_EXPORT EGLSurface bionic_eglCreatePbufferFromClientBuffer(EGLDisplay d, EGLenum type, EGLClientBuffer b,
                                                              EGLConfig c, const EGLint *a) {
    t_error = EGL_BAD_PARAMETER;
    return EGL_NO_SURFACE;
}
NDK_EXPORT EGLSurface bionic_eglCreatePixmapSurface(EGLDisplay d, EGLConfig c, EGLNativePixmapType p, const EGLint *a) {
    t_error = EGL_BAD_NATIVE_PIXMAP;
    return EGL_NO_SURFACE;
}
NDK_EXPORT EGLint bionic_eglDupNativeFenceFDANDROID(EGLDisplay d, EGLSyncKHR s) { return -1; }
NDK_EXPORT EGLBoolean bionic_eglBindTexImage(EGLDisplay d, EGLSurface s, EGLint b) { return fail(EGL_BAD_MATCH); }
NDK_EXPORT EGLBoolean bionic_eglReleaseTexImage(EGLDisplay d, EGLSurface s, EGLint b) { return fail(EGL_BAD_MATCH); }

NDK_EXPORT void *bionic_eglGetProcAddress(const char *name) {
    if (!name) return NULL;
    void *p = andropkg_gl_symbol(name);  // our wrappers (bionic_gl*, bionic_egl*) first
    if (!p && strncmp(name, "gl", 2) == 0) p = ndk_gl_proc(name);
    return p;
}

// ---- framebuffer 0 is the current surface ------------------------------------------------------
NDK_EXPORT void bionic_glBindFramebuffer(GLenum target, GLuint fb) {
    if (fb == 0 && t_draw) {
        if (!t_draw->fbo || t_draw->fbo_ctx != t_ctx) ensure_fbo(t_draw);
        fb = t_draw->fbo;
    }
    glBindFramebuffer(target, fb);
}
NDK_EXPORT void bionic_glBindFramebufferOES(GLenum target, GLuint fb) { bionic_glBindFramebuffer(target, fb); }

#if TARGET_OS_IPHONE
// GL_COLOR/GL_DEPTH/GL_STENCIL name the default framebuffer's attachments.
static void map_default_attachments(GLsizei n, const GLenum *in, GLenum *out) {
    GLint fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    bool ours = t_draw && (GLuint)fb == t_draw->fbo;
    for (GLsizei i = 0; i < n; i++) {
        GLenum a = in[i];
        if (ours && a == 0x1800) a = GL_COLOR_ATTACHMENT0;
        if (ours && a == 0x1801) a = GL_DEPTH_ATTACHMENT;
        if (ours && a == 0x1802) a = GL_STENCIL_ATTACHMENT;
        out[i] = a;
    }
}
NDK_EXPORT void bionic_glInvalidateFramebuffer(GLenum target, GLsizei n, const GLenum *att) {
    GLenum m[8];
    if (n > 8) return;
    map_default_attachments(n, att, m);
    if (t_ctx && t_ctx->version >= 3) glInvalidateFramebuffer(target, n, m);
    else glDiscardFramebufferEXT(target, n, m);
}
NDK_EXPORT void bionic_glDiscardFramebufferEXT(GLenum target, GLsizei n, const GLenum *att) {
    GLenum m[8];
    if (n > 8) return;
    map_default_attachments(n, att, m);
    glDiscardFramebufferEXT(target, n, m);
}

// AAPCS64 gives every stack argument an 8-byte slot; Darwin packs them by size. Entry points
// with 4-byte arguments past the eighth one take them as 64-bit slots here.
NDK_EXPORT void bionic_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1,
                                         GLint dy1, uint64_t mask, uint64_t filter) {
    glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, (GLbitfield)mask, (GLenum)filter);
}
NDK_EXPORT void bionic_glTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h,
                                       GLsizei d, uint64_t format, uint64_t type, const void *pixels) {
    glTexSubImage3D(target, level, x, y, z, w, h, d, (GLenum)format, (GLenum)type, pixels);
}
NDK_EXPORT void bionic_glCompressedTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w,
                                                 GLsizei h, GLsizei d, uint64_t format, uint64_t size,
                                                 const void *data) {
    glCompressedTexSubImage3D(target, level, x, y, z, w, h, d, (GLenum)format, (GLsizei)size, data);
}
#else
// ---- macOS: GLES 2.0 on OpenGL 2.1 -------------------------------------------------------------
NDK_EXPORT const GLubyte *bionic_glGetString(GLenum name) {
    switch (name) {
    case GL_VERSION: return (const GLubyte *)"OpenGL ES 2.0 (andropkg on OpenGL 2.1)";
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"OpenGL ES GLSL ES 1.00";
    case GL_EXTENSIONS:
        return (const GLubyte *)"GL_OES_vertex_array_object GL_OES_standard_derivatives GL_OES_texture_npot "
                                "GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_rgb8_rgba8 "
                                "GL_OES_element_index_uint GL_EXT_texture_format_BGRA8888";
    }
    return glGetString(name);
}
NDK_EXPORT void bionic_glGetIntegerv(GLenum name, GLint *v) {
    switch (name) {
    case 0x8DFB: glGetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS, v); *v /= 4; return;   // MAX_VERTEX_UNIFORM_VECTORS
    case 0x8DFD: glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, v); *v /= 4; return; // MAX_FRAGMENT_UNIFORM_VECTORS
    case 0x8DFC: glGetIntegerv(GL_MAX_VARYING_FLOATS, v); *v /= 4; return;              // MAX_VARYING_VECTORS
    case 0x8DFA: *v = 1; return;                                                        // SHADER_COMPILER
    case 0x8DF9: *v = 0; return;                                                        // NUM_SHADER_BINARY_FORMATS
    case 0x8B9A: *v = GL_UNSIGNED_BYTE; return;  // IMPLEMENTATION_COLOR_READ_TYPE
    case 0x8B9B: *v = GL_RGBA; return;           // IMPLEMENTATION_COLOR_READ_FORMAT
    }
    glGetIntegerv(name, v);
}
NDK_EXPORT void bionic_glClearDepthf(GLfloat d) { glClearDepth(d); }
NDK_EXPORT void bionic_glDepthRangef(GLfloat n, GLfloat f) { glDepthRange(n, f); }
NDK_EXPORT void bionic_glReleaseShaderCompiler(void) {}
NDK_EXPORT void bionic_glShaderBinary(GLsizei n, const GLuint *s, GLenum fmt, const void *b, GLsizei len) {}
NDK_EXPORT void bionic_glGetShaderPrecisionFormat(GLenum shader, GLenum type, GLint *range, GLint *precision) {
    bool is_float = type <= 0x8DF5;  // LOW_FLOAT..HIGH_FLOAT
    range[0] = range[1] = is_float ? 127 : 31;
    *precision = is_float ? 23 : 0;
}
NDK_EXPORT void bionic_glGenVertexArraysOES(GLsizei n, GLuint *a) { glGenVertexArraysAPPLE(n, a); }
NDK_EXPORT void bionic_glBindVertexArrayOES(GLuint a) { glBindVertexArrayAPPLE(a); }
NDK_EXPORT void bionic_glDeleteVertexArraysOES(GLsizei n, const GLuint *a) { glDeleteVertexArraysAPPLE(n, a); }
NDK_EXPORT GLboolean bionic_glIsVertexArrayOES(GLuint a) { return glIsVertexArrayAPPLE(a); }
NDK_EXPORT void bionic_glTexImage2D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum fmt,
                                    GLenum type, const void *p) {
    if (ifmt == GL_BGRA) ifmt = GL_RGBA;  // EXT_texture_format_BGRA8888: BGRA internal format
    glTexImage2D(t, l, ifmt, w, h, b, fmt, type, p);
}

// GLSL ES 1.00 -> GLSL 1.20: the version line, precision statements and the ES-only
// extension directives go; precision qualifiers become empty macros.
static bool is_ident(char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
NDK_EXPORT void bionic_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths) {
    size_t total = 0;
    for (GLsizei i = 0; i < count; i++) total += lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
    char *src = malloc(total + 1), *p = src;
    for (GLsizei i = 0; i < count; i++) {
        size_t n = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
        memcpy(p, strings[i], n);
        p += n;
    }
    *p = 0;
    if (strstr(src, "#version 300") || strstr(src, "#version 310") || strstr(src, "#version 320"))
        ndk_log(NDK_LOG_WARN, "GLES: GLSL ES 3.x shader on the macOS GLES 2.0 stand-in");
    static const char kHeader[] = "#version 120\n#define lowp\n#define mediump\n#define highp\n";
    char *out = malloc(sizeof kHeader + total + 1), *o = out;
    memcpy(o, kHeader, sizeof kHeader - 1);
    o += sizeof kHeader - 1;
    for (char *line = src; *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line + 1) : strlen(line);
        const char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        bool drop = strncmp(t, "#version", 8) == 0 ||
                    (strncmp(t, "#extension", 10) == 0 && strstr(t, "GL_OES_standard_derivatives") &&
                     (!end || strstr(t, "GL_OES_standard_derivatives") < end));
        if (!drop) {
            // "precision <qualifier> <type>;" statements (anywhere in the line)
            for (size_t i = 0; i < len; i++) {
                if (strncmp(line + i, "precision", 9) == 0 && (i == 0 || !is_ident(line[i - 1])) &&
                    !is_ident(line[i + 9])) {
                    char *semi = memchr(line + i, ';', len - i);
                    if (semi) {
                        memset(line + i, ' ', (size_t)(semi - (line + i)) + 1);
                        i = (size_t)(semi - line);
                    }
                }
            }
            memcpy(o, line, len);
            o += len;
        }
        line += len;
    }
    *o = 0;
    const GLchar *one = out;
    glShaderSource(shader, 1, &one, NULL);
    free(src);
    free(out);
}
#endif
