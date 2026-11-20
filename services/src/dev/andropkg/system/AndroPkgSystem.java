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
    static String launchActivity;
    static Object parsedPackage;     // android.content.pm.PackageParser.Package
    static ApplicationInfo appInfo;
    static PackageInfo packageInfo;
    static Object appThread;         // android.app.IApplicationThread
    static IInterface activityClientController;
    static final Map<IBinder, ActivityInfo> activities = new HashMap<>();

    public static void main(String[] args) throws Exception {
        apkPath = args[0];
        dataDir = args[1];
        launchActivity = args.length > 2 ? args[2] : null;
        installServices();
        parsePackage();
        Log.i(TAG, "starting " + appInfo.packageName + " (" + apkPath + ")");
        // Never returns: the app's main looper takes over this thread.
        Hidden.callStatic(Hidden.cls("android.app.ActivityThread"), "main",
                (Object) new String[] {"seq=1"});
    }

    // ---- package ----------------------------------------------------------------------

    private static void parsePackage() {
        Class<?> ppClass = Hidden.cls("android.content.pm.PackageParser");
        Object parser = Hidden.newInstance(ppClass);
        parsedPackage = Hidden.call(parser, "parsePackage", new File(apkPath), 0);
        appInfo = (ApplicationInfo) Hidden.getField(parsedPackage, "applicationInfo");
        appInfo.sourceDir = appInfo.publicSourceDir = apkPath;
        appInfo.dataDir = dataDir;
        // As on a device: dataDir is the credential-protected dir (/data/user/0/<pkg>).
        Hidden.setField(appInfo, "credentialProtectedDataDir", dataDir);
        Hidden.setField(appInfo, "deviceProtectedDataDir", dataDir + "_de");
        new File(dataDir + "_de").mkdirs();
        appInfo.nativeLibraryDir = dataDir + "/lib";
        appInfo.uid = Process.myUid();
        if (appInfo.processName == null) appInfo.processName = appInfo.packageName;
        new File(dataDir).mkdirs();

        packageInfo = new PackageInfo();
        packageInfo.packageName = appInfo.packageName;
        packageInfo.applicationInfo = appInfo;
        packageInfo.versionName = (String) Hidden.getField(parsedPackage, "mVersionName");
        packageInfo.activities = activityInfos().toArray(new ActivityInfo[0]);

        if (launchActivity == null) launchActivity = findLauncherActivity();
    }

    @SuppressWarnings("unchecked")
    static List<ActivityInfo> activityInfos() {
        List<Object> acts = (List<Object>) Hidden.getField(parsedPackage, "activities");
        List<ActivityInfo> out = new java.util.ArrayList<>();
        for (Object a : acts) {
            ActivityInfo info = (ActivityInfo) Hidden.getField(a, "info");
            info.applicationInfo = appInfo;
            out.add(info);
        }
        return out;
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
        services.put("display", LocalService.create("android.hardware.display.IDisplayManager", new DisplayManagerImpl()));
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

        // getContentProvider(IApplicationThread, String callingPackage, String name, int userId,
        //                    boolean stable): only platform providers live outside the app.
        public synchronized Object getContentProvider(Object caller, String callingPackage, String name,
                int userId, boolean stable) {
            if (SettingsProvider.AUTHORITY.equals(name)) {
                if (settingsHolder == null) settingsHolder = settings.holder();
                return settingsHolder;
            }
            Log.w(TAG, "content provider \"" + name + "\" not provided");
            return null;
        }

        public void attachApplication(Object thread, long startSeq) {
            appThread = thread;
            Configuration config = new Configuration(Resources.getSystem().getConfiguration());
            Object compat = Hidden.getField(Hidden.cls("android.content.res.CompatibilityInfo"),
                    "DEFAULT_COMPATIBILITY_INFO");
            Hidden.call(thread, "bindApplication",
                    appInfo.processName, appInfo, null, null,
                    Hidden.callStatic(Hidden.cls("android.content.pm.ProviderInfoList"), "fromList",
                            new java.util.ArrayList<>()) /* providers */, null /* testName */, null /* profiler */, null, null, null,
                    0 /* debugMode */, false, false, false, false /* persistent */,
                    config, compat, null /* services */, new Bundle() /* coreSettings */,
                    "unknown" /* buildSerial */, null, null, new long[0], null,
                    0L, 0L);
            startActivity(launchActivity);
        }

        public void finishAttachApplication(long startSeq) {}

        public int getCurrentUser$() { return 0; }
    }

    static void startActivity(String className) {
        ActivityInfo info = activityInfo(className);
        Intent intent = new Intent(Intent.ACTION_MAIN)
                .addCategory(Intent.CATEGORY_LAUNCHER)
                .setComponent(new ComponentName(appInfo.packageName, className));
        IBinder token = new Binder();
        activities.put(token, info);
        Configuration config = new Configuration(Resources.getSystem().getConfiguration());

        Object launch = Hidden.callStatic(Hidden.cls("android.app.servertransaction.LaunchActivityItem"),
                "obtain", intent, 0, info, config, new Configuration(), 0 /* deviceId */,
                null /* referrer */, null, 2 /* ActivityManager.PROCESS_STATE_TOP (hidden) */, null, null, null,
                null, null, true /* isForward */, null, new Binder() /* assistToken */,
                activityClientController, new Binder(), false, null);
        Object resume = Hidden.callStatic(Hidden.cls("android.app.servertransaction.ResumeActivityItem"),
                "obtain", true /* isForward */, false);
        schedule(token, launch, resume);
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
        public Object getActivityClientController() { return activityClientController; }
    }

    /** IActivityClientController: callbacks from the app's activities. */
    static final class ActivityClientControllerImpl {
        public boolean finishActivity(IBinder token, int resultCode, Intent data, int finishTask) {
            Log.i(TAG, "finishActivity " + activities.get(token).name);
            Object destroy = Hidden.callStatic(
                    Hidden.cls("android.app.servertransaction.DestroyActivityItem"), "obtain", true, 0);
            schedule(token, null, destroy);
            return true;
        }

        public void activityDestroyed(IBinder token) {
            ActivityInfo info = activities.remove(token);
            Log.i(TAG, "activityDestroyed " + (info != null ? info.name : token));
            if (activities.isEmpty()) {
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

        public boolean hasSystemFeature(String name, int version) { return false; }

        public boolean isSafeMode() { return false; }
    }
}
