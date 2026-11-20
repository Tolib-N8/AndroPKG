package dev.andropkg.system;

import android.content.Context;
import android.graphics.PixelFormat;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import java.util.HashMap;
import java.util.Map;

/**
 * android.app.INotificationManager: toasts. Since Android 11 text toasts are drawn by
 * SystemUI; here the same view (ToastPresenter's text toast) is shown in a TYPE_TOAST window
 * of the app's process. Custom-view toasts are shown by the app itself (Toast.TN) when told.
 * Notifications are accepted and dropped for now.
 */
final class NotificationManagerImpl {
    private static final String TAG = "andropkg";
    private Handler mainHandler;  // created on first use: services start before the main Looper
    private final Map<IBinder, View> shown = new HashMap<>();

    private synchronized Handler main() {
        if (mainHandler == null) mainHandler = new Handler(Looper.getMainLooper());
        return mainHandler;
    }

    private static int durationMs(int duration) {
        return duration == 1 /* LENGTH_LONG */ ? 3500 : 2000;
    }

    // enqueueTextToast(String pkg, IBinder token, CharSequence text, int duration, boolean isUiContext,
    //     int displayId, ITransientNotificationCallback callback)
    public void enqueueTextToast(String pkg, IBinder token, CharSequence text, int duration, boolean isUiContext,
            int displayId, Object callback) {
        Log.i(TAG, "toast: " + text);
        main().post(() -> {
            Context ctx = (Context) Hidden.callStatic(Hidden.cls("android.app.ActivityThread"), "currentApplication");
            if (ctx == null) return;
            hide(token);
            View v = (View) Hidden.callStatic(Hidden.cls("android.widget.ToastPresenter"), "getTextToastView", ctx, text);
            WindowManager.LayoutParams lp = new WindowManager.LayoutParams();
            lp.type = 2005;  // TYPE_TOAST
            lp.width = lp.height = WindowManager.LayoutParams.WRAP_CONTENT;
            lp.format = PixelFormat.TRANSLUCENT;
            lp.flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                    | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON;
            lp.gravity = Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL;
            int id = ctx.getResources().getIdentifier("toast_y_offset", "dimen", "android");
            lp.y = id != 0 ? ctx.getResources().getDimensionPixelSize(id) : (int) (24 * ctx.getResources().getDisplayMetrics().density);
            lp.setTitle("Toast");
            ctx.getSystemService(WindowManager.class).addView(v, lp);
            shown.put(token, v);
            if (callback != null) Hidden.call(callback, "onToastShown");
            main().postDelayed(() -> {
                if (shown.get(token) == v) {
                    hide(token);
                    if (callback != null) Hidden.call(callback, "onToastHidden");
                }
            }, durationMs(duration));
        });
    }

    // enqueueToast(String pkg, IBinder token, ITransientNotification callback, int duration,
    //     boolean isUiContext, int displayId): a custom-view toast, shown by the app's Toast.TN.
    public void enqueueToast(String pkg, IBinder token, Object callback, int duration, boolean isUiContext,
            int displayId) {
        main().post(() -> {
            Hidden.call(callback, "show", new android.os.Binder());
            main().postDelayed(() -> Hidden.call(callback, "hide"), durationMs(duration));
        });
    }

    public void cancelToast(String pkg, IBinder token) {
        main().post(() -> hide(token));
    }

    private void hide(IBinder token) {
        View v = shown.remove(token);
        if (v != null && v.isAttachedToWindow()) {
            v.getContext().getSystemService(WindowManager.class).removeViewImmediate(v);
        }
    }

    public boolean areNotificationsEnabled(String pkg) { return true; }
    public boolean areNotificationsEnabledForPackage(String pkg, int uid) { return true; }
    public int getImportance(String pkg) { return 3; /* IMPORTANCE_DEFAULT */ }
    public int getPackageImportance(String pkg) { return 3; }
}
