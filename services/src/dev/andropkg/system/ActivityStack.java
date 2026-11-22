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
        final IBinder token;
        final ActivityInfo info;
        final Intent intent;
        final Record resultTo;
        final String resultWho;
        final int requestCode;
        boolean finishing;
        boolean translucent;  // windowIsTranslucent / windowIsFloating: what's under it shows
        boolean stopped;      // sent StopActivityItem (no longer visible)

        Record(ActivityInfo info, Intent intent, Record resultTo, String resultWho, int requestCode) {
            this.token = new ActivityToken(info.name);
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
    /** The task went behind (iOS app backgrounded): pause and stop the top activity. */
    synchronized void background() {
        Record top = top();
        if (top == null) return;
        AndroPkgSystem.schedule(top.token, null, pauseItem(false));
        top.stopped = true;
        AndroPkgSystem.schedule(top.token, null, stopItem());
    }

    /** Back to front: the top activity restarts and resumes. */
    synchronized void foreground() {
        Record top = top();
        if (top == null) return;
        top.stopped = false;
        AndroPkgSystem.schedule(top.token, null, resumeItem(false));
    }

    synchronized boolean isRoot(IBinder token) {
        return !stack.isEmpty() && stack.get(0).token == token;
    }

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

    /**
     * Starts `info` per the intent flags and launch mode; returns ActivityManager.START_SUCCESS
     * (or START_DELIVERED_TO_TOP when an existing instance got the intent via onNewIntent).
     *   CLEAR_TASK (+NEW_TASK): the task is emptied first (TaskStackBuilder does this).
     *   CLEAR_TOP / singleTask / singleInstance: an existing instance becomes the top; it gets
     *     onNewIntent if it is single-top (flag or launchMode), else it is recreated.
     *   SINGLE_TOP / singleTop: the top instance gets onNewIntent instead of a new copy.
     */
    synchronized int start(ActivityInfo info, Intent intent, IBinder resultTo, String resultWho, int requestCode) {
        if (intent.getComponent() == null) {
            intent = new Intent(intent).setComponent(new ComponentName(info.packageName, info.name));
        }
        int flags = intent.getFlags();
        boolean singleTop = (flags & Intent.FLAG_ACTIVITY_SINGLE_TOP) != 0
                || info.launchMode == ActivityInfo.LAUNCH_SINGLE_TOP;
        boolean singleTask = info.launchMode == ActivityInfo.LAUNCH_SINGLE_TASK
                || info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE
                || info.launchMode == 4 /* LAUNCH_SINGLE_INSTANCE_PER_TASK */;
        List<Record> doomed = new ArrayList<>();
        if ((flags & Intent.FLAG_ACTIVITY_CLEAR_TASK) != 0 && (flags & Intent.FLAG_ACTIVITY_NEW_TASK) != 0) {
            doomed.addAll(stack);
        } else if ((flags & Intent.FLAG_ACTIVITY_CLEAR_TOP) != 0 || singleTask) {
            Record existing = null;
            for (int i = stack.size() - 1; i >= 0 && existing == null; i--) {
                if (stack.get(i).info.name.equals(info.name)) existing = stack.get(i);
            }
            if (existing != null) {
                doomed.addAll(stack.subList(stack.indexOf(existing) + 1, stack.size()));
                if (singleTop || singleTask) {
                    clear(doomed, existing);
                    deliverNewIntent(existing, intent);
                    return 3;  // START_DELIVERED_TO_TOP
                }
                doomed.add(existing);
            }
        } else if (singleTop && top() != null && top().info.name.equals(info.name)) {
            deliverNewIntent(top(), intent);
            return 3;
        }
        Record caller = resultTo != null ? find(resultTo) : null;
        Record r = new Record(info, intent, requestCode >= 0 ? caller : null, resultWho, requestCode);
        Record prev = top();
        stack.removeAll(doomed);
        stack.add(r);
        alive.add(r.token);
        Log.i(TAG, "start " + info.name + (prev != null ? " over " + prev.info.name : "")
                + (doomed.isEmpty() ? "" : " (clearing " + doomed.size() + ")"));
        if (prev != null) AndroPkgSystem.schedule(prev.token, null, pauseItem(doomed.contains(prev)));
        Configuration config = WindowManagerImpl.displayConfiguration(Resources.getSystem().getConfiguration());
        Object launch = Hidden.callStatic(Hidden.cls("android.app.servertransaction.LaunchActivityItem"),
                "obtain", intent, System.identityHashCode(r), info, config, new Configuration(), 0 /* deviceId */,
                null /* referrer */, null, 2 /* PROCESS_STATE_TOP */, null, null, null, null, null,
                true /* isForward */, null, new Binder() /* assistToken */,
                AndroPkgSystem.activityClientController, new Binder() /* shareableActivityToken */, false, null);
        AndroPkgSystem.schedule(r.token, launch, resumeItem(true));
        // As ActivityRecord.occludesParent: a translucent or floating activity (dialogs, login
        // overlays, trampolines) leaves the ones under it visible (paused, not stopped) down to
        // the first opaque one; an opaque one stops everything under it that is still visible.
        r.translucent = isTranslucentOrFloating(info);
        if (!r.translucent) {
            for (Record below : stack) {
                if (below != r && !below.stopped && !doomed.contains(below)) {
                    below.stopped = true;
                    AndroPkgSystem.schedule(below.token, null, stopItem());
                }
            }
        }
        for (Record d : doomed) destroy(d);
        return 0;
    }

    /** Finishes `doomed` (the activities above `newTop`, top first) and resumes `newTop`. */
    private void clear(List<Record> doomed, Record newTop) {
        if (doomed.isEmpty()) return;
        Record prev = top();
        stack.removeAll(doomed);
        Log.i(TAG, "clear " + doomed.size() + " above " + newTop.info.name);
        AndroPkgSystem.schedule(prev.token, null, pauseItem(true));
        newTop.stopped = false;
        AndroPkgSystem.schedule(newTop.token, null, resumeItem(false));
        for (int i = doomed.size() - 1; i >= 0; i--) destroy(doomed.get(i));
    }

    private void destroy(Record r) {
        r.finishing = true;
        AndroPkgSystem.schedule(r.token, null,
                Hidden.callStatic(Hidden.cls("android.app.servertransaction.DestroyActivityItem"), "obtain", true, 0));
    }

    /** onNewIntent for an existing activity (paused around the call if it is resumed). */
    private void deliverNewIntent(Record r, Intent intent) {
        Log.i(TAG, "new intent for " + r.info.name);
        Object referrer = Hidden.newInstance(Hidden.cls("com.android.internal.content.ReferrerIntent"),
                intent, AndroPkgSystem.appInfo.packageName);
        Object item = Hidden.callStatic(Hidden.cls("android.app.servertransaction.NewIntentItem"), "obtain",
                Collections.singletonList(referrer), true);
        r.stopped = false;
        AndroPkgSystem.schedule(r.token, item, resumeItem(false));
    }

    /** Finishes `token`'s activity, delivering its result; true if it existed. */
    synchronized boolean finish(IBinder token, int resultCode, Intent data) {
        Record r = find(token);
        // Already on its way out (e.g. cleared by CLEAR_TASK): finish() succeeded, as on Android.
        if (r == null) return alive.contains(token);
        if (r.finishing) return true;
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
            next.stopped = false;
            AndroPkgSystem.schedule(next.token, result, resumeItem(false));
        }
        AndroPkgSystem.schedule(r.token, null,
                Hidden.callStatic(Hidden.cls("android.app.servertransaction.DestroyActivityItem"), "obtain", true, 0));
        return true;
    }

    /**
     * Up navigation (Activity.navigateUpTo): finishes `token`'s activity and everything above
     * the nearest instance of `dest` below it; that parent gets onNewIntent if it is single-top
     * (or the intent says CLEAR_TOP), and the result. Without such a parent only `token` goes.
     */
    synchronized boolean navigateUpTo(IBinder token, Intent dest, int resultCode, Intent resultData) {
        Record r = find(token);
        if (r == null) return false;
        int start = stack.indexOf(r);
        int parentIndex = start - 1;
        Record parent = null;
        ComponentName c = dest != null ? dest.getComponent() : null;
        if (c != null) {
            for (int i = start - 1; i >= 0; i--) {
                if (stack.get(i).info.name.equals(c.getClassName())) {
                    parent = stack.get(i);
                    parentIndex = i;
                    break;
                }
            }
        }
        List<Record> doomed = new ArrayList<>(stack.subList(parentIndex + 1, stack.size()));
        Log.i(TAG, "navigate up from " + r.info.name + (parent != null ? " to " + parent.info.name : ""));
        if (parent == null) return finish(token, resultCode, resultData);
        clear(doomed, parent);
        boolean singleTop = parent.info.launchMode != ActivityInfo.LAUNCH_MULTIPLE
                || (dest.getFlags() & Intent.FLAG_ACTIVITY_CLEAR_TOP) != 0;
        if (singleTop) deliverNewIntent(parent, dest);
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

    /** ActivityInfo.isTranslucentOrFloating, from the activity's theme. */
    static boolean isTranslucentOrFloating(ActivityInfo info) {
        try {
            android.content.Context app = (android.content.Context) Hidden.callStatic(
                    Hidden.cls("android.app.ActivityThread"), "currentApplication");
            if (app == null) return false;
            Resources.Theme theme = app.getResources().newTheme();
            int themeRes = info.getThemeResource();
            if (themeRes != 0) theme.applyStyle(themeRes, true);
            android.content.res.TypedArray a = theme.obtainStyledAttributes(new int[] {
                    android.R.attr.windowIsTranslucent, android.R.attr.windowIsFloating,
                    android.R.attr.windowSwipeToDismiss});
            try {
                boolean translucent = a.getBoolean(0, false);
                boolean floating = a.getBoolean(1, false);
                boolean swipe = !a.hasValue(0) && a.getBoolean(2, false);
                return translucent || floating || swipe;
            } finally {
                a.recycle();
            }
        } catch (Throwable t) {
            Log.w(TAG, "theme of " + info.name, t);
            return false;
        }
    }

    private static Object stopItem() {
        return Hidden.callStatic(Hidden.cls("android.app.servertransaction.StopActivityItem"), "obtain", 0);
    }
}
