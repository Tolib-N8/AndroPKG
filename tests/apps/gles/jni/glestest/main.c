// EGL + GLES 2.0 in a NativeActivity: a rotating triangle (shaders, client-side vertex arrays,
// a uniform) over a blue clear. Logs the GL strings, a pixel read back from framebuffer 0,
// the frame count; a tap finishes.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "GlesTest", __VA_ARGS__)

struct gl {
    EGLDisplay dpy; EGLSurface surf; EGLContext ctx;
    int w, h, frames;
    GLuint prog; GLint aPos, uColor, uAngle;
};

static const char* kVs =
    "#version 100\n"
    "attribute vec2 aPos;\n"
    "uniform float uAngle;\n"
    "void main() {\n"
    "  float c = cos(uAngle), s = sin(uAngle);\n"
    "  gl_Position = vec4(c * aPos.x - s * aPos.y, s * aPos.x + c * aPos.y, 0.0, 1.0);\n"
    "}\n";
static const char* kFs =
    "precision mediump float;\n"
    "uniform lowp vec4 uColor;\n"
    "void main() { gl_FragColor = uColor; }\n";

static GLuint shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, sizeof log, NULL, log); LOG("[gl] shader error: %s", log); }
    return s;
}

static int init(struct android_app* app, struct gl* g) {
    g->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor;
    eglInitialize(g->dpy, &major, &minor);
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 16, EGL_NONE};
    EGLConfig cfg; EGLint n = 0;
    eglChooseConfig(g->dpy, attrs, &cfg, 1, &n);
    EGLint visual; eglGetConfigAttrib(g->dpy, cfg, EGL_NATIVE_VISUAL_ID, &visual);
    ANativeWindow_setBuffersGeometry(app->window, 0, 0, visual);
    g->surf = eglCreateWindowSurface(g->dpy, cfg, app->window, NULL);
    const EGLint cattrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    g->ctx = eglCreateContext(g->dpy, cfg, EGL_NO_CONTEXT, cattrs);
    if (eglMakeCurrent(g->dpy, g->surf, g->surf, g->ctx) != EGL_TRUE) { LOG("[gl] makeCurrent failed 0x%x", eglGetError()); return -1; }
    eglQuerySurface(g->dpy, g->surf, EGL_WIDTH, &g->w);
    eglQuerySurface(g->dpy, g->surf, EGL_HEIGHT, &g->h);
    LOG("[gl] egl %d.%d configs=%d surface %dx%d", major, minor, n, g->w, g->h);
    LOG("[gl] version: %s", glGetString(GL_VERSION));
    LOG("[gl] glsl: %s", glGetString(GL_SHADING_LANGUAGE_VERSION));
    g->prog = glCreateProgram();
    glAttachShader(g->prog, shader(GL_VERTEX_SHADER, kVs));
    glAttachShader(g->prog, shader(GL_FRAGMENT_SHADER, kFs));
    glLinkProgram(g->prog);
    GLint linked = 0; glGetProgramiv(g->prog, GL_LINK_STATUS, &linked);
    g->aPos = glGetAttribLocation(g->prog, "aPos");
    g->uColor = glGetUniformLocation(g->prog, "uColor");
    g->uAngle = glGetUniformLocation(g->prog, "uAngle");
    LOG("[gl] program linked=%d", linked);
    return 0;
}

static void draw(struct gl* g) {
    if (!g->ctx) return;
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    static const GLfloat tri[] = {0.0f, 0.8f, -0.7f, -0.6f, 0.7f, -0.6f};
    glUseProgram(g->prog);
    glUniform4f(g->uColor, 1.0f, 0.5f, 0.0f, 1.0f);
    glUniform1f(g->uAngle, g->frames * 0.02f);
    glVertexAttribPointer((GLuint)g->aPos, 2, GL_FLOAT, GL_FALSE, 0, tri);
    glEnableVertexAttribArray((GLuint)g->aPos);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (g->frames == 0) {
        unsigned char center[4], corner[4];
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glReadPixels(g->w / 2, g->h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
        glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
        LOG("[gl] center=%02x%02x%02x corner=%02x%02x%02x error=0x%x", center[0], center[1], center[2],
            corner[0], corner[1], corner[2], glGetError());
    }
    eglSwapBuffers(g->dpy, g->surf);
    static double t1;
    if (++g->frames == 1) t1 = now_s();
    if (g->frames == 30) LOG("[gl] 30 frames");
    if (g->frames == 61) LOG("[gl] fps=%.0f", 60 / (now_s() - t1));
}

static void term(struct gl* g) {
    if (g->dpy == EGL_NO_DISPLAY) return;
    eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g->ctx) eglDestroyContext(g->dpy, g->ctx);
    if (g->surf) eglDestroySurface(g->dpy, g->surf);
    g->ctx = EGL_NO_CONTEXT; g->surf = EGL_NO_SURFACE;
}

static void on_cmd(struct android_app* app, int32_t cmd) {
    struct gl* g = app->userData;
    if (cmd == APP_CMD_INIT_WINDOW && app->window) init(app, g);
    if (cmd == APP_CMD_TERM_WINDOW) { LOG("[gl] term window after %d frames", g->frames); term(g); }
}

static int32_t on_input(struct android_app* app, AInputEvent* e) {
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION &&
        (AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK) == AMOTION_EVENT_ACTION_UP) {
        LOG("[gl] tap");
        ANativeActivity_finish(app->activity);
        return 1;
    }
    return 0;
}

void android_main(struct android_app* app) {
    struct gl g = {0};
    app->userData = &g;
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    for (;;) {
        int events;
        struct android_poll_source* src;
        while (ALooper_pollOnce(g.ctx ? 0 : -1, NULL, &events, (void**)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) { term(&g); LOG("[gl] exit"); return; }
        }
        draw(&g);
    }
}
