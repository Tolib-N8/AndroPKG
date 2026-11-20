package dev.andropkg.system;

import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.Binder;
import android.os.IBinder;
import android.util.Log;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * The app's one task: a back stack of activities, driven with the same ClientTransactions
 * ActivityTaskManagerService sends, in the same order:
 *
 *   start B over A:  A pause -> B launch+resume -> A stop
 *   finish B:        B pause -> A resume (restart/start/resume) [+ result] -> B destroy
 *
 * Transactions run in order on the app's main thread, so the lifecycle interleaves exactly
 * as on a device.
 */
final class ActivityStack {
    private static final String TAG = "andropkg";

    static final class Record {
        final IBinder token = new Binder();
        final ActivityInfo info;
        final Intent intent;
        final Record resultTo;
        final String resultWho;
        final int requestCode;
        boolean finishing;

        Record(ActivityInfo info, Intent intent, Record resultTo, String resultWho, int requestCode) {
            this.info = info;
            this.intent = intent;
            this.resultTo = resultTo;
            this.resultWho = resultWho;
            this.requestCode = requestCode;
        }
    }

    private final List<Record> stack = new ArrayList<>();  // bottom .. top
    private final java.util.Set<IBinder> alive = new java.util.HashSet<>();  // not yet destroyed

    synchronized Record find(IBinder token) {
        for (Record r : stack) {
            if (r.token == token) return r;
        }
        return null;
    }

    /** True once every activity ever started has been destroyed. */
    synchronized boolean isEmpty() {
        return stack.isEmpty() && alive.isEmpty();
    }

    private Record top() {
        return stack.isEmpty() ? null : stack.get(stack.size() - 1);
    }

    /** ActivityInfo of the app's activity that handles `intent`, or null. */
    static ActivityInfo resolve(Intent intent) {
        ComponentName c = intent.getComponent();
        if (c != null) {
            if (!AndroPkgSystem.appInfo.packageName.equals(c.getPackageName())) return null;
            for (ActivityInfo ai : AndroPkgSystem.activityInfos()) {
                if (ai.name.equals(c.getClassName())) return ai;
            }
            return null;
        }
        for (Object a : AndroPkgSystem.components("activities")) {
            @SuppressWarnings("unchecked")
            List<Object> filters = (List<Object>) Hidden.getField(a, "intents");
            for (Object f : filters) {
                int match = ((IntentFilter) f).match(intent.getAction(), intent.getType(), intent.getScheme(),
                        intent.getData(), intent.getCategories(), TAG);
                if (match >= 0) {
                    ActivityInfo info = (ActivityInfo) Hidden.getField(a, "info");
                    info.applicationInfo = AndroPkgSystem.appInfo;
                    return info;
                }
            }
        }
        return null;
    }

    /** Starts `info` on top; returns ActivityManager.START_SUCCESS. */
    synchronized int start(ActivityInfo info, Intent intent, IBinder resultTo, String resultWho, int requestCode) {
        if (intent.getComponent() == null) {
            intent = new Intent(intent).setComponent(new ComponentName(info.packageName, info.name));
        }
        Record caller = resultTo != null ? find(resultTo) : null;
        Record r = new Record(info, intent, requestCode >= 0 ? caller : null, resultWho, requestCode);
        Record prev = top();
        stack.add(r);
        alive.add(r.token);
        Log.i(TAG, "start " + info.name + (prev != null ? " over " + prev.info.name : ""));
        if (prev != null) AndroPkgSystem.schedule(prev.token, null, pauseItem(false));
        Configuration config = WindowManagerImpl.displayConfiguration(Resources.getSystem().getConfiguration());
        Object launch = Hidden.callStatic(Hidden.cls("android.app.servertransaction.LaunchActivityItem"),
                "obtain", intent, System.identityHashCode(r), info, config, new Configuration(), 0 /* deviceId */,
                null /* referrer */, null, 2 /* PROCESS_STATE_TOP */, null, null, null, null, null,
                true /* isForward */, null, new Binder() /* assistToken */,
                AndroPkgSystem.activityClientController, new Binder() /* shareableActivityToken */, false, null);
        AndroPkgSystem.schedule(r.token, launch, resumeItem(true));
        if (prev != null) AndroPkgSystem.schedule(prev.token, null, stopItem());
        return 0;
    }

    /** Finishes `token`'s activity, delivering its result; true if it existed. */
    synchronized boolean finish(IBinder token, int resultCode, Intent data) {
        Record r = find(token);
        if (r == null || r.finishing) return false;
        r.finishing = true;
        boolean wasTop = r == top();
        stack.remove(r);
        Log.i(TAG, "finish " + r.info.name);
        Record next = top();
        AndroPkgSystem.schedule(r.token, null, pauseItem(true));
        if (wasTop && next != null) {
            Object result = null;
            if (r.resultTo == next && r.requestCode >= 0) {
                Object info = Hidden.newInstance(Hidden.cls("android.app.ResultInfo"), r.resultWho, r.requestCode,
                        resultCode, data);
                result = Hidden.callStatic(Hidden.cls("android.app.servertransaction.ActivityResultItem"), "obtain",
                        Collections.singletonList(info));
            }
            AndroPkgSystem.schedule(next.token, result, resumeItem(false));
        }
        AndroPkgSystem.schedule(r.token, null,
                Hidden.callStatic(Hidden.cls("android.app.servertransaction.DestroyActivityItem"), "obtain", true, 0));
        return true;
    }

    synchronized void destroyed(IBinder token) {
        Record r = find(token);
        if (r != null) stack.remove(r);
        alive.remove(token);
    }

    private static Object pauseItem(boolean finished) {
        // PauseActivityItem.obtain(finished, userLeaving, configChanges, dontReport, autoEnteringPip)
        return Hidden.callStatic(Hidden.cls("android.app.servertransaction.PauseActivityItem"), "obtain",
                finished, !finished, 0, false, false);
    }

    private static Object resumeItem(boolean forward) {
        return Hidden.callStatic(Hidden.cls("android.app.servertransaction.ResumeActivityItem"), "obtain",
                forward, false);
    }

    private static Object stopItem() {
        return Hidden.callStatic(Hidden.cls("android.app.servertransaction.StopActivityItem"), "obtain", 0);
    }
}
