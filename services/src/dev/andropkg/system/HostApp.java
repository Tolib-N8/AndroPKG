package dev.andropkg.system;

import android.util.Log;

/**
 * The host app's own lifecycle, as the app's task sees it: iOS moving the app to the
 * background is the task going behind another (top activity paused and stopped); coming
 * back resumes it. Called by native code on the app's main thread.
 */
final class HostApp {
    private static boolean visible = true;

    private HostApp() {}

    static void setVisible(boolean v) {
        if (v == visible) return;
        visible = v;
        Log.i("andropkg", v ? "host: foreground" : "host: background");
        if (v) {
            AndroPkgSystem.stack.foreground();
        } else {
            AndroPkgSystem.stack.background();
        }
    }
}
