package dev.andropkg.system;

import android.content.Context;
import android.content.Intent;
import android.content.IntentSender;
import android.content.pm.PackageInstaller;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * android.content.pm.IPackageInstaller: app stores (F-Droid, RuStore, ...) install apps with
 * PackageInstaller sessions. A session's APKs are written through FileBridge, as
 * PackageInstallerService has them written; commit() puts them where the host's launcher looks
 * for installed apps (host-app Launcher.mm: Application Support/andropkg/apps), as
 * <package>.apk or, a split install, <package>/base.apk + splits, and reports success to the
 * store. The app then appears in AndroPkg's list (one with native code needs it converted).
 *
 * AIDL methods are matched by name with any arguments (LocalService's Object[] form).
 */
final class PackageInstallerImpl {
    private static final String TAG = "andropkg-installer";

    private final class Session {
        final int id;
        final PackageInstaller.SessionParams params;
        final String installer;
        final File dir;
        final Map<String, File> files = new LinkedHashMap<>();
        float progress;
        boolean sealed, done;

        Session(int id, PackageInstaller.SessionParams params, String installer) {
            this.id = id;
            this.params = params;
            this.installer = installer;
            this.dir = new File(stagingRoot(), "session_" + id);
            dir.mkdirs();
        }

        String packageName() {
            Object p = params != null ? field(params, "appPackageName") : null;
            return p instanceof String ? (String) p : null;
        }
    }

    private final Map<Integer, Session> sessions = new HashMap<>();
    private final List<Object> callbacks = new ArrayList<>();  // IPackageInstallerCallback
    private int nextId = 1;

    private static Object field(Object o, String name) {
        try {
            return Hidden.getField(o, name);
        } catch (RuntimeException e) {
            return null;
        }
    }

    private static <T> T find(Object[] args, Class<T> type) {
        for (Object a : args) {
            if (type.isInstance(a)) return type.cast(a);
        }
        return null;
    }

    private static int intArg(Object[] args, int which) {
        for (Object a : args) {
            if (a instanceof Integer && which-- == 0) return (Integer) a;
        }
        return -1;
    }

    private static String stringArg(Object[] args, int which) {
        for (Object a : args) {
            if (a instanceof String && which-- == 0) return (String) a;
        }
        return null;
    }

    /** Application Support/andropkg (the host) from the app's data directory, .../andropkg/data/app/<name>. */
    private static File hostRoot() {
        File data = new File(AndroPkgSystem.hostDataDir);
        File root = data.getParentFile() != null ? data.getParentFile().getParentFile() : null;
        return root != null && root.getParentFile() != null ? root.getParentFile() : data;
    }

    private static File stagingRoot() {
        File d = new File(hostRoot(), "installer");
        d.mkdirs();
        return d;
    }

    /** Where the launcher lists installed apps. */
    static File appsDir() {
        File d = new File(hostRoot(), "apps");
        d.mkdirs();
        return d;
    }

    private synchronized Session session(int id) {
        return sessions.get(id);
    }

    private void notifyCallbacks(String method, Object... args) {
        List<Object> cbs;
        synchronized (this) {
            cbs = new ArrayList<>(callbacks);
        }
        for (Object cb : cbs) {
            try {
                Hidden.call(cb, method, args);
            } catch (RuntimeException e) {
                Log.w(TAG, method + " callback", e);
            }
        }
    }

    // ---- IPackageInstaller ----

    public int createSession(Object[] args) {
        PackageInstaller.SessionParams params = find(args, PackageInstaller.SessionParams.class);
        String installer = stringArg(args, 0);
        Session s;
        synchronized (this) {
            s = new Session(nextId++, params, installer);
            sessions.put(s.id, s);
        }
        Log.i(TAG, "session " + s.id + " created" + (s.packageName() != null ? " for " + s.packageName() : ""));
        notifyCallbacks("onSessionCreated", s.id);
        return s.id;
    }

    public Object openSession(Object[] args) {
        Session s = session(intArg(args, 0));
        if (s == null) throw new SecurityException("no session " + intArg(args, 0));
        return LocalService.create("android.content.pm.IPackageInstallerSession", new SessionImpl(s)).iface;
    }

    public PackageInstaller.SessionInfo getSessionInfo(Object[] args) {
        Session s = session(intArg(args, 0));
        return s != null ? info(s) : null;
    }

    public Object getAllSessions(Object[] args) {
        return slice(infos(null));
    }

    public Object getMySessions(Object[] args) {
        return slice(infos(stringArg(args, 0)));
    }

    public Object getStagedSessions(Object[] args) {
        return slice(new ArrayList<>());
    }

    public void abandonSession(Object[] args) {
        Session s;
        synchronized (this) {
            s = sessions.remove(intArg(args, 0));
        }
        if (s != null) abandon(s);
    }

    public synchronized void registerCallback(Object[] args) {
        if (args.length > 0 && args[0] != null) callbacks.add(args[0]);
    }

    public synchronized void unregisterCallback(Object[] args) {
        if (args.length > 0) callbacks.remove(args[0]);
    }

    public void updateSessionAppIcon(Object[] args) {}

    public void updateSessionAppLabel(Object[] args) {}

    public void setPermissionsResult(Object[] args) {}

    /** uninstall(VersionedPackage, callerPackageName, flags, statusReceiver, userId): removes it from the launcher. */
    public void uninstall(Object[] args) {
        android.content.pm.VersionedPackage vp = find(args, android.content.pm.VersionedPackage.class);
        String pkg = vp != null ? vp.getPackageName() : null;
        boolean removed = false;
        if (pkg != null) {
            removed = new File(appsDir(), pkg + ".apk").delete();
            File dir = new File(appsDir(), pkg);
            if (dir.isDirectory()) {
                File[] fs = dir.listFiles();
                if (fs != null) for (File f : fs) f.delete();
                removed |= dir.delete();
            }
        }
        if (removed) {
            installerFile(pkg).delete();
            broadcast(Intent.ACTION_PACKAGE_REMOVED, pkg);
        }
        report(find(args, IntentSender.class), removed ? PackageInstaller.STATUS_SUCCESS : PackageInstaller.STATUS_FAILURE,
                pkg, -1, removed ? null : "not installed");
    }

    private List<PackageInstaller.SessionInfo> infos(String installer) {
        List<PackageInstaller.SessionInfo> l = new ArrayList<>();
        synchronized (this) {
            for (Session s : sessions.values()) {
                if (installer == null || installer.equals(s.installer)) l.add(info(s));
            }
        }
        return l;
    }

    private static Object slice(List<?> list) {
        return Hidden.newInstance(Hidden.cls("android.content.pm.ParceledListSlice"), list);
    }

    private PackageInstaller.SessionInfo info(Session s) {
        PackageInstaller.SessionInfo i = (PackageInstaller.SessionInfo) Hidden.newInstance(PackageInstaller.SessionInfo.class);
        Hidden.setField(i, "sessionId", s.id);
        Hidden.setField(i, "installerPackageName", s.installer);
        Hidden.setField(i, "appPackageName", s.packageName());
        Hidden.setField(i, "progress", s.progress);
        Hidden.setField(i, "sealed", s.sealed);
        Hidden.setField(i, "active", !s.done);
        if (s.params != null) {
            Object mode = field(s.params, "mode");
            if (mode instanceof Integer) Hidden.setField(i, "mode", mode);
            Object size = field(s.params, "sizeBytes");
            if (size instanceof Long) Hidden.setField(i, "sizeBytes", size);
            Object label = field(s.params, "appLabel");
            if (label instanceof String) Hidden.setField(i, "appLabel", label);
        }
        return i;
    }

    private void abandon(Session s) {
        s.done = true;
        File[] fs = s.dir.listFiles();
        if (fs != null) for (File f : fs) f.delete();
        s.dir.delete();
        notifyCallbacks("onSessionFinished", s.id, false);
    }

    /** Sends the store its result (PackageInstaller.EXTRA_STATUS, ...) through its IntentSender. */
    private static void report(IntentSender receiver, int status, String pkg, int sessionId, String message) {
        if (receiver == null) return;
        Intent fill = new Intent();
        fill.putExtra(PackageInstaller.EXTRA_STATUS, status);
        if (pkg != null) fill.putExtra(PackageInstaller.EXTRA_PACKAGE_NAME, pkg);
        if (sessionId >= 0) fill.putExtra(PackageInstaller.EXTRA_SESSION_ID, sessionId);
        if (message != null) fill.putExtra(PackageInstaller.EXTRA_STATUS_MESSAGE, message);
        try {
            Context app = (Context) Hidden.callStatic(Hidden.cls("android.app.ActivityThread"), "currentApplication");
            receiver.sendIntent(app, 0, fill, null, null);
        } catch (Exception e) {
            Log.w(TAG, "status receiver", e);
        }
    }

    // ---- IPackageInstallerSession ----

    final class SessionImpl {
        private final Session s;

        SessionImpl(Session s) {
            this.s = s;
        }

        private File file(String name) {
            File f = new File(s.dir, new File(name).getName());  // (a bare name: no paths out of the session)
            synchronized (s) {
                s.files.put(new File(name).getName(), f);
            }
            return f;
        }

        public void setClientProgress(Object[] a) {
            if (a.length > 0 && a[0] instanceof Float) s.progress = (Float) a[0];
            notifyCallbacks("onSessionProgressChanged", s.id, s.progress);
        }

        public void addClientProgress(Object[] a) {
            if (a.length > 0 && a[0] instanceof Float) s.progress += (Float) a[0];
            notifyCallbacks("onSessionProgressChanged", s.id, s.progress);
        }

        public String[] getNames(Object[] a) {
            synchronized (s) {
                return s.files.keySet().toArray(new String[0]);
            }
        }

        /** The client writes through a FileBridge (PackageInstaller.Session.openWrite). */
        public ParcelFileDescriptor openWrite(Object[] a) throws Exception {
            String name = stringArg(a, 0);
            long offset = a.length > 1 && a[1] instanceof Long ? (Long) a[1] : 0;
            File f = file(name);
            ParcelFileDescriptor target = ParcelFileDescriptor.open(f,
                    ParcelFileDescriptor.MODE_WRITE_ONLY | ParcelFileDescriptor.MODE_CREATE);
            if (offset > 0) {
                android.system.Os.lseek(target.getFileDescriptor(), offset, android.system.OsConstants.SEEK_SET);
            }
            if (SystemPropertiesRevocable()) return target;
            // The client writes the FileBridge protocol (PackageInstaller.Session.openWrite).
            Object bridge = Hidden.newInstance(Hidden.cls("android.os.FileBridge"));
            Hidden.call(bridge, "setTargetFile", target);
            Hidden.call(bridge, "start");
            return (ParcelFileDescriptor) Hidden.call(bridge, "getClientSocket");
        }

        private boolean SystemPropertiesRevocable() {
            try {
                return (Boolean) Hidden.callStatic(Hidden.cls("android.os.SystemProperties"), "getBoolean",
                        "fw.revocable_fd", false);
            } catch (RuntimeException e) {
                return false;
            }
        }

        public ParcelFileDescriptor openRead(Object[] a) throws Exception {
            return ParcelFileDescriptor.open(file(stringArg(a, 0)), ParcelFileDescriptor.MODE_READ_ONLY);
        }

        /** write(name, offset, length, fd): the APK from a file descriptor the client gives. */
        public void write(Object[] a) throws Exception {
            String name = stringArg(a, 0);
            ParcelFileDescriptor in = find(a, ParcelFileDescriptor.class);
            if (in == null) return;
            try (InputStream is = new FileInputStream(in.getFileDescriptor());
                 OutputStream os = new FileOutputStream(file(name))) {
                byte[] b = new byte[1 << 16];
                for (int n; (n = is.read(b)) > 0; ) os.write(b, 0, n);
            }
        }

        public void removeSplit(Object[] a) {
            String name = stringArg(a, 0);
            synchronized (s) {
                File f = s.files.remove(name);
                if (f != null) f.delete();
            }
        }

        public void close(Object[] a) {}

        public void seal(Object[] a) {
            s.sealed = true;
        }

        public void abandon(Object[] a) {
            synchronized (PackageInstallerImpl.this) {
                sessions.remove(s.id);
            }
            PackageInstallerImpl.this.abandon(s);
        }

        public boolean isMultiPackage(Object[] a) {
            return false;
        }

        public int[] getChildSessionIds(Object[] a) {
            return new int[0];
        }

        public int getParentSessionId(Object[] a) {
            return -1;
        }

        public boolean isStaged(Object[] a) {
            return false;
        }

        public int getInstallFlags(Object[] a) {
            Object f = s.params != null ? field(s.params, "installFlags") : null;
            return f instanceof Integer ? (Integer) f : 0;
        }

        public boolean isApplicationEnabledSettingPersistent(Object[] a) {
            return false;
        }

        public boolean isRequestUpdateOwnership(Object[] a) {
            return false;
        }

        /** commit(statusReceiver, forTransferred): into the launcher's apps. */
        public void commit(Object[] a) {
            IntentSender receiver = find(a, IntentSender.class);
            s.sealed = true;
            String pkg = s.packageName();
            String error = null;
            List<File> apks = new ArrayList<>();
            synchronized (s) {
                for (File f : s.files.values()) if (f.length() > 0 && isApk(f)) apks.add(f);
            }
            if (apks.isEmpty()) error = "no APK written";
            if (error == null && pkg == null) pkg = packageOf(apks);
            if (error == null && pkg == null) error = "not an APK";
            if (error == null) {
                try {
                    install(pkg, apks);
                } catch (Exception e) {
                    error = e.toString();
                }
            }
            s.done = true;
            synchronized (PackageInstallerImpl.this) {
                sessions.remove(s.id);
            }
            File[] left = s.dir.listFiles();
            if (left != null) for (File f : left) f.delete();
            s.dir.delete();
            if (error == null) {
                Log.i(TAG, "installed " + pkg + " (" + apks.size() + " APK" + (apks.size() > 1 ? "s" : "") + ")");
                recordInstaller(pkg, s.installer);
                broadcast(Intent.ACTION_PACKAGE_ADDED, pkg);
            } else {
                Log.w(TAG, "session " + s.id + " failed: " + error);
            }
            notifyCallbacks("onSessionFinished", s.id, error == null);
            report(receiver, error == null ? PackageInstaller.STATUS_SUCCESS : PackageInstaller.STATUS_FAILURE_INVALID,
                    pkg, s.id, error);
        }
    }

    // ---- installed apps (what the launcher lists), for the package manager ----

    private static final Map<String, Object[]> infoCache = new HashMap<>();  // path -> {mtime, PackageInfo}

    /** The installed app's APK (<package>.apk, or <package>/base.apk), or null. */
    static File apkOf(String pkg) {
        if (pkg == null || pkg.contains("/")) return null;
        File single = new File(appsDir(), pkg + ".apk");
        if (single.isFile()) return single;
        File base = new File(new File(appsDir(), pkg), "base.apk");
        return base.isFile() ? base : null;
    }

    static android.content.pm.PackageInfo packageInfo(String pkg, long flags) {
        File apk = apkOf(pkg);
        return apk != null ? parse(apk, flags) : null;
    }

    static List<android.content.pm.PackageInfo> installed(long flags) {
        List<android.content.pm.PackageInfo> l = new ArrayList<>();
        File[] fs = appsDir().listFiles();
        if (fs == null) return l;
        for (File f : fs) {
            File apk = f.isDirectory() ? new File(f, "base.apk") : f.getName().endsWith(".apk") ? f : null;
            if (apk == null || !apk.isFile()) continue;
            android.content.pm.PackageInfo pi = parse(apk, flags);
            if (pi != null) l.add(pi);
        }
        return l;
    }

    private static android.content.pm.PackageInfo parse(File apk, long flags) {
        String key = apk.getPath() + "#" + flags;
        synchronized (infoCache) {
            Object[] c = infoCache.get(key);
            if (c != null && (Long) c[0] == apk.lastModified()) return (android.content.pm.PackageInfo) c[1];
        }
        android.content.pm.PackageInfo pi = null;
        try {
            Context app = (Context) Hidden.callStatic(Hidden.cls("android.app.ActivityThread"), "currentApplication");
            if (app != null) {
                pi = app.getPackageManager().getPackageArchiveInfo(apk.getPath(),
                        android.content.pm.PackageManager.PackageInfoFlags.of(flags & ~android.content.pm.PackageManager.GET_SIGNATURES));
            }
            if (pi != null && pi.applicationInfo != null) {
                pi.applicationInfo.sourceDir = pi.applicationInfo.publicSourceDir = apk.getPath();
                pi.applicationInfo.enabled = true;
                pi.firstInstallTime = pi.lastUpdateTime = apk.lastModified();
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "reading " + apk, e);
        }
        synchronized (infoCache) {
            infoCache.put(key, new Object[] {apk.lastModified(), pi});
        }
        return pi;
    }

    /** Which store installed an app (stores update only the apps they installed). */
    private static File installerFile(String pkg) {
        File d = new File(hostRoot(), "installer/by-package");
        d.mkdirs();
        return new File(d, pkg);
    }

    private static void recordInstaller(String pkg, String installer) {
        if (installer == null) return;
        try (OutputStream o = new FileOutputStream(installerFile(pkg))) {
            o.write(installer.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (Exception e) {
            Log.w(TAG, "installer of " + pkg, e);
        }
    }

    static String installerOf(String pkg) {
        File f = installerFile(pkg);
        if (pkg.contains("/") || !f.isFile()) return null;
        try {
            return new String(java.nio.file.Files.readAllBytes(f.toPath()), java.nio.charset.StandardCharsets.UTF_8);
        } catch (Exception e) {
            return null;
        }
    }

    /** android.content.pm.InstallSourceInfo: the installing package (null: sideloaded). */
    static Object installSource(String installer) {
        for (java.lang.reflect.Constructor<?> k : Hidden.cls("android.content.pm.InstallSourceInfo").getDeclaredConstructors()) {
            Class<?>[] p = k.getParameterTypes();
            if (p.length < 4 || p[0] != String.class) continue;
            Object[] a = new Object[p.length];
            for (int n = 0; n < p.length; n++) {
                if (p[n] == String.class) a[n] = installer;
                else if (p[n] == int.class) a[n] = installer != null ? 2 : 0;  // PACKAGE_SOURCE_STORE / UNSPECIFIED
            }
            try {
                k.setAccessible(true);
                return k.newInstance(a);
            } catch (Exception e) {
                Log.w(TAG, "InstallSourceInfo", e);
            }
        }
        return null;
    }

    private static final Map<String, Object[]> launcherCache = new HashMap<>();  // pkg -> {mtime, ActivityInfo}

    /** The installed app's launcher activity (MAIN + LAUNCHER), or null; stores list launchable apps. */
    static android.content.pm.ActivityInfo launcherActivity(String pkg) {
        File apk = apkOf(pkg);
        if (apk == null) return null;
        File target = apk.getName().equals("base.apk") ? apk.getParentFile() : apk;  // a split install: the cluster
        synchronized (launcherCache) {
            Object[] c = launcherCache.get(pkg);
            if (c != null && (Long) c[0] == apk.lastModified()) return (android.content.pm.ActivityInfo) c[1];
        }
        android.content.pm.ActivityInfo found = null;
        try {
            Object parser = Hidden.newInstance(Hidden.cls("android.content.pm.PackageParser"));
            Object p = Hidden.call(parser, "parsePackage", target, 0);
            Object ai = Hidden.getField(p, "applicationInfo");
            if (ai instanceof android.content.pm.ApplicationInfo) {
                ((android.content.pm.ApplicationInfo) ai).sourceDir = apk.getPath();
                ((android.content.pm.ApplicationInfo) ai).enabled = true;
            }
            for (Object a : (List<?>) Hidden.getField(p, "activities")) {
                for (Object filter : (List<?>) Hidden.getField(a, "intents")) {
                    android.content.IntentFilter f = (android.content.IntentFilter) filter;
                    if (f.hasAction(Intent.ACTION_MAIN) && f.hasCategory(Intent.CATEGORY_LAUNCHER)) {
                        found = (android.content.pm.ActivityInfo) Hidden.getField(a, "info");
                        break;
                    }
                }
                if (found != null) break;
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "activities of " + pkg, e);
        }
        synchronized (launcherCache) {
            launcherCache.put(pkg, new Object[] {apk.lastModified(), found});
        }
        return found;
    }

    /** Whether the intent asks for launcher activities (an app store's or launcher's query). */
    static boolean isLauncherQuery(Intent intent) {
        return Intent.ACTION_MAIN.equals(intent.getAction()) && intent.getCategories() != null
                && (intent.hasCategory(Intent.CATEGORY_LAUNCHER) || intent.hasCategory(Intent.CATEGORY_INFO));
    }

    /** ACTION_PACKAGE_ADDED/REMOVED, as the package manager broadcasts them (stores refresh on it). */
    private static void broadcast(String action, String pkg) {
        try {
            Context app = (Context) Hidden.callStatic(Hidden.cls("android.app.ActivityThread"), "currentApplication");
            if (app != null) app.sendBroadcast(new Intent(action, android.net.Uri.fromParts("package", pkg, null)));
        } catch (RuntimeException e) {
            Log.w(TAG, action, e);
        }
    }

    /** The package name in an APK's manifest (the framework's own parser), or null. */
    private static String packageOf(List<File> apks) {
        for (File f : apks) {
            try {
                Context app = (Context) Hidden.callStatic(Hidden.cls("android.app.ActivityThread"), "currentApplication");
                android.content.pm.PackageInfo pi = app.getPackageManager().getPackageArchiveInfo(f.getPath(), 0);
                if (pi != null && pi.packageName != null) return pi.packageName;
            } catch (RuntimeException e) {
                Log.w(TAG, "reading " + f.getName(), e);
            }
        }
        return null;
    }

    /**
     * Stores write more than APKs into a session: dex metadata (RuStore's "0.dm"), baseline
     * profiles, v4 signatures. Only APKs (zips) are the package.
     */
    private static boolean isApk(File f) {
        String n = f.getName().toLowerCase(java.util.Locale.ROOT);
        if (n.endsWith(".dm") || n.endsWith(".prof") || n.endsWith(".idsig") || n.endsWith(".fsv_sig")
                || n.endsWith(".digests") || n.endsWith(".signature")) {
            return false;
        }
        try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(f, "r")) {
            return r.readInt() == 0x504B0304;  // "PK\3\4"
        } catch (Exception e) {
            return false;
        }
    }

    private static void install(String pkg, List<File> apks) throws Exception {
        File apps = appsDir();
        File single = new File(apps, pkg + ".apk");
        File dir = new File(apps, pkg);
        single.delete();
        if (dir.isDirectory()) {
            File[] fs = dir.listFiles();
            if (fs != null) for (File f : fs) f.delete();
            dir.delete();
        }
        if (apks.size() == 1) {
            copy(apks.get(0), single);
            return;
        }
        dir.mkdirs();
        // The base APK is the one without a split name in its manifest (stores name the files
        // as they like); splits keep their split name: split_<name>.apk, as Android stores them.
        File base = null;
        Map<File, String> splits = new LinkedHashMap<>();
        for (File f : apks) {
            String split = splitName(f);
            if (split == null && base == null) base = f;
            else splits.put(f, split != null ? split : f.getName());
        }
        if (base == null) throw new IllegalArgumentException("no base APK among " + apks.size());
        copy(base, new File(dir, "base.apk"));
        for (Map.Entry<File, String> e : splits.entrySet()) {
            String name = e.getValue().replaceAll("[^A-Za-z0-9._-]", "_");
            copy(e.getKey(), new File(dir, "split_" + name + (name.endsWith(".apk") ? "" : ".apk")));
        }
    }

    /** The split name in an APK's manifest (null: a base APK), via the framework's parser. */
    private static String splitName(File apk) {
        try {
            Object lite = Hidden.callStatic(Hidden.cls("android.content.pm.PackageParser"), "parseApkLite", apk, 0);
            Object n = field(lite, "splitName");
            return n instanceof String ? (String) n : null;
        } catch (RuntimeException e) {
            Log.w(TAG, "reading " + apk.getName(), e);
            return apk.getName().startsWith("split") ? apk.getName() : null;
        }
    }

    private static void copy(File from, File to) throws Exception {
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
            byte[] b = new byte[1 << 16];
            for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n);
        }
    }
}
