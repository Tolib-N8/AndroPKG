package dev.andropkg.system;

import android.app.job.JobInfo;
import android.app.job.JobWorkItem;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PersistableBundle;
import android.util.Log;
import java.lang.reflect.Constructor;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * android.app.job.IJobScheduler (JobScheduler; WorkManager's SystemJobScheduler on API 23+,
 * which most apps use for their background work: syncs, uploads, refreshes).
 *
 * Jobs of the one app run in-process, as JobSchedulerService runs them: after the job's minimum
 * latency (a periodic job: every interval), its JobService is bound and IJobService.startJob
 * gets JobParameters whose callback is ours (jobFinished, dequeueWork, ...). Constraints
 * (network, charging, idle) count as met. Jobs live as long as the process, as WorkManager
 * re-schedules its work on every start.
 *
 * The AIDL methods take a namespace first since Android 14; they are matched by name with any
 * arguments (LocalService's Object[] form), the JobInfo/JobWorkItem/int found by type.
 */
final class JobSchedulerImpl {
    private static final String TAG = "andropkg-jobs";
    private static final int RESULT_SUCCESS = 1;

    private final class Job {
        final JobInfo info;
        final List<JobWorkItem> work = new ArrayList<>();
        int nextWorkId = 1;
        boolean running, rerun, cancelled;
        Object service;      // IJobService
        Object params;       // JobParameters
        ServiceConnection connection;
        final Runnable start = this::run;

        Job(JobInfo info) {
            this.info = info;
        }

        int id() {
            return info.getId();
        }

        void run() {
            synchronized (JobSchedulerImpl.this) {
                if (cancelled || running) return;
                running = true;
            }
            Context app = app();
            if (app == null) {  // not up yet: later
                synchronized (JobSchedulerImpl.this) {
                    running = false;
                }
                handler().postDelayed(start, 1000);
                return;
            }
            ComponentName c = info.getService();
            connection = new ServiceConnection() {
                @Override
                public void onServiceConnected(ComponentName name, IBinder binder) {
                    try {
                        service = Hidden.callStatic(Hidden.cls("android.app.job.IJobService$Stub"), "asInterface", binder);
                        params = parameters(Job.this);
                        Log.i(TAG, "job " + id() + " (" + c.getShortClassName() + ") starts");
                        Hidden.call(service, "startJob", params);
                    } catch (Throwable t) {
                        Log.w(TAG, "job " + id() + " (" + c.getShortClassName() + ") failed to start", t);
                        finished(Job.this, false);
                    }
                }

                @Override
                public void onServiceDisconnected(ComponentName name) {}
            };
            boolean bound = app.bindService(new Intent().setComponent(c), connection, Context.BIND_AUTO_CREATE);
            if (!bound) {
                Log.w(TAG, "job " + id() + ": cannot bind " + c);
                finished(this, false);
            }
        }
    }

    private final Map<Integer, Job> jobs = new HashMap<>();
    // (Created on first use: the services are installed before the main looper exists.)
    private Handler mainHandler;

    private synchronized Handler handler() {
        if (mainHandler == null) mainHandler = new Handler(Looper.getMainLooper());
        return mainHandler;
    }

    private static Context app() {
        try {
            return (Context) Hidden.callStatic(Hidden.cls("android.app.ActivityThread"), "currentApplication");
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

    private static int intArg(Object[] args) {
        for (Object a : args) {
            if (a instanceof Integer) return (Integer) a;
        }
        return -1;
    }

    // ---- IJobScheduler ----

    public int schedule(Object[] args) {
        JobInfo info = find(args, JobInfo.class);
        if (info == null) return 0;
        add(info, null);
        return RESULT_SUCCESS;
    }

    public int enqueue(Object[] args) {
        JobInfo info = find(args, JobInfo.class);
        if (info == null) return 0;
        add(info, find(args, JobWorkItem.class));
        return RESULT_SUCCESS;
    }

    public int scheduleAsPackage(Object[] args) {
        return schedule(args);
    }

    public void cancel(Object[] args) {
        int id = intArg(args);
        Job job;
        synchronized (this) {
            job = jobs.remove(id);
        }
        if (job != null) stop(job);
    }

    public void cancelAll(Object[] args) {
        List<Job> all;
        synchronized (this) {
            all = new ArrayList<>(jobs.values());
            jobs.clear();
        }
        for (Job j : all) stop(j);
    }

    public void cancelAllInNamespace(Object[] args) {
        cancelAll(args);
    }

    public synchronized JobInfo getPendingJob(Object[] args) {
        Job j = jobs.get(intArg(args));
        return j != null ? j.info : null;
    }

    public Object getAllPendingJobsInNamespace(Object[] args) {
        return slice(pending());
    }

    /** Android 14: Map<namespace, ParceledListSlice<JobInfo>>; before, the slice itself. */
    public Object getAllPendingJobs(Object[] args) {
        return slice(pending());
    }

    public int getPendingJobReason(Object[] args) {
        return 0;  // PENDING_JOB_REASON_UNDEFINED
    }

    public boolean canRunUserInitiatedJobs(Object[] args) {
        return true;
    }

    public boolean hasRunUserInitiatedJobsPermission(Object[] args) {
        return true;
    }

    private synchronized List<JobInfo> pending() {
        List<JobInfo> l = new ArrayList<>();
        for (Job j : jobs.values()) l.add(j.info);
        return l;
    }

    private static Object slice(List<JobInfo> list) {
        return Hidden.newInstance(Hidden.cls("android.content.pm.ParceledListSlice"), list);
    }

    // ---- scheduling ----

    private void add(JobInfo info, JobWorkItem work) {
        Job job;
        boolean plan;
        synchronized (this) {
            job = jobs.get(info.getId());
            if (job != null && work != null && !job.cancelled) {
                job.work.add(work);  // enqueue() onto a job that exists: more work for it
                plan = !job.running;
            } else {
                if (job != null) {
                    job.cancelled = true;
                    handler().removeCallbacks(job.start);
                    if (job.running) stop(job);
                }
                job = new Job(info);
                if (work != null) job.work.add(work);
                jobs.put(info.getId(), job);
                plan = true;
            }
        }
        if (plan) {
            long delay = info.isPeriodic() ? 0 : Math.max(0, info.getMinLatencyMillis());
            handler().removeCallbacks(job.start);
            handler().postDelayed(job.start, delay);
        }
    }

    private void stop(Job job) {
        job.cancelled = true;
        handler().removeCallbacks(job.start);
        if (job.service != null && job.params != null) {
            try {
                Hidden.call(job.service, "stopJob", job.params);
            } catch (Throwable t) {
                Log.w(TAG, "stopJob " + job.id(), t);
            }
        }
        unbind(job);
    }

    private void unbind(Job job) {
        ServiceConnection c = job.connection;
        job.connection = null;
        job.service = null;
        Context app = app();
        if (c != null && app != null) {
            handler().post(() -> {
                try {
                    app.unbindService(c);
                } catch (RuntimeException e) {
                    // already unbound
                }
            });
        }
    }

    private void finished(Job job, boolean reschedule) {
        Log.i(TAG, "job " + job.id() + " finished" + (reschedule ? " (retry)" : ""));
        long delay = -1;
        synchronized (this) {
            job.running = false;
            if (job.cancelled) {
                // stopped or replaced
            } else if (reschedule) {
                delay = Math.max(1000, job.info.getInitialBackoffMillis());
            } else if (job.info.isPeriodic()) {
                delay = Math.max(15 * 60 * 1000L, job.info.getIntervalMillis());
            } else if (!job.work.isEmpty()) {
                delay = 0;  // work enqueued while it ran
            } else {
                jobs.remove(job.id());
            }
        }
        unbind(job);
        if (delay >= 0) handler().postDelayed(job.start, delay);
    }

    private synchronized Job byId(int id) {
        return jobs.get(id);
    }

    // ---- JobParameters and its IJobCallback ----

    private Object parameters(Job job) throws Exception {
        IBinder callback = LocalService.create("android.app.job.IJobCallback", new Callback());
        JobInfo i = job.info;
        PersistableBundle extras = i.getExtras();
        for (Constructor<?> k : Class.forName("android.app.job.JobParameters").getDeclaredConstructors()) {
            Class<?>[] p = k.getParameterTypes();
            if (p.length < 4 || p[0] != IBinder.class) continue;
            Object[] a = new Object[p.length];
            int ints = 0;
            for (int n = 0; n < p.length; n++) {
                Class<?> t = p[n];
                if (n == 0) a[n] = callback;
                else if (t == String.class) a[n] = null;  // the namespace (default) and trigger authorities
                else if (t == int.class) a[n] = ints++ == 0 ? i.getId() : 0;
                else if (t == PersistableBundle.class) a[n] = extras;
                else if (t == android.os.Bundle.class) a[n] = i.getTransientExtras();
                else if (t == android.content.ClipData.class) a[n] = i.getClipData();
                else if (t == boolean.class) a[n] = false;
                else a[n] = null;  // trigger URIs/authorities, Network
            }
            k.setAccessible(true);
            return k.newInstance(a);
        }
        throw new IllegalStateException("no JobParameters constructor");
    }

    /** android.app.job.IJobCallback: the job's calls back to the scheduler. */
    final class Callback {
        public void acknowledgeStartMessage(int jobId, boolean ongoing) {
            Job job = byId(jobId);
            if (job != null && !ongoing) finished(job, false);  // onStartJob returned false: done
        }

        public void acknowledgeStopMessage(int jobId, boolean reschedule) {
            Job job = byId(jobId);
            if (job != null) finished(job, reschedule);
        }

        public void jobFinished(int jobId, boolean reschedule) {
            Job job = byId(jobId);
            if (job != null) finished(job, reschedule);
        }

        public JobWorkItem dequeueWork(int jobId) {
            Job job = byId(jobId);
            if (job == null) return null;
            synchronized (JobSchedulerImpl.this) {
                if (job.work.isEmpty()) return null;
                JobWorkItem w = job.work.remove(0);
                try {
                    Hidden.setField(w, "mWorkId", job.nextWorkId++);
                } catch (RuntimeException e) {
                    // (older field name)
                }
                return w;
            }
        }

        public boolean completeWork(int jobId, int workId) {
            return true;
        }
    }
}
