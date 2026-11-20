package dev.andropkg.system;

import android.content.res.Configuration;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.IBinder;
import android.util.Log;
import android.view.SurfaceControl;
import java.util.HashMap;
import java.util.Map;

/**
 * android.view.IWindowManager and the per-client IWindowSession: what WindowManagerService
 * does for one app. Every window covers the display; each gets a SurfaceControl whose
 * buffer the host shows (no compositor: iOS composes the result).
 */
final class WindowManagerImpl {
    private static final String TAG = "andropkg";
    static final boolean DEBUG = System.getProperty("andropkg.debug.window") != null;

    static DisplayManagerImpl display;
    private final Session session = new Session();
    private final LocalService.LocalBinder sessionBinder = LocalService.create("android.view.IWindowSession", session);

    WindowManagerImpl(DisplayManagerImpl display) {
        WindowManagerImpl.display = display;
    }

    // IWindowManager
    public Object openSession(Object callback) { return sessionBinder.iface; }
    public float getCurrentAnimatorScale() { return 1f; }
    public int getInitialDisplayDensity(int displayId) { return display.dpi; }
    public int getBaseDisplayDensity(int displayId) { return display.dpi; }
    public boolean hasNavigationBar(int displayId) { return false; }
    public boolean isKeyguardLocked() { return false; }
    public boolean isKeyguardSecure(int userId) { return false; }
    public int getDefaultDisplayRotation() { return 0; }

    /** The display as an app window's Configuration sees it (bounds, size in dp, density). */
    static void applyDisplay(Configuration config) {
        int w = display.width, h = display.height, dpi = display.dpi;
        config.densityDpi = dpi;
        config.screenWidthDp = w * 160 / dpi;
        config.screenHeightDp = h * 160 / dpi;
        config.smallestScreenWidthDp = Math.min(config.screenWidthDp, config.screenHeightDp);
        config.orientation = h >= w ? Configuration.ORIENTATION_PORTRAIT : Configuration.ORIENTATION_LANDSCAPE;
        Object wc = Hidden.getField(config, "windowConfiguration");
        Rect bounds = new Rect(0, 0, w, h);
        Hidden.call(wc, "setBounds", bounds);
        Hidden.call(wc, "setAppBounds", bounds);
        Hidden.call(wc, "setMaxBounds", bounds);
        Hidden.call(wc, "setWindowingMode", 1 /* WINDOWING_MODE_FULLSCREEN */);
        Hidden.call(wc, "setRotation", 0);
    }

    static Configuration displayConfiguration(Configuration base) {
        Configuration c = new Configuration(base);
        applyDisplay(c);
        return c;
    }

    /** android.view.IWindowSession. */
    static final class Session {
        private final Map<IBinder, SurfaceControl> surfaces = new HashMap<>();

        private static IBinder key(Object window) {
            return ((android.os.IInterface) window).asBinder();
        }

        // addToDisplayAsUser(IWindow, LayoutParams, int viewVisibility, int displayId, int userId,
        //     int requestedVisibleTypes, InputChannel out, InsetsState out,
        //     InsetsSourceControl.Array out, Rect attachedFrame out, float[] sizeCompatScale out)
        public int addToDisplayAsUser(Object window, Object attrs, int viewVisibility, int displayId, int userId,
                int requestedVisibleTypes, Object outInputChannel, Object outInsetsState,
                Object outActiveControls, Rect outAttachedFrame, float[] outSizeCompatScale) {
            Log.i(TAG, "window added: " + Hidden.call(attrs, "getTitle"));
            Hidden.call(outInsetsState, "setDisplayFrame", new Rect(0, 0, display.width, display.height));
            if (outAttachedFrame != null) outAttachedFrame.setEmpty();
            if (outSizeCompatScale != null && outSizeCompatScale.length > 0) outSizeCompatScale[0] = 1f;
            // ADD_OKAY, and the window is visible (the activity is in front) in touch mode.
            Class<?> g = Hidden.cls("android.view.WindowManagerGlobal");
            return (int) Hidden.getField(g, "ADD_FLAG_APP_VISIBLE") | (int) Hidden.getField(g, "ADD_FLAG_IN_TOUCH_MODE");
        }

        public int addToDisplay(Object window, Object attrs, int viewVisibility, int displayId,
                int requestedVisibleTypes, Object outInputChannel, Object outInsetsState,
                Object outActiveControls, Rect outAttachedFrame, float[] outSizeCompatScale) {
            return addToDisplayAsUser(window, attrs, viewVisibility, displayId, 0, requestedVisibleTypes,
                    outInputChannel, outInsetsState, outActiveControls, outAttachedFrame, outSizeCompatScale);
        }

        // relayout(IWindow, LayoutParams, int reqW, int reqH, int viewVisibility, int flags, int seq,
        //     int lastSyncSeqId, ClientWindowFrames out, MergedConfiguration out, SurfaceControl out,
        //     InsetsState out, InsetsSourceControl.Array out, Bundle out)
        public int relayout(Object window, Object attrs, int requestedWidth, int requestedHeight,
                int viewVisibility, int flags, int seq, int lastSyncSeqId, Object outFrames,
                Object outMergedConfiguration, SurfaceControl outSurfaceControl, Object outInsetsState,
                Object outActiveControls, Bundle outSyncSeqIdBundle) {
            int w = display.width, h = display.height;
            Rect full = new Rect(0, 0, w, h);
            if (DEBUG) Log.i(TAG, "relayout " + requestedWidth + "x" + requestedHeight + " vis=" + viewVisibility);
            if (outFrames != null) {
                ((Rect) Hidden.getField(outFrames, "frame")).set(full);
                ((Rect) Hidden.getField(outFrames, "displayFrame")).set(full);
                ((Rect) Hidden.getField(outFrames, "parentFrame")).set(full);
            }
            if (outMergedConfiguration != null) {
                Configuration global = displayConfiguration(
                        android.content.res.Resources.getSystem().getConfiguration());
                Hidden.call(outMergedConfiguration, "setConfiguration", global, new Configuration(global));
            }
            if (outInsetsState != null) Hidden.call(outInsetsState, "setDisplayFrame", full);
            int result = 0;
            if (viewVisibility == 0 /* VISIBLE */ && outSurfaceControl != null) {
                SurfaceControl sc = surfaces.get(key(window));
                if (sc == null) {
                    sc = new SurfaceControl.Builder().setName("andropkg:" + Hidden.call(attrs, "getTitle"))
                            .setBufferSize(w, h).build();
                    surfaces.put(key(window), sc);
                    result |= (int) Hidden.getField(Hidden.cls("android.view.WindowManagerGlobal"),
                            "RELAYOUT_RES_FIRST_TIME");
                }
                Hidden.call(outSurfaceControl, "copyFrom", sc, "relayout");
            }
            return result;
        }

        public void finishDrawing(Object window, SurfaceControl.Transaction postDrawTransaction, int seqId) {
            if (postDrawTransaction != null) postDrawTransaction.apply();
        }

        public void remove(Object window) {
            surfaces.remove(key(window));
            Log.i(TAG, "window removed");
        }

        public boolean performHapticFeedback(int effectId, boolean always) { return false; }
    }
}
