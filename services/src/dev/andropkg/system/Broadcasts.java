package dev.andropkg.system;

import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Process;
import android.util.Log;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Broadcasts within the app, as ActivityManagerService's BroadcastQueue delivers them:
 * registered receivers (IIntentReceiver.performReceive) and the app's manifest receivers
 * (IApplicationThread.scheduleReceiver), asynchronously on the main thread. Sticky
 * broadcasts are kept and handed to matching registrations.
 */
final class Broadcasts {
    private static final String TAG = "andropkg";

    private static final class Registration {
        final Object receiver;  // IIntentReceiver
        final IntentFilter filter;

        Registration(Object receiver, IntentFilter filter) {
            this.receiver = receiver;
            this.filter = filter;
        }
    }

    private final List<Registration> registrations = new ArrayList<>();
    private final Map<String, Intent> sticky = new HashMap<>();
    private Handler mainHandler;

    private synchronized Handler main() {
        if (mainHandler == null) mainHandler = new Handler(Looper.getMainLooper());
        return mainHandler;
    }

    private static boolean matches(IntentFilter f, Intent i) {
        return f.match(i.getAction(), i.getType(), i.getScheme(), i.getData(), i.getCategories(), TAG) >= 0;
    }

    private static IBinder binder(Object receiver) {
        return ((android.os.IInterface) receiver).asBinder();
    }

    /** registerReceiver: returns the matching sticky broadcast, if any. */
    synchronized Intent register(Object receiver, IntentFilter filter) {
        Intent stickyMatch = null;
        for (Intent i : sticky.values()) {
            if (matches(filter, i)) stickyMatch = i;
        }
        if (receiver != null) registrations.add(new Registration(receiver, filter));
        return stickyMatch;
    }

    synchronized void unregister(Object receiver) {
        IBinder b = binder(receiver);
        registrations.removeIf(r -> binder(r.receiver) == b);
    }

    /** Delivers `intent` to the app's receivers; then to `resultTo`, if given. */
    int send(Intent intent, boolean isSticky, Object resultTo) {
        final Intent in = new Intent(intent);
        List<Object> dynamic = new ArrayList<>();
        synchronized (this) {
            if (isSticky && in.getAction() != null) sticky.put(in.getAction(), in);
            if (in.getComponent() == null) {
                for (Registration r : registrations) {
                    if (matches(r.filter, in)) dynamic.add(r.receiver);
                }
            }
        }
        List<ActivityInfo> manifest = manifestReceivers(in);
        if (dynamic.isEmpty() && manifest.isEmpty() && resultTo == null) return 0;
        Log.i(TAG, "broadcast " + in.getAction() + " -> " + (dynamic.size() + manifest.size()) + " receiver(s)");
        main().post(() -> {
            for (Object r : dynamic) {
                Hidden.call(r, "performReceive", new Intent(in), 0, null, null, false, isSticky, 0);
            }
            Object compat = Hidden.getField(Hidden.cls("android.content.res.CompatibilityInfo"),
                    "DEFAULT_COMPATIBILITY_INFO");
            for (ActivityInfo info : manifest) {
                Intent i = new Intent(in).setComponent(new ComponentName(info.packageName, info.name));
                Hidden.call(AndroPkgSystem.appThread, "scheduleReceiver", i, info, compat, 0, null, null,
                        false /* ordered */, true /* assumeDelivered */, 0, 2 /* TOP */, Process.myUid(),
                        AndroPkgSystem.appInfo.packageName);
            }
            if (resultTo != null) Hidden.call(resultTo, "performReceive", new Intent(in), 0, null, null, false, false, 0);
        });
        return 0;  // ActivityManager.BROADCAST_SUCCESS
    }

    /** Manifest receivers: explicit, or implicit broadcasts aimed at the app's package. */
    private static List<ActivityInfo> manifestReceivers(Intent intent) {
        List<ActivityInfo> out = new ArrayList<>();
        String pkg = AndroPkgSystem.appInfo.packageName;
        ComponentName c = intent.getComponent();
        for (Object r : AndroPkgSystem.components("receivers")) {
            ActivityInfo info = (ActivityInfo) Hidden.getField(r, "info");
            info.applicationInfo = AndroPkgSystem.appInfo;
            if (c != null) {
                if (pkg.equals(c.getPackageName()) && info.name.equals(c.getClassName())) out.add(info);
                continue;
            }
            if (!pkg.equals(intent.getPackage())) continue;
            @SuppressWarnings("unchecked")
            List<Object> filters = (List<Object>) Hidden.getField(r, "intents");
            for (Object f : filters) {
                if (matches((IntentFilter) f, intent)) {
                    out.add(info);
                    break;
                }
            }
        }
        return out;
    }
}
