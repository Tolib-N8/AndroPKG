package dev.andropkg.system;

import java.lang.reflect.Array;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Reflection helpers for the framework's hidden (@hide) APIs. andropkg compiles against the
 * public SDK only, so hidden classes/methods are reached by name. Methods are matched by name
 * and arity, which keeps this code tolerant of small signature changes between releases.
 */
final class Hidden {
    private Hidden() {}

    static Class<?> cls(String name) {
        try {
            return Class.forName(name);
        } catch (ClassNotFoundException e) {
            throw new IllegalStateException("andropkg: missing framework class " + name, e);
        }
    }

    private static final java.util.concurrent.ConcurrentHashMap<String, Method> METHODS =
            new java.util.concurrent.ConcurrentHashMap<>();

    static Method method(Class<?> c, String name, int arity) {
        String key = c.getName() + "#" + name + "/" + arity;
        Method cached = METHODS.get(key);
        if (cached != null) return cached;
        for (Class<?> k = c; k != null; k = k.getSuperclass()) {
            for (Method m : k.getDeclaredMethods()) {
                if (m.getName().equals(name) && (arity < 0 || m.getParameterCount() == arity)) {
                    m.setAccessible(true);
                    METHODS.put(key, m);
                    return m;
                }
            }
        }
        throw new IllegalStateException("andropkg: no method " + c.getName() + "." + name + "/" + arity);
    }

    static Object call(Object target, String name, Object... args) {
        return invoke(method(target.getClass(), name, args.length), target, args);
    }

    static Object callStatic(Class<?> c, String name, Object... args) {
        return invoke(method(c, name, args.length), null, args);
    }

    static Object invoke(Method m, Object target, Object... args) {
        try {
            return m.invoke(target, args);
        } catch (java.lang.reflect.InvocationTargetException e) {
            Throwable t = e.getCause();
            if (t instanceof RuntimeException) throw (RuntimeException) t;
            if (t instanceof Error) throw (Error) t;
            throw new RuntimeException(t);
        } catch (IllegalAccessException e) {
            throw new RuntimeException(e);
        }
    }

    static Object newInstance(Class<?> c, Object... args) {
        for (Constructor<?> k : c.getDeclaredConstructors()) {
            if (k.getParameterCount() == args.length) {
                try {
                    k.setAccessible(true);
                    return k.newInstance(args);
                } catch (ReflectiveOperationException e) {
                    throw new RuntimeException(e);
                }
            }
        }
        throw new IllegalStateException("andropkg: no constructor " + c.getName() + "/" + args.length);
    }

    static Object getField(Object target, String name) {
        try {
            Field f = field(target instanceof Class ? (Class<?>) target : target.getClass(), name);
            return f.get(target instanceof Class ? null : target);
        } catch (IllegalAccessException e) {
            throw new RuntimeException(e);
        }
    }

    static void setField(Object target, String name, Object value) {
        try {
            field(target.getClass(), name).set(target, value);
        } catch (IllegalAccessException e) {
            throw new RuntimeException(e);
        }
    }

    static void setStaticField(Class<?> c, String name, Object value) {
        try {
            field(c, name).set(null, value);
        } catch (IllegalAccessException e) {
            throw new RuntimeException(e);
        }
    }

    private static final java.util.concurrent.ConcurrentHashMap<String, Field> FIELDS =
            new java.util.concurrent.ConcurrentHashMap<>();

    // Cached, and found by scanning declared fields: getDeclaredField() throws for every class
    // up the hierarchy that lacks it, and exceptions are slow in the interpreter (thousands of
    // component lookups while parsing a large app's manifest).
    private static Field field(Class<?> c, String name) {
        String key = c.getName() + "#" + name;
        Field cached = FIELDS.get(key);
        if (cached != null) return cached;
        for (Class<?> k = c; k != null; k = k.getSuperclass()) {
            for (Field f : k.getDeclaredFields()) {
                if (f.getName().equals(name)) {
                    f.setAccessible(true);
                    FIELDS.put(key, f);
                    return f;
                }
            }
        }
        throw new IllegalStateException("andropkg: no field " + c.getName() + "." + name);
    }

    /** Neutral value for an unimplemented service method's return type. */
    static Object defaultValue(Class<?> type) {
        if (type == void.class) return null;
        if (type == boolean.class) return false;
        if (type == int.class) return 0;
        if (type == long.class) return 0L;
        if (type == float.class) return 0f;
        if (type == double.class) return 0d;
        if (type == byte.class) return (byte) 0;
        if (type == short.class) return (short) 0;
        if (type == char.class) return (char) 0;
        if (type.isArray()) return Array.newInstance(type.getComponentType(), 0);
        if (type == List.class) return new ArrayList<>();
        if (type == Map.class) return new HashMap<>();
        if (type.getName().equals("android.content.pm.ParceledListSlice")) return callStatic(type, "emptyList");
        return null;
    }
}
