package dev.andropkg.system;

import android.app.Activity;
import android.app.Application;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import java.lang.reflect.Proxy;

/**
 * -Dandropkg.debug.touch: logs, for every touch reaching an activity window, whether the view
 * tree consumed it and the chain of views that took it (ViewGroup touch targets).
 */
final class TouchDebug {
    private static final String TAG = "andropkg-touch";

    private TouchDebug() {}

    static void installWhenReady() {
        if (System.getProperty("andropkg.debug.touch") == null) return;
        Thread t = new Thread(() -> {
            Application app;
            while ((app = (Application) Hidden.callStatic(Hidden.cls("android.app.ActivityThread"),
                    "currentApplication")) == null) {
                try { Thread.sleep(20); } catch (InterruptedException e) { return; }
            }
            Application a = app;
            new Handler(Looper.getMainLooper()).postAtFrontOfQueue(() -> a.registerActivityLifecycleCallbacks(CALLBACKS));
        }, "andropkg-touchdebug");
        t.setDaemon(true);
        t.start();
    }

    private static String describe(View v) {
        String id = "";
        if (v.getId() != View.NO_ID) {
            try { id = "#" + v.getResources().getResourceEntryName(v.getId()); } catch (RuntimeException e) { id = "#" + v.getId(); }
        }
        return v.getClass().getSimpleName() + id + (v.isClickable() ? "(clickable)" : "");
    }

    private static String chain(View root) {
        StringBuilder sb = new StringBuilder(describe(root));
        View v = root;
        while (v instanceof ViewGroup) {
            Object target = Hidden.getField(v, "mFirstTouchTarget");
            if (target == null) break;
            v = (View) Hidden.getField(target, "child");
            sb.append(" > ").append(describe(v));
        }
        return sb.toString();
    }

    private static final Application.ActivityLifecycleCallbacks CALLBACKS = new Application.ActivityLifecycleCallbacks() {
        @Override public void onActivityResumed(Activity activity) {
            Window w = activity.getWindow();
            Window.Callback cb = w.getCallback();
            if (Proxy.isProxyClass(cb.getClass())) return;
            w.setCallback((Window.Callback) Proxy.newProxyInstance(Window.Callback.class.getClassLoader(),
                    new Class<?>[] {Window.Callback.class}, (proxy, m, args) -> {
                        Object r = m.invoke(cb, args);
                        if (m.getName().equals("dispatchKeyEvent")) {
                            Log.i(TAG, activity.getLocalClassName() + " key " + args[0] + " handled=" + r);
                        }
                        if (m.getName().equals("onMenuItemSelected")) {
                            Log.i(TAG, activity.getLocalClassName() + " menu item " + args[1] + " handled=" + r);
                        }
                        if (m.getName().equals("dispatchTouchEvent")) {
                            MotionEvent e = (MotionEvent) args[0];
                            if (e.getActionMasked() != MotionEvent.ACTION_MOVE) {
                                Log.i(TAG, activity.getLocalClassName() + " " + MotionEvent.actionToString(e.getActionMasked())
                                        + " " + (int) e.getX() + "," + (int) e.getY() + " handled=" + r
                                        + " via " + chain(w.getDecorView()));
                            }
                        }
                        return r;
                    }));
        }
        @Override public void onActivityCreated(Activity a, Bundle b) {}
        @Override public void onActivityStarted(Activity a) {}
        @Override public void onActivityPaused(Activity a) {}
        @Override public void onActivityStopped(Activity a) {}
        @Override public void onActivitySaveInstanceState(Activity a, Bundle b) {}
        @Override public void onActivityDestroyed(Activity a) {}
    };
}
