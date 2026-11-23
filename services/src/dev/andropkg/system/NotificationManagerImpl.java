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
 * Notification channels and groups are kept (in memory) so apps can read them back.
 * Notifications become the host's notifications (runtime/media/notifications.mm): title and
 * text, a sound unless the channel is quiet; tapping one sends its contentIntent. Ongoing,
 * media and group-summary notifications are not shown (a player's is the lock screen's Now
 * Playing).
 */
final class NotificationManagerImpl {
    private static final String TAG = "andropkg";
    private Handler mainHandler;  // created on first use: services start before the main Looper
    private final Map<IBinder, View> shown = new HashMap<>();

    private synchronized Handler main() {
        if (mainHandler == null) mainHandler = new Handler(Looper.getMainLooper());
        return mainHandler;
    }

    // ---- notifications (host: runtime/media/notifications.mm) ----

    static native void nativePost(String key, String title, String text, boolean silent);
    static native void nativeCancel(String key);
    static NotificationManagerImpl instance;
    private final Map<String, android.app.PendingIntent> contentIntents = new HashMap<>();
    private final Map<String, Notification> active = new LinkedHashMap<>();

    NotificationManagerImpl() {
        instance = this;
    }

    private static String key(String pkg, String tag, int id) {
        return pkg + ":" + (tag == null ? "" : tag) + ":" + id;
    }

    // enqueueNotificationWithTag(String pkg, String opPkg, String tag, int id, Notification n, int userId)
    public void enqueueNotificationWithTag(String pkg, String opPkg, String tag, int id, Notification n, int userId) {
        String key = key(pkg, tag, id);
        synchronized (this) {
            active.put(key, n);
            if (n.contentIntent != null) contentIntents.put(key, n.contentIntent);
        }
        android.os.Bundle x = n.extras;
        String template = x != null ? x.getString(Notification.EXTRA_TEMPLATE) : null;
        boolean media = template != null && template.contains("MediaStyle");
        boolean ongoing = (n.flags & (Notification.FLAG_ONGOING_EVENT | Notification.FLAG_FOREGROUND_SERVICE)) != 0;
        boolean summary = (n.flags & Notification.FLAG_GROUP_SUMMARY) != 0;
        if (media || ongoing || summary) return;
        CharSequence title = x != null ? x.getCharSequence(Notification.EXTRA_TITLE) : null;
        CharSequence text = x != null ? x.getCharSequence(Notification.EXTRA_BIG_TEXT) : null;
        if (text == null && x != null) text = x.getCharSequence(Notification.EXTRA_TEXT);
        if (title == null && text == null) return;
        Log.i(TAG, "notification " + id + " [" + n.getChannelId() + "]: " + title);
        NotificationChannel channel = n.getChannelId() != null ? channels.get(n.getChannelId()) : null;
        boolean silent = (n.flags & Notification.FLAG_ONLY_ALERT_ONCE) != 0 && isUpdate(key)
                || channel != null && channel.getImportance() < 3 /* IMPORTANCE_DEFAULT */;
        try {
            nativePost(key, title != null ? title.toString() : "", text != null ? text.toString() : "", silent);
        } catch (UnsatisfiedLinkError e) {
            Log.i(TAG, "notification (no host yet): " + title + ": " + text);
        }
    }

    private final java.util.Set<String> posted = new java.util.HashSet<>();

    private synchronized boolean isUpdate(String key) {
        return !posted.add(key);
    }

    // cancelNotificationWithTag(String pkg, String opPkg, String tag, int id, int userId)
    public void cancelNotificationWithTag(String pkg, String opPkg, String tag, int id, int userId) {
        String key = key(pkg, tag, id);
        synchronized (this) {
            active.remove(key);
            contentIntents.remove(key);
            posted.remove(key);
        }
        try {
            nativeCancel(key);
        } catch (UnsatisfiedLinkError e) {
            // no host notifications yet
        }
    }

    public void cancelAllNotifications(String pkg, int userId) {
        List<String> keys;
        synchronized (this) {
            keys = new ArrayList<>(active.keySet());
        }
        for (String k : keys) {
            String[] p = k.split(":", 3);
            cancelNotificationWithTag(p[0], p[0], p[1].isEmpty() ? null : p[1], Integer.parseInt(p[2]), userId);
        }
    }

    /** The user tapped a notification (host): the app's contentIntent. */
    static void hostOpened(String key) {
        NotificationManagerImpl m = instance;
        if (m == null) return;
        android.app.PendingIntent pi;
        Notification n;
        synchronized (m) {
            pi = m.contentIntents.get(key);
            n = m.active.get(key);
        }
        if (pi == null) return;
        try {
            pi.send();
        } catch (android.app.PendingIntent.CanceledException e) {
            Log.w(TAG, "notification intent cancelled: " + key);
        }
        if (n != null && (n.flags & Notification.FLAG_AUTO_CANCEL) != 0) {
            String[] p = key.split(":", 3);
            m.cancelNotificationWithTag(p[0], p[0], p[1].isEmpty() ? null : p[1], Integer.parseInt(p[2]), 0);
        }
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

}
