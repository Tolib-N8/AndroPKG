package dev.andropkg.system;

import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.util.Log;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.HashSet;
import java.util.Set;

/**
 * An in-process system service. There is no binder driver: the IBinder handed to the framework
 * answers queryLocalInterface() with a dynamic proxy of the AIDL interface, so callers talk to
 * the implementation directly (exactly what happens for same-process binder calls on Android).
 *
 * Methods the implementation defines (same name and arity) are called on it; everything else
 * returns a neutral default and is logged once, which shows what an app actually needs.
 */
final class LocalService {
    private static final String TAG = "andropkg-svc";

    private LocalService() {}

    static final class LocalBinder extends Binder {
        final String descriptor;
        IInterface iface;

        LocalBinder(String descriptor) {
            this.descriptor = descriptor;
        }

        @Override
        public IInterface queryLocalInterface(String d) {
            return descriptor.equals(d) ? iface : null;
        }

        @Override
        public String getInterfaceDescriptor() {
            return descriptor;
        }
    }

    /** Returns the IBinder for `aidlInterface` backed by `impl`. */
    static LocalBinder create(String aidlInterface, Object impl) {
        Class<?> iface = Hidden.cls(aidlInterface);
        LocalBinder binder = new LocalBinder(aidlInterface);
        Set<String> reported = new HashSet<>();
        String shortName = aidlInterface.substring(aidlInterface.lastIndexOf('.') + 1);
        binder.iface = (IInterface) Proxy.newProxyInstance(iface.getClassLoader(), new Class<?>[] {iface},
                (proxy, method, args) -> {
                    String name = method.getName();
                    int arity = args == null ? 0 : args.length;
                    if (name.equals("asBinder") && arity == 0) return binder;
                    if (method.getDeclaringClass() == Object.class) {
                        if (name.equals("hashCode")) return System.identityHashCode(proxy);
                        if (name.equals("equals")) return proxy == args[0];
                        if (name.equals("toString")) return "andropkg:" + shortName;
                    }
                    Method m = find(impl.getClass(), name, arity);
                    if (m != null) return Hidden.invoke(m, impl, args == null ? new Object[0] : args);
                    // An implementation can take any signature of a method as (Object[] args).
                    Method any = findVarargs(impl.getClass(), name);
                    if (any != null) return Hidden.invoke(any, impl, (Object) (args == null ? new Object[0] : args));
                    if (reported.add(name + "/" + arity)) Log.w(TAG, shortName + "." + name + "() -> default");
                    replyToResultReceivers(args);
                    return Hidden.defaultValue(method.getReturnType());
                });
        return binder;
    }

    /**
     * Callers often block until the service answers through an IResultReceiver (e.g.
     * SyncResultReceiver with a 5 s timeout). An unimplemented method answers "no result".
     */
    private static void replyToResultReceivers(Object[] args) {
        if (args == null) return;
        for (Object a : args) {
            if (a != null && RESULT_RECEIVER != null && RESULT_RECEIVER.isInstance(a)) {
                try {
                    Hidden.call(a, "send", 0, null);
                } catch (RuntimeException e) {
                    Log.w(TAG, "IResultReceiver.send failed: " + e);
                }
            }
        }
    }

    private static final Class<?> RESULT_RECEIVER = optionalClass("com.android.internal.os.IResultReceiver");

    private static Class<?> optionalClass(String name) {
        try {
            return Class.forName(name);
        } catch (ClassNotFoundException e) {
            return null;
        }
    }

    private static Method findVarargs(Class<?> c, String name) {
        for (Method m : c.getDeclaredMethods()) {
            Class<?>[] p = m.getParameterTypes();
            if (m.getName().equals(name) && p.length == 1 && p[0] == Object[].class) {
                m.setAccessible(true);
                return m;
            }
        }
        return null;
    }

    private static Method find(Class<?> c, String name, int arity) {
        for (Method m : c.getDeclaredMethods()) {
            if (m.getName().equals(name) && m.getParameterCount() == arity) {
                m.setAccessible(true);
                return m;
            }
        }
        return null;
    }
}
