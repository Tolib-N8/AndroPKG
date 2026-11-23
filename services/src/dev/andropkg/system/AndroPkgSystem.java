package dev.andropkg.system;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Process;
import android.util.Log;
import java.io.File;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * andropkg's "system_server": a handful of in-process services and the launch sequence that
 * ActivityManagerService normally drives. Entry point: main(apkPath, dataDir[, activity]).
 *
 * Flow (same messages as on Android):
 *   ActivityThread.main() -> IActivityManager.attachApplication(appThread)
 *     -> appThread.bindApplication(...)            (Application + LoadedApk)
 *     -> appThread.scheduleTransaction(Launch + Resume)
 *   Activity.finish() -> IActivityClientController.finishActivity -> Destroy transaction
 */
public final class AndroPkgSystem {
    static final String TAG = "andropkg";

    static String apkPath;
    static String dataDir;
    static String hostDataDir;  // the app's data directory on the host (dataDir is its Android path)
    static volatile Throwable lastFatal;
    static String launchActivity;
    static Object parsedPackage;     // android.content.pm.PackageParser.Package
    static ApplicationInfo appInfo;
    static PackageInfo packageInfo;
    static Object appThread;         // android.app.IApplicationThread
    static IInterface activityClientController;
    static final ActivityStack stack = new ActivityStack();
    static final MediaSessionImpl mediaSessions = new MediaSessionImpl();

    /**
     * The APK's signing certificates (v3/v2 signature block, else v1 JAR signature), as the
     * package manager reports them: apps and SDKs check their own signature (Yandex Passport,
     * Google APIs keyed by certificate hash). Read without verifying digests (the installer's job).
     */
    static void setSigningInfo(PackageInfo info, String apk) {
        try {
            Object input = Hidden.callStatic(Hidden.cls("android.content.pm.parsing.result.ParseTypeImpl"),
                    "forDefaultParsing");
            Object res = Hidden.callStatic(Hidden.cls("android.util.apk.ApkSignatureVerifier"),
                    "unsafeGetCertsWithoutVerification", input, apk, 1 /* SignatureSchemeVersion.JAR */);
            if ((boolean) Hidden.call(res, "isError")) {
                Log.w(TAG, "no signing certificates in " + apk + ": " + Hidden.call(res, "getErrorMessage"));
                return;
            }
            Object details = Hidden.call(res, "getResult");
            info.signatures = (android.content.pm.Signature[]) Hidden.call(details, "getSignatures");
            Class<?> sd = Hidden.cls("android.content.pm.SigningDetails");
            java.lang.reflect.Constructor<android.content.pm.SigningInfo> c =
                    android.content.pm.SigningInfo.class.getDeclaredConstructor(sd);
            c.setAccessible(true);
            info.signingInfo = c.newInstance(details);
        } catch (Throwable t) {
            Log.w(TAG, "signing certificates unavailable for " + apk, t);
        }
    }

