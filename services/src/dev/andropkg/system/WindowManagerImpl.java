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

    /** The display frame and the system bars an app window must keep clear of. */
    static void fillInsets(Object insetsState) {
        int w = display.width, h = display.height;
        Hidden.call(insetsState, "setDisplayFrame", new Rect(0, 0, w, h));
        addBar(insetsState, 0, android.view.WindowInsets.Type.statusBars(), new Rect(0, 0, w, display.insetTop));
        if (display.insetBottom > 0) {
            addBar(insetsState, 1, android.view.WindowInsets.Type.navigationBars(),
                    new Rect(0, h - display.insetBottom, w, h));
        }
    }

    private static void addBar(Object insetsState, int index, int type, Rect frame) {
        int id = (int) Hidden.callStatic(Hidden.cls("android.view.InsetsSource"), "createId", null, index, type);
        Object source = Hidden.call(insetsState, "getOrCreateSource", id, type);
        Hidden.call(source, "setFrame", frame);
        Hidden.call(source, "setVisible", true);
    }

    static Configuration displayConfiguration(Configuration base) {
        Configuration c = new Configuration(base);
        applyDisplay(c);
        return c;
    }

    /** android.view.IWindowSession: window frames, z-order and surfaces, as WMS computes them. */
    static final class Session {
        /** One window of the app: what WMS's WindowState tracks. */
        private static final class Win {
            final int id;
            final IBinder client;
            final Win parent;  // sub-windows (panels, popups): the window they are attached to
            final int z;
            final android.view.WindowManager.LayoutParams attrs = new android.view.WindowManager.LayoutParams();
            int requestedVisibleTypes = android.view.WindowInsets.Type.systemBars();
            SurfaceControl sc;
            final Rect frame = new Rect();

            Win(int id, IBinder client, Win parent, int z) {
                this.id = id;
                this.client = client;
                this.parent = parent;
                this.z = z;
            }
        }

        private final Map<IBinder, Win> windows = new HashMap<>();
        private int nextId = 1;
        private int nextSeq = 1;
        private final Object layout = Hidden.newInstance(Hidden.cls("android.view.WindowLayout"));

        private static IBinder key(Object window) {
            return ((android.os.IInterface) window).asBinder();
        }

        private static boolean isSubWindow(int type) {
            return type >= 1000 && type <= 1999;  // FIRST_SUB_WINDOW .. LAST_SUB_WINDOW
        }

        /** Z-order as WMS's policy layers it: app windows, their sub-windows, then system windows. */
        private int zFor(android.view.WindowManager.LayoutParams a, Win parent) {
            int seq = nextSeq++;
            if (parent != null && isSubWindow(a.type)) {
                int sub;
                switch (a.type) {
                    case 1001: sub = -2; break;  // TYPE_APPLICATION_MEDIA: below its parent
                    case 1004: sub = -1; break;  // TYPE_APPLICATION_MEDIA_OVERLAY
                    case 1002: sub = 2; break;   // TYPE_APPLICATION_SUB_PANEL
                    case 1005: sub = 3; break;   // TYPE_APPLICATION_ABOVE_SUB_PANEL
                    default: sub = 1;            // PANEL, ATTACHED_DIALOG
                }
                return parent.z + sub * 1000 + seq % 1000;
            }
            int base = a.type < 1000 ? 1 : 2;  // application windows, then system (toasts, overlays)
            return base * 100_000_000 + seq * 10_000;
        }

        // addToDisplayAsUser(IWindow, LayoutParams, int viewVisibility, int displayId, int userId,
        //     int requestedVisibleTypes, InputChannel out, InsetsState out,
        //     InsetsSourceControl.Array out, Rect attachedFrame out, float[] sizeCompatScale out)
        public int addToDisplayAsUser(Object window, Object attrs, int viewVisibility, int displayId, int userId,
                int requestedVisibleTypes, Object outInputChannel, Object outInsetsState,
                Object outActiveControls, Rect outAttachedFrame, float[] outSizeCompatScale) {
            android.view.WindowManager.LayoutParams lp = (android.view.WindowManager.LayoutParams) attrs;
            Win parent = isSubWindow(lp.type) && lp.token != null ? windows.get(lp.token) : null;
            Win w = new Win(nextId++, key(window), parent, zFor(lp, parent));
            w.attrs.copyFrom(lp);
            w.requestedVisibleTypes = requestedVisibleTypes;
            windows.put(w.client, w);
            // The window's input channel names it: the runtime routes input by this id.
            if (outInputChannel != null) Hidden.setField(outInputChannel, "mPtr", (long) w.id);
            Log.i(TAG, "window added: " + lp.getTitle() + (DEBUG ? " type=" + lp.type + " z=" + w.z : ""));
            fillInsets(outInsetsState);
            if (outAttachedFrame != null) {
                if (parent != null) outAttachedFrame.set(parent.frame); else outAttachedFrame.setEmpty();
            }
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

        public void updateRequestedVisibleTypes(Object window, int types) {
            Win w = windows.get(key(window));
            if (w != null) w.requestedVisibleTypes = types;
        }

        // relayout(IWindow, LayoutParams, int reqW, int reqH, int viewVisibility, int flags, int seq,
        //     int lastSyncSeqId, ClientWindowFrames out, MergedConfiguration out, SurfaceControl out,
        //     InsetsState out, InsetsSourceControl.Array out, Bundle out)
        public int relayout(Object window, Object attrs, int requestedWidth, int requestedHeight,
                int viewVisibility, int flags, int seq, int lastSyncSeqId, Object outFrames,
                Object outMergedConfiguration, SurfaceControl outSurfaceControl, Object outInsetsState,
                Object outActiveControls, Bundle outSyncSeqIdBundle) {
            Win w = windows.get(key(window));
            if (w == null) return 0;
            if (attrs != null) w.attrs.copyFrom((android.view.WindowManager.LayoutParams) attrs);
            int dw = display.width, dh = display.height;
            Rect full = new Rect(0, 0, dw, dh);

            // The frame, exactly as WMS computes it (WindowLayout is shared with the client).
            Object frames = outFrames != null ? outFrames
                    : Hidden.newInstance(Hidden.cls("android.window.ClientWindowFrames"));
            Hidden.setField(frames, "attachedFrame", w.parent != null ? new Rect(w.parent.frame) : null);
            Object state = Hidden.newInstance(Hidden.cls("android.view.InsetsState"));
            fillInsets(state);
            Hidden.call(layout, "computeFrames", w.attrs, state, full, full, 1 /* WINDOWING_MODE_FULLSCREEN */,
                    requestedWidth, requestedHeight, w.requestedVisibleTypes, 1f, frames);
            w.frame.set((Rect) Hidden.getField(frames, "frame"));
            if (DEBUG) {
                Log.i(TAG, "relayout " + w.attrs.getTitle() + " req=" + requestedWidth + "x" + requestedHeight
                        + " vis=" + viewVisibility + " frame=" + w.frame);
            }
            if (outMergedConfiguration != null) {
                Configuration global = displayConfiguration(
                        android.content.res.Resources.getSystem().getConfiguration());
                Hidden.call(outMergedConfiguration, "setConfiguration", global, new Configuration(global));
            }
            if (outInsetsState != null) fillInsets(outInsetsState);

            // The surface: the frame plus surface insets (room for the window's own shadow).
            Rect si = (Rect) Hidden.getField(w.attrs, "surfaceInsets");
            int sw = Math.max(1, w.frame.width() + si.left + si.right);
            int sh = Math.max(1, w.frame.height() + si.top + si.bottom);
            int result = 0;
            boolean visible = viewVisibility == 0 /* VISIBLE */;
            if (visible && outSurfaceControl != null) {
                if (w.sc == null) {
                    w.sc = new SurfaceControl.Builder().setName("andropkg:" + w.attrs.getTitle())
                            .setBufferSize(sw, sh).build();
                    result |= (int) Hidden.getField(Hidden.cls("android.view.WindowManagerGlobal"),
                            "RELAYOUT_RES_FIRST_TIME");
                }
                Hidden.call(outSurfaceControl, "copyFrom", w.sc, "relayout");
            }
            if (w.sc != null) {
                float dim = (w.attrs.flags & android.view.WindowManager.LayoutParams.FLAG_DIM_BEHIND) != 0
                        ? w.attrs.dimAmount : 0f;
                nativeSetWindow(w.id, (long) Hidden.getField(w.sc, "mNativeObject"),
                        w.frame.left, w.frame.top, w.frame.width(), w.frame.height(),
                        w.frame.left - si.left, w.frame.top - si.top, sw, sh,
                        w.z, w.attrs.flags, dim, visible, dw, dh);
            }
            return result;
        }

        public void finishDrawing(Object window, SurfaceControl.Transaction postDrawTransaction, int seqId) {
            if (postDrawTransaction != null) postDrawTransaction.apply();
        }

        public void remove(Object window) {
            Win w = windows.remove(key(window));
            if (w == null) return;
            if (w.sc != null) nativeRemoveWindow(w.id);
            Log.i(TAG, "window removed: " + w.attrs.getTitle());
            if (DEBUG) Log.i(TAG, "removed from", new Throwable());
        }

        public boolean performHapticFeedback(int effectId, boolean always) { return false; }
    }

    // Native (runtime/graphics/window_jni.cc): the host-side window table — composition, input
    // routing and focus. Registered when the first SurfaceControl is created.
    static native void nativeSetWindow(int id, long surfaceControl, int frameX, int frameY, int frameW,
            int frameH, int surfaceX, int surfaceY, int surfaceW, int surfaceH, int z, int flags, float dim,
            boolean visible, int displayW, int displayH);

    static native void nativeRemoveWindow(int id);
}
