// A NativeActivity (android_native_app_glue) drawing in software through ANativeWindow:
// a 160x120 RGB_565 buffer (scaled to the window), red left / blue right; a tap turns it
// green and finishes the activity.
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <stdint.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "NativeActTest", __VA_ARGS__)

struct state { int touched, frames; };

static void draw(struct android_app* app, struct state* st) {
    ANativeWindow* w = app->window;
    if (!w) return;
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(w, &buf, NULL) != 0) { LOG("[nat] lock failed"); return; }
    for (int y = 0; y < buf.height; y++) {
        uint16_t* row = (uint16_t*)buf.bits + y * buf.stride;
        for (int x = 0; x < buf.width; x++)
            row[x] = st->touched ? 0x07E0 : (x < buf.width / 2 ? 0xF800 : 0x001F);
    }
    ANativeWindow_unlockAndPost(w);
    if (st->frames++ == 0) LOG("[nat] first frame %dx%d fmt=%d", buf.width, buf.height, buf.format);
}

static void on_cmd(struct android_app* app, int32_t cmd) {
    struct state* st = app->userData;
    switch (cmd) {
    case APP_CMD_INIT_WINDOW: {
        int ww = ANativeWindow_getWidth(app->window), wh = ANativeWindow_getHeight(app->window);
        ANativeWindow_setBuffersGeometry(app->window, 160, 120, WINDOW_FORMAT_RGB_565);
        LOG("[nat] window %dx%d -> buffer %dx%d", ww, wh, ANativeWindow_getWidth(app->window),
            ANativeWindow_getHeight(app->window));
        draw(app, st);
        break;
    }
    case APP_CMD_GAINED_FOCUS: LOG("[nat] focus"); break;
    case APP_CMD_RESUME: LOG("[nat] resume"); break;
    case APP_CMD_TERM_WINDOW: LOG("[nat] term window"); break;
    case APP_CMD_DESTROY: LOG("[nat] destroy"); break;
    }
}

static int32_t on_input(struct android_app* app, AInputEvent* e) {
    struct state* st = app->userData;
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION &&
        (AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK) == AMOTION_EVENT_ACTION_DOWN) {
        LOG("[nat] touch %d,%d pointers=%zu", (int)AMotionEvent_getX(e, 0), (int)AMotionEvent_getY(e, 0),
            AMotionEvent_getPointerCount(e));
        st->touched = 1;
        draw(app, st);
        ANativeActivity_finish(app->activity);
        return 1;
    }
    return 0;
}

void android_main(struct android_app* app) {
    struct state st = {0};
    app->userData = &st;
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    LOG("[nat] android_main sdk=%d internal=%s", app->activity->sdkVersion,
        app->activity->internalDataPath ? "yes" : "no");
    for (;;) {
        int events;
        struct android_poll_source* src;
        while (ALooper_pollOnce(-1, NULL, &events, (void**)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) { LOG("[nat] exit"); return; }
        }
    }
}