    static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    public static void main(String[] args) throws Exception {
        apkPath = args[0];
        dataDir = args[1];
        launchActivity = args.length > 2 ? args[2] : null;
        startWatchdog(Thread.currentThread());
        // As RuntimeInit.commonInit: uncaught exceptions are logged before the app's own
        // handler (crash reporters often exit without saying why).
        Hidden.callStatic(Thread.class, "setUncaughtExceptionPreHandler",
                (Thread.UncaughtExceptionHandler) (t, e) -> {
                    lastFatal = e;
                    Log.e("AndroidRuntime", "FATAL EXCEPTION: " + t.getName() + "\nProcess: " + appInfoName(), e);
                });
        // As RuntimeInit's KillApplicationHandler: the default handler ends the process. Crash
        // reporters chain to the previous default handler and expect one (a null one throws an
        // NPE from inside their handler, hiding the original exception).
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
            if (e != lastFatal) {  // (not already logged by the pre-handler)
                Log.e("AndroidRuntime", "FATAL EXCEPTION: " + t.getName() + "\nProcess: " + appInfoName(), e);
            }
            Process.killProcess(Process.myPid());
            System.exit(10);
        });
        installServices();
        // The window manager's natives (and the WKWebView bridge's) register when the first
        // SurfaceControl is made; apps create WebViews in onCreate, before their first window.
        try {
            new android.view.SurfaceControl.Builder().setName("andropkg:init").setBufferSize(1, 1).build().release();
        } catch (Throwable t) {
            Log.w(TAG, "SurfaceControl natives", t);
        }
        // As RuntimeInit.commonInit: the full MIME table (framework's mime.types), not libcore's
        // short built-in list (which lacks e.g. m4a, flac, webp).
        Class<?> factory = Hidden.cls("android.content.type.DefaultMimeMapFactory");
        Hidden.callStatic(Hidden.cls("libcore.content.type.MimeMap"), "setDefaultSupplier",
                (java.util.function.Supplier<Object>) () -> Hidden.callStatic(factory, "create"));
        // Software rendering: windows draw through Surface.lockCanvas() into CPU buffers until
        // hwui's GPU pipeline (RenderThread over Metal) is ported.
        Hidden.setStaticField(Hidden.cls("android.view.ThreadedRenderer"), "sRendererEnabled", false);
        parsePackage();
        if (System.getenv("ANDROPKG_PREVERIFY") != null) {
            // scripts/preverify.sh: the runtime verifies every class of the APK as its dex files
            // load, writes the vdex and exits. Only the class loader is needed: the app itself
            // would start running meanwhile (and may exit, missing its native libraries here).
            new dalvik.system.PathClassLoader(appInfo.sourceDir, ClassLoader.getSystemClassLoader().getParent());
            Log.i(TAG, "preverifying " + appInfo.sourceDir);
            while (true) {
                try {
                    Thread.sleep(60_000);
                } catch (InterruptedException e) {
                    // keep waiting for the runtime to exit
                }
            }
        }
        TouchDebug.installWhenReady();
        Log.i(TAG, "starting " + appInfo.packageName + " (" + apkPath + ")");
        // Never returns: the app's main looper takes over this thread.
        Hidden.callStatic(Hidden.cls("android.app.ActivityThread"), "main",
                (Object) new String[] {"seq=1"});
    }

    private static String appInfoName() { return appInfo != null ? appInfo.packageName : "?"; }

    /** -Dandropkg.watchdog=<seconds>: then log where the main thread is (hang diagnosis). */
    private static void startWatchdog(Thread main) {
        String s = System.getProperty("andropkg.watchdog");
        if (s == null) return;
        Thread t = new Thread(() -> {
            try {
                Thread.sleep(Long.parseLong(s) * 1000);
            } catch (InterruptedException e) {
                return;
            }
            StringBuilder b = new StringBuilder("watchdog: main thread is at\n");
            for (StackTraceElement e : main.getStackTrace()) b.append("    at ").append(e).append('\n');
            Log.w(TAG, b.toString());
        }, "andropkg-watchdog");
        t.setDaemon(true);
        t.start();
    }

    // ---- package ----------------------------------------------------------------------

    private static void parsePackage() {
        Class<?> ppClass = Hidden.cls("android.content.pm.PackageParser");
        Object parser = Hidden.newInstance(ppClass);
        // A directory is a split package (base APK + config splits: ABI, density, languages),
        // as an app bundle installs: PackageParser parses the cluster.
        parsedPackage = Hidden.call(parser, "parsePackage", new File(apkPath), 0);
        appInfo = (ApplicationInfo) Hidden.getField(parsedPackage, "applicationInfo");
        if (new File(apkPath).isDirectory()) {
            String base = (String) Hidden.getField(parsedPackage, "baseCodePath");
            String[] splits = (String[]) Hidden.getField(parsedPackage, "splitCodePaths");
            String[] names = (String[]) Hidden.getField(parsedPackage, "splitNames");
            appInfo.sourceDir = appInfo.publicSourceDir = base;
            appInfo.splitSourceDirs = appInfo.splitPublicSourceDirs = splits;
            appInfo.splitNames = names;
            Log.i(TAG, "split package: " + new File(base).getName() + " + " + (splits == null ? 0 : splits.length)
                    + " splits " + java.util.Arrays.toString(names));
        } else {
            appInfo.sourceDir = appInfo.publicSourceDir = apkPath;
        }
        // As on a device, the app sees /data/user/0/<pkg> (credential-protected) and
        // /data/user_de/0/<pkg>; path translation maps them onto the host directory (dataDir),
        // whose iOS container path changes when the app is reinstalled. Apps store these paths.
        String hostData = dataDir;
        hostDataDir = hostData;
        try {
            android.system.Os.setenv("ANDROPKG_APP_DATA_HOST", hostData, true);
            android.system.Os.setenv("ANDROPKG_APP_DATA_PKG", appInfo.packageName, true);
        } catch (android.system.ErrnoException e) {
            throw new RuntimeException(e);
        }
        new File(hostData).mkdirs();
        new File(hostData + "_de").mkdirs();
        dataDir = "/data/user/0/" + appInfo.packageName;
        appInfo.dataDir = dataDir;
        Hidden.setField(appInfo, "credentialProtectedDataDir", dataDir);
        Hidden.setField(appInfo, "deviceProtectedDataDir", "/data/user_de/0/" + appInfo.packageName);
        // Converted native libraries (scripts/prepare-native-libs.sh): <host data>/lib, or a
        // signed location inside the iOS app bundle (-Dandropkg.nativeLibraryDir). A host path:
        // the dynamic loader doesn't translate.
        appInfo.nativeLibraryDir = System.getProperty("andropkg.nativeLibraryDir", hostData + "/lib");
        Hidden.setField(appInfo, "primaryCpuAbi", "arm64-v8a");
        appInfo.uid = Process.myUid();
        if (appInfo.processName == null) appInfo.processName = appInfo.packageName;
        if (appInfo.metaData == null) appInfo.metaData = (android.os.Bundle) Hidden.getField(parsedPackage, "mAppMetaData");
        // keystore2's key blobs: next to the app's data, outside it (as /data/misc/keystore).
        // (debugging switch) start with an empty app-specific external storage (caches).
        if (System.getenv("ANDROPKG_TEST_CLEAR_EXTERNAL") != null) {
            deleteTree(new File("/storage/emulated/0/Android/data/" + appInfo.packageName));
            Log.i(TAG, "cleared external app storage");
        }
        if (System.getenv("ANDROPKG_TEST_NO_KEYSTORE") == null) {  // (debugging switch)
            AndroidKeyStore.install(new File(hostData + "_keystore"));
        }

        packageInfo = new PackageInfo();
        packageInfo.packageName = appInfo.packageName;
        packageInfo.applicationInfo = appInfo;
        packageInfo.versionName = (String) Hidden.getField(parsedPackage, "mVersionName");
        packageInfo.activities = activityInfos().toArray(new ActivityInfo[0]);
        packageInfo.receivers = AndroPkgSystem.<ActivityInfo>infos("receivers").toArray(new ActivityInfo[0]);
        packageInfo.providers = AndroPkgSystem.<android.content.pm.ProviderInfo>infos("providers")
                .toArray(new android.content.pm.ProviderInfo[0]);
        packageInfo.services = AndroPkgSystem.<android.content.pm.ServiceInfo>infos("services")
                .toArray(new android.content.pm.ServiceInfo[0]);
        packageInfo.versionCode = (int) Hidden.getField(parsedPackage, "mVersionCode");
        setSigningInfo(packageInfo, appInfo.sourceDir);

        if (launchActivity == null) launchActivity = findLauncherActivity();
    }

    /** PackageParser.Package components: "activities", "receivers", "providers", "services". */
    @SuppressWarnings("unchecked")
    static List<Object> components(String field) {
        return (List<Object>) Hidden.getField(parsedPackage, field);
    }

    /** The ComponentInfos of one kind, bound to the app's ApplicationInfo. */
    @SuppressWarnings("unchecked")
    static <T extends android.content.pm.ComponentInfo> List<T> infos(String field) {
        List<T> out = new java.util.ArrayList<>();
        for (Object c : components(field)) {
            T info = (T) Hidden.getField(c, "info");
            info.applicationInfo = appInfo;
            // As with PackageManager.GET_META_DATA (always on: the app's own components).
            if (info.metaData == null) info.metaData = (android.os.Bundle) Hidden.getField(c, "metaData");
            out.add(info);
        }
        return out;
    }

    static List<ActivityInfo> activityInfos() {
        return infos("activities");
    }

    @SuppressWarnings("unchecked")
    private static String findLauncherActivity() {
        List<Object> acts = (List<Object>) Hidden.getField(parsedPackage, "activities");
        for (Object a : acts) {
            for (Object filter : (List<Object>) Hidden.getField(a, "intents")) {
                android.content.IntentFilter f = (android.content.IntentFilter) filter;
                if (f.hasAction(Intent.ACTION_MAIN) && f.hasCategory(Intent.CATEGORY_LAUNCHER)) {
                    return ((ActivityInfo) Hidden.getField(a, "info")).name;
                }
            }
        }
        throw new IllegalStateException("no launcher activity in " + apkPath);
    }

    static ActivityInfo activityInfo(String className) {
        for (ActivityInfo ai : activityInfos()) {
            if (ai.name.equals(className)) return ai;
        }
        throw new IllegalArgumentException("unknown activity " + className);
    }

    // ---- services ---------------------------------------------------------------------

    static final Map<String, IBinder> services = new HashMap<>();

    private static void installServices() {
        services.put("activity", LocalService.create("android.app.IActivityManager", new ActivityManagerImpl()));
        services.put("activity_task", LocalService.create("android.app.IActivityTaskManager", new ActivityTaskManagerImpl()));
        services.put("package", LocalService.create("android.content.pm.IPackageManager", new PackageManagerImpl()));
        services.put("permissionmgr", LocalService.create("android.permission.IPermissionManager", new Object()));
        DisplayManagerImpl display = new DisplayManagerImpl();
        services.put("display", LocalService.create("android.hardware.display.IDisplayManager", display));
        services.put("window", LocalService.create("android.view.IWindowManager", new WindowManagerImpl(display)));
        services.put("input", LocalService.create("android.hardware.input.IInputManager", new InputManagerImpl()));
        services.put("user", LocalService.create("android.os.IUserManager", new UserManagerImpl()));
        services.put("notification", LocalService.create("android.app.INotificationManager", new NotificationManagerImpl()));
        services.put("mount", LocalService.create("android.os.storage.IStorageManager", new StorageManagerImpl()));
        services.put("connectivity", LocalService.create("android.net.IConnectivityManager", new ConnectivityManagerImpl()));
        services.put("audio", LocalService.create("android.media.IAudioService", new AudioServiceImpl()));
        services.put("media_session", LocalService.create("android.media.session.ISessionManager", mediaSessions));
        services.put("media_router", LocalService.create("android.media.IMediaRouterService", new MediaRouterImpl()));
        services.put("media_metrics", LocalService.create("android.media.metrics.IMediaMetricsManager", new MediaMetricsImpl()));
        services.put("account", LocalService.create("android.accounts.IAccountManager", new AccountManagerImpl()));
        services.put("input_method", LocalService.create("com.android.internal.view.IInputMethodManager",
                new InputMethodManagerImpl()));
        activityClientController =
                LocalService.create("android.app.IActivityClientController", new ActivityClientControllerImpl()).iface;
        // The service manager itself is in-process: no binder driver, no servicemanager daemon.
        Class<?> sm = Hidden.cls("android.os.ServiceManager");
        Object iServiceManager = LocalService.create("android.os.IServiceManager", new ServiceManagerImpl()).iface;
        Hidden.setStaticField(sm, "sServiceManager", iServiceManager);
        Hidden.callStatic(sm, "initServiceCache", services);
    }

    /** android.os.IServiceManager over the in-process registry. */
    static final class ServiceManagerImpl {
        private final java.util.Set<String> missing = new java.util.HashSet<>();

        public IBinder getService(String name) { return lookup(name); }
        public IBinder checkService(String name) { return lookup(name); }
        public boolean isDeclared(String name) { return services.containsKey(name); }
        public void addService(String name, IBinder service, boolean allowIsolated, int dumpPriority) {
            services.put(name, service);
        }

        private IBinder lookup(String name) {
            IBinder b = services.get(name);
            if (b == null && !missing.contains(name)) {
                b = DefaultServices.create(name);
                if (b != null) services.put(name, b);
                else if (missing.add(name)) Log.w(TAG, "service \"" + name + "\" not provided");
            }
            return b;
        }
    }

    /** IActivityManager: process attach + launch (what ActivityManagerService does). */
    static final class ActivityManagerImpl {
        private final SettingsProvider settings = new SettingsProvider();
        private Object settingsHolder;
        private final MediaStoreProvider mediaStore = new MediaStoreProvider();
        private Object mediaStoreHolder;

        // getContentProvider(IApplicationThread, String callingPackage, String name, int userId,
        //                    boolean stable): only platform providers live outside the app.
        public synchronized Object getContentProvider(Object caller, String callingPackage, String name,
                int userId, boolean stable) {
            if (SettingsProvider.AUTHORITY.equals(name)) {
                if (settingsHolder == null) settingsHolder = settings.holder();
                return settingsHolder;
            }
            if (MediaStoreProvider.AUTHORITY.equals(name)) {
                if (mediaStoreHolder == null) mediaStoreHolder = mediaStore.holder();
                return mediaStoreHolder;
            }
            Log.w(TAG, "content provider \"" + name + "\" not provided");
            return null;
        }

        public void attachApplication(Object thread, long startSeq) {
            appThread = thread;
            Configuration config = WindowManagerImpl.displayConfiguration(Resources.getSystem().getConfiguration());
            Object compat = Hidden.getField(Hidden.cls("android.content.res.CompatibilityInfo"),
                    "DEFAULT_COMPATIBILITY_INFO");
            Hidden.call(thread, "bindApplication",
                    appInfo.processName, appInfo, null, null,
                    // The app's own content providers are installed in-process by bindApplication.
                    Hidden.callStatic(Hidden.cls("android.content.pm.ProviderInfoList"), "fromList",
                            infos("providers")) /* providers */, null /* testName */, null /* profiler */, null, null, null,
                    0 /* debugMode */, false, false, false, false /* persistent */,
                    config, compat, null /* services */, new Bundle() /* coreSettings */,
                    "unknown" /* buildSerial */, null, null, new long[0], null,
                    0L, 0L);
            // WebView without a WebView package: local content as styled text (WKWebView bridge
            // later). Installed once the application is bound: apps call
            // WebView.setDataDirectorySuffix() in attachBaseContext, before WebView may initialize.
            new android.os.Handler(android.os.Looper.getMainLooper())
                    .post(dev.andropkg.system.webview.TextWebView::install);
            ActivityInfo info = activityInfo(launchActivity);
            Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                    .setComponent(new ComponentName(appInfo.packageName, info.name))
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            stack.start(info, intent, null, null, -1);
        }

        public void finishAttachApplication(long startSeq) {}

        // ---- services (ActiveServices) ----
        private final ActiveServices serviceManager = new ActiveServices();

        public ComponentName startService(Object caller, Intent service, String resolvedType, boolean requireForeground,
                String callingPackage, String callingFeatureId, int userId) {
            return serviceManager.start(service);
        }

        public int stopService(Object caller, Intent service, String resolvedType, int userId) {
            return serviceManager.stop(service);
        }

        public boolean stopServiceToken(ComponentName className, IBinder token, int startId) {
            return serviceManager.stopSelf(className, token, startId);
        }

        public int bindServiceInstance(Object caller, IBinder token, Intent service, String resolvedType,
                Object connection, long flags, String instanceName, String callingPackage, int userId) {
            return serviceManager.bind(service, connection, flags);
        }

        public boolean unbindService(Object connection) {
            return serviceManager.unbind(connection);
        }

        public void publishService(IBinder token, Intent intent, IBinder service) {
            serviceManager.publish(token, intent, service);
        }

        public IBinder peekService(Intent service, String resolvedType, String callingPackage) {
            return serviceManager.peek(service);
        }

        // ---- broadcasts and pending intents ----
        private final Broadcasts broadcasts = new Broadcasts();
        private final PendingIntents pendingIntents = new PendingIntents(broadcasts, serviceManager);

        public Intent registerReceiverWithFeature(Object caller, String callerPackage, String featureId,
                String receiverId, Object receiver, android.content.IntentFilter filter, String permission, int userId,
                int flags) {
            return broadcasts.register(receiver, filter);
        }

        public void unregisterReceiver(Object receiver) {
            broadcasts.unregister(receiver);
        }

        public int broadcastIntentWithFeature(Object caller, String featureId, Intent intent, String resolvedType,
                Object resultTo, int resultCode, String resultData, Bundle map, String[] requiredPermissions,
                String[] excludedPermissions, String[] excludePackages, int appOp, Bundle options, boolean serialized,
                boolean sticky, int userId) {
            return broadcasts.send(intent, sticky, resultTo);
        }

        public Object getIntentSenderWithFeature(int type, String packageName, String featureId, IBinder token,
                String resultWho, int requestCode, Intent[] intents, String[] resolvedTypes, int flags, Bundle options,
                int userId) {
            return pendingIntents.get(type, intents, requestCode, flags);
        }

        public int sendIntentSender(Object caller, Object target, IBinder whitelistToken, int code, Intent intent,
                String resolvedType, Object finishedReceiver, String requiredPermission, Bundle options) {
            return pendingIntents.send(target, code, intent, finishedReceiver);
        }

        public void cancelIntentSender(Object sender) { pendingIntents.cancel(sender); }
        public Intent getIntentForIntentSender(Object sender) { return pendingIntents.intentOf(sender); }
        public Object getInfoForIntentSender(Object sender) { return pendingIntents.infoOf(sender); }
        public boolean registerIntentSenderCancelListenerEx(Object sender, Object receiver) { return true; }

        public void setServiceForeground(ComponentName className, IBinder token, int id,
                android.app.Notification notification, int flags, int foregroundServiceType) {
            Log.i(TAG, "service " + className.getShortClassName() + (id != 0 ? " in foreground" : " left foreground"));
        }

        public int getCurrentUser$() { return 0; }
    }

    static void schedule(IBinder token, Object callback, Object lifecycle) {
        Object tx = Hidden.callStatic(Hidden.cls("android.app.servertransaction.ClientTransaction"),
                "obtain", appThread, token);
        if (callback != null) Hidden.call(tx, "addCallback", callback);
        if (lifecycle != null) Hidden.call(tx, "setLifecycleStateRequest", lifecycle);
        Hidden.call(appThread, "scheduleTransaction", tx);
    }

    /** IActivityTaskManager. */
    static final class ActivityTaskManagerImpl {
        /** ActivityManager.getDeviceConfigurationInfo (IActivityTaskManager on 14): a touch screen, no keyboard, GLES 3.0 (EAGL). */
        public android.content.pm.ConfigurationInfo getDeviceConfigurationInfo() {
            android.content.pm.ConfigurationInfo c = new android.content.pm.ConfigurationInfo();
            c.reqTouchScreen = android.content.res.Configuration.TOUCHSCREEN_FINGER;
            c.reqKeyboardType = android.content.res.Configuration.KEYBOARD_NOKEYS;
            c.reqNavigation = android.content.res.Configuration.NAVIGATION_NONAV;
            c.reqGlEsVersion = 0x00030000;
            return c;
        }

        public Object getActivityClientController() { return activityClientController; }

        // startActivity(IApplicationThread caller, String callingPackage, String callingFeatureId,
        //     Intent intent, String resolvedType, IBinder resultTo, String resultWho, int requestCode,
        //     int flags, ProfilerInfo profilerInfo, Bundle options)
        public int startActivity(Object caller, String callingPackage, String callingFeatureId, Intent intent,
                String resolvedType, IBinder resultTo, String resultWho, int requestCode, int flags,
                Object profilerInfo, Bundle options) {
            ActivityInfo info = ActivityStack.resolve(intent);
            if (info == null) {
                Log.w(TAG, "no activity for " + intent);
                return -92;  // ActivityManager.START_CLASS_NOT_FOUND -> ActivityNotFoundException
            }
            return stack.start(info, intent, resultTo, resultWho, requestCode);
        }

        // startActivities(IApplicationThread caller, String callingPackage, String callingFeatureId,
        //     Intent[] intents, String[] resolvedTypes, IBinder resultTo, Bundle options, int userId):
        // e.g. TaskStackBuilder (parent stack + target). Started in order, bottom first.
        public int startActivities(Object caller, String callingPackage, String callingFeatureId, Intent[] intents,
                String[] resolvedTypes, IBinder resultTo, Bundle options, int userId) {
            for (Intent intent : intents) {
                if (ActivityStack.resolve(intent) == null) {
                    Log.w(TAG, "no activity for " + intent);
                    return -92;
                }
            }
            for (Intent intent : intents) stack.start(ActivityStack.resolve(intent), intent, resultTo, null, -1);
            return 0;
        }
    }

    /** IActivityClientController: callbacks from the app's activities. */
    static final class ActivityClientControllerImpl {
        public boolean finishActivity(IBinder token, int resultCode, Intent data, int finishTask) {
            return stack.finish(token, resultCode, data);
        }

        // navigateUpTo(IBinder token, Intent destIntent, String resolvedType, int resultCode, Intent resultData)
        public boolean navigateUpTo(IBinder token, Intent dest, String resolvedType, int resultCode, Intent data) {
            return stack.navigateUpTo(token, dest, resultCode, data);
        }

        public int getTaskForActivity(IBinder token, boolean onlyRoot) {
            return !onlyRoot || stack.isRoot(token) ? 1 /* the app's one task */ : -1;
        }

        // onBackPressed(IBinder token, IRequestFinishCallback callback): Activity.onBackPressed
        // asks the system (Android 13+), for every activity. Above the root: finish it (via the
        // callback, i.e. finishAfterTransition). On the task's root, Android 12+ moves the task
        // back instead of finishing it; on iOS the app simply stays.
        // Tests: -Dandropkg.back.finishesRoot=1 finishes the root too (pre-12 behavior).
        public void onBackPressed(IBinder token, Object callback) {
            if (!stack.isRoot(token) || System.getProperty("andropkg.back.finishesRoot") != null) {
                Hidden.call(callback, "requestFinish");
            }
        }

        public void activityDestroyed(IBinder token) {
            stack.destroyed(token);
            if (stack.isEmpty()) {
                Log.i(TAG, "last activity gone; app finished");
                System.exit(0);
            }
        }
    }

    /** IPackageManager: answers for the one installed package. */
    static final class PackageManagerImpl {
        public PackageInfo getPackageInfo(String pkg, long flags, int userId) {
            return appInfo.packageName.equals(pkg) ? packageInfo : null;
        }

        public ApplicationInfo getApplicationInfo(String pkg, long flags, int userId) {
            return appInfo.packageName.equals(pkg) ? appInfo : null;
        }

        public ActivityInfo getActivityInfo(ComponentName c, long flags, int userId) {
            return appInfo.packageName.equals(c.getPackageName()) ? activityInfo(c.getClassName()) : null;
        }

        public android.content.pm.ProviderInfo getProviderInfo(ComponentName c, long flags, int userId) {
            return find(AndroPkgSystem.<android.content.pm.ProviderInfo>infos("providers"), c);
        }

        public android.content.pm.ServiceInfo getServiceInfo(ComponentName c, long flags, int userId) {
            return find(AndroPkgSystem.<android.content.pm.ServiceInfo>infos("services"), c);
        }

        public ActivityInfo getReceiverInfo(ComponentName c, long flags, int userId) {
            return find(AndroPkgSystem.<ActivityInfo>infos("receivers"), c);
        }

        public android.content.pm.ProviderInfo resolveContentProvider(String authority, long flags, int userId) {
            for (android.content.pm.ProviderInfo p : AndroPkgSystem.<android.content.pm.ProviderInfo>infos("providers")) {
                if (p.authority != null && java.util.Arrays.asList(p.authority.split(";")).contains(authority)) return p;
            }
            return null;
        }

        public android.content.pm.ResolveInfo resolveIntent(Intent intent, String type, long flags, int userId) {
            ActivityInfo ai = ActivityStack.resolve(intent);
            if (ai == null) return null;
            android.content.pm.ResolveInfo ri = new android.content.pm.ResolveInfo();
            ri.activityInfo = ai;
            return ri;
        }

        public Object queryIntentActivities(Intent intent, String type, long flags, int userId) {
            List<android.content.pm.ResolveInfo> list = new java.util.ArrayList<>();
            android.content.pm.ResolveInfo ri = resolveIntent(intent, type, flags, userId);
            if (ri != null) list.add(ri);
            return Hidden.newInstance(Hidden.cls("android.content.pm.ParceledListSlice"), list);
        }

        public int getComponentEnabledSetting(ComponentName c, int userId) { return 0; }
        public int getApplicationEnabledSetting(String pkg, int userId) { return 0; }
        public String getInstallerPackageName(String pkg) { return null; }
        public int getPackageUid(String pkg, long flags, int userId) {
            return appInfo.packageName.equals(pkg) ? appInfo.uid : -1;
        }
        public String[] getPackagesForUid(int uid) { return new String[] {appInfo.packageName}; }

        private static <T extends android.content.pm.ComponentInfo> T find(List<T> list, ComponentName c) {
            if (!appInfo.packageName.equals(c.getPackageName())) return null;
            for (T i : list) {
                if (i.name.equals(c.getClassName())) return i;
            }
            return null;
        }

        public boolean hasSystemFeature(String name, int version) { return false; }

        public boolean isSafeMode() { return false; }
    }
}
