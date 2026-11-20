package dev.andropkg.system;

import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.IBinder;
import android.util.Log;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;

/**
 * The app's Services, driven as ActivityManagerService's ActiveServices drives them:
 * scheduleCreateService / scheduleServiceArgs / scheduleBindService / scheduleUnbindService /
 * scheduleStopService on the app thread, publishService -> IServiceConnection.connected.
 * A service lives while it is started or has auto-create bindings.
 */
final class ActiveServices {
    private static final String TAG = "andropkg";
    private static final int BIND_AUTO_CREATE = 1;

    private static final class Binding {
        final Intent intent;
        final List<Object> connections = new ArrayList<>();  // IServiceConnection
        final List<Boolean> autoCreate = new ArrayList<>();
        IBinder binder;
        boolean requested, published;

        Binding(Intent intent) {
            this.intent = intent;
        }
    }

    private static final class Record {
        final ServiceInfo info;
        final ComponentName name;
        final IBinder token = new Binder();
        final Map<Intent.FilterComparison, Binding> bindings = new HashMap<>();
        boolean created, started;
        int lastStartId;

        Record(ServiceInfo info) {
            this.info = info;
            this.name = new ComponentName(info.packageName, info.name);
        }
    }

    private final Map<ComponentName, Record> services = new HashMap<>();
    private final Map<IBinder, Record> byConnection = new HashMap<>();

    /** The app's service for `intent` (explicit, or by intent filter), or null. */
    private static ServiceInfo resolve(Intent intent) {
        ComponentName c = intent.getComponent();
        String pkg = AndroPkgSystem.appInfo.packageName;
        if (c != null) {
            if (!pkg.equals(c.getPackageName())) return null;
            for (ServiceInfo si : AndroPkgSystem.<ServiceInfo>infos("services")) {
                if (si.name.equals(c.getClassName())) return si;
            }
            return null;
        }
        if (intent.getPackage() != null && !pkg.equals(intent.getPackage())) return null;
        for (Object s : AndroPkgSystem.components("services")) {
            @SuppressWarnings("unchecked")
            List<Object> filters = (List<Object>) Hidden.getField(s, "intents");
            for (Object f : filters) {
                if (((IntentFilter) f).match(intent.getAction(), intent.getType(), intent.getScheme(), intent.getData(),
                        intent.getCategories(), TAG) >= 0) {
                    ServiceInfo si = (ServiceInfo) Hidden.getField(s, "info");
                    si.applicationInfo = AndroPkgSystem.appInfo;
                    return si;
                }
            }
        }
        return null;
    }

    private Record record(Intent intent, boolean create) {
        ServiceInfo si = resolve(intent);
        if (si == null) return null;
        ComponentName name = new ComponentName(si.packageName, si.name);
        Record r = services.get(name);
        if (r == null && create) {
            r = new Record(si);
            services.put(name, r);
        }
        return r;
    }

    private Record byToken(IBinder token) {
        for (Record r : services.values()) {
            if (r.token == token) return r;
        }
        return null;
    }

    private static Object compat() {
        return Hidden.getField(Hidden.cls("android.content.res.CompatibilityInfo"), "DEFAULT_COMPATIBILITY_INFO");
    }

    private void ensureCreated(Record r) {
        if (r.created) return;
        r.created = true;
        Log.i(TAG, "service create " + r.name.getClassName());
        Hidden.call(AndroPkgSystem.appThread, "scheduleCreateService", r.token, r.info, compat(), 2 /* TOP */);
        for (Binding b : r.bindings.values()) requestBind(r, b);
    }

    private void requestBind(Record r, Binding b) {
        if (!r.created || b.requested || b.connections.isEmpty()) return;
        b.requested = true;
        Hidden.call(AndroPkgSystem.appThread, "scheduleBindService", r.token, b.intent, false, 2, 0L);
    }

    private void maybeDestroy(Record r) {
        if (r.started) return;
        for (Binding b : r.bindings.values()) {
            if (b.autoCreate.contains(true)) return;
        }
        for (Binding b : r.bindings.values()) {
            if (b.requested) Hidden.call(AndroPkgSystem.appThread, "scheduleUnbindService", r.token, b.intent);
        }
        services.remove(r.name);
        if (r.created) {
            Log.i(TAG, "service destroy " + r.name.getClassName());
            Hidden.call(AndroPkgSystem.appThread, "scheduleStopService", r.token);
        }
    }

    synchronized ComponentName start(Intent intent) {
        Record r = record(intent, true);
        if (r == null) {
            Log.w(TAG, "startService: no service for " + intent);
            return null;
        }
        ensureCreated(r);
        r.started = true;
        Object args = Hidden.newInstance(Hidden.cls("android.app.ServiceStartArgs"), false, ++r.lastStartId, 0, intent);
        List<Object> list = new ArrayList<>();
        list.add(args);
        Hidden.call(AndroPkgSystem.appThread, "scheduleServiceArgs", r.token,
                Hidden.newInstance(Hidden.cls("android.content.pm.ParceledListSlice"), list));
        return r.name;
    }

    synchronized int stop(Intent intent) {
        Record r = record(intent, false);
        if (r == null || !r.started) return 0;
        r.started = false;
        maybeDestroy(r);
        return 1;
    }

    synchronized boolean stopSelf(ComponentName name, IBinder token, int startId) {
        Record r = byToken(token);
        if (r == null || (startId >= 0 && startId != r.lastStartId)) return false;
        r.started = false;
        maybeDestroy(r);
        return true;
    }

    synchronized int bind(Intent intent, Object connection, long flags) {
        Record r = record(intent, true);
        if (r == null) {
            Log.w(TAG, "bindService: no service for " + intent);
            return 0;
        }
        Intent.FilterComparison key = new Intent.FilterComparison(new Intent(intent));
        Binding b = r.bindings.computeIfAbsent(key, k -> new Binding(k.getIntent()));
        b.connections.add(connection);
        b.autoCreate.add((flags & BIND_AUTO_CREATE) != 0);
        byConnection.put(((android.os.IInterface) connection).asBinder(), r);
        if ((flags & BIND_AUTO_CREATE) != 0) ensureCreated(r);
        if (b.published) {
            Hidden.call(connection, "connected", r.name, b.binder, false);
        } else {
            requestBind(r, b);
        }
        return 1;
    }

    synchronized boolean unbind(Object connection) {
        Record r = byConnection.remove(((android.os.IInterface) connection).asBinder());
        if (r == null) return false;
        for (Iterator<Binding> it = r.bindings.values().iterator(); it.hasNext(); ) {
            Binding b = it.next();
            int i = b.connections.indexOf(connection);
            if (i < 0) continue;
            b.connections.remove(i);
            b.autoCreate.remove(i);
            if (b.connections.isEmpty()) {
                if (b.requested) Hidden.call(AndroPkgSystem.appThread, "scheduleUnbindService", r.token, b.intent);
                it.remove();
            }
        }
        maybeDestroy(r);
        return true;
    }

    synchronized void publish(IBinder token, Intent intent, IBinder service) {
        Record r = byToken(token);
        if (r == null) return;
        Binding b = r.bindings.get(new Intent.FilterComparison(intent));
        if (b == null) return;
        b.binder = service;
        b.published = true;
        for (Object c : b.connections) Hidden.call(c, "connected", r.name, service, false);
    }

    synchronized IBinder peek(Intent intent) {
        Record r = record(intent, false);
        if (r == null) return null;
        Binding b = r.bindings.get(new Intent.FilterComparison(intent));
        return b != null ? b.binder : null;
    }
}
