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
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationChannelGroup;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * android.app.INotificationManager: toasts. Since Android 11 text toasts are drawn by
 * SystemUI; here the same view (ToastPresenter's text toast) is shown in a TYPE_TOAST window
 * of the app's process. Custom-view toasts are shown by the app itself (Toast.TN) when told.
 * Notification channels and groups are kept (in memory) so apps can read them back;
 * notifications themselves are logged and dropped for now.
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

    // ---- channels ------------------------------------------------------------------------
    private final Map<String, NotificationChannel> channels = new LinkedHashMap<>();
    private final Map<String, NotificationChannelGroup> groups = new LinkedHashMap<>();

    @SuppressWarnings("unchecked")
    private static <T> List<T> listOf(Object slice) {
        return slice == null ? new ArrayList<>() : (List<T>) Hidden.call(slice, "getList");
    }

    private static Object slice(List<?> list) {
        return Hidden.newInstance(Hidden.cls("android.content.pm.ParceledListSlice"), list);
    }

    public void createNotificationChannels(String pkg, Object slice) {
        synchronized (channels) {
            for (NotificationChannel c : NotificationManagerImpl.<NotificationChannel>listOf(slice)) {
                NotificationChannel old = channels.get(c.getId());
                if (old != null) {
                    // Like NotificationManagerService: an app may rename, lower importance, re-describe.
                    old.setName(c.getName());
                    old.setDescription(c.getDescription());
                    if (c.getImportance() < old.getImportance()) old.setImportance(c.getImportance());
                    if (old.getGroup() == null) old.setGroup(c.getGroup());
                } else {
                    channels.put(c.getId(), c);
                }
            }
        }
    }

    public void createNotificationChannelsForPackage(String pkg, int uid, Object slice) {
        createNotificationChannels(pkg, slice);
    }

    public void createNotificationChannelGroups(String pkg, Object slice) {
        synchronized (channels) {
            for (NotificationChannelGroup g : NotificationManagerImpl.<NotificationChannelGroup>listOf(slice)) {
                groups.put(g.getId(), g);
            }
        }
    }

    public NotificationChannel getNotificationChannel(String callingPkg, int userId, String pkg, String id) {
        synchronized (channels) {
            NotificationChannel c = channels.get(id);
            if (c == null && NotificationChannel.DEFAULT_CHANNEL_ID.equals(id)) {
                c = new NotificationChannel(id, "Miscellaneous", 3);
                channels.put(id, c);
            }
            return c;
        }
    }

    public NotificationChannel getNotificationChannelForPackage(String pkg, int uid, String id, String conv,
            boolean includeDeleted) {
        return getNotificationChannel(pkg, 0, pkg, id);
    }

    public NotificationChannel getConversationNotificationChannel(String callingPkg, int userId, String pkg,
            String id, boolean returnParent, String conversationId) {
        return getNotificationChannel(callingPkg, userId, pkg, id);
    }

    public Object getNotificationChannels(String callingPkg, String pkg, int userId) {
        synchronized (channels) { return slice(new ArrayList<>(channels.values())); }
    }

    public Object getNotificationChannelsForPackage(String pkg, int uid, boolean includeDeleted) {
        return getNotificationChannels(pkg, pkg, 0);
    }

    public int getNumNotificationChannelsForPackage(String pkg, int uid, boolean includeDeleted) {
        synchronized (channels) { return channels.size(); }
    }

    public void deleteNotificationChannel(String pkg, String id) {
        synchronized (channels) { channels.remove(id); }
    }

    public void updateNotificationChannelForPackage(String pkg, int uid, NotificationChannel c) {
        synchronized (channels) { channels.put(c.getId(), c); }
    }

    public NotificationChannelGroup getNotificationChannelGroup(String pkg, String id) {
        synchronized (channels) { return groups.get(id); }
    }

    public Object getNotificationChannelGroups(String pkg) {
        synchronized (channels) {
            List<NotificationChannelGroup> out = new ArrayList<>();
            for (NotificationChannelGroup g : groups.values()) {
                NotificationChannelGroup copy = g.clone();
                for (NotificationChannel c : channels.values()) {
                    if (g.getId().equals(c.getGroup())) Hidden.call(copy, "addChannel", c);
                }
                out.add(copy);
            }
            return slice(out);
        }
    }

    public void deleteNotificationChannelGroup(String pkg, String id) {
        synchronized (channels) {
            groups.remove(id);
            channels.values().removeIf(c -> id.equals(c.getGroup()));
        }
    }

    public boolean onlyHasDefaultChannel(String pkg, int uid) { return false; }

    // ---- notifications (logged, not shown yet) --------------------------------------------
    public void enqueueNotificationWithTag(String pkg, String opPkg, String tag, int id, Notification n, int userId) {
        CharSequence title = n.extras != null ? n.extras.getCharSequence(Notification.EXTRA_TITLE) : null;
        CharSequence text = n.extras != null ? n.extras.getCharSequence(Notification.EXTRA_TEXT) : null;
        Log.i(TAG, "notification " + id + " [" + n.getChannelId() + "]: " + title + (text != null ? " — " + text : ""));
    }
}
