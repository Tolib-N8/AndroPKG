package dev.andropkg.test.components;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

public class StartedService extends Service {
    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Log.i(MainActivity.TAG, "[components] service started n=" + intent.getIntExtra("n", 0) + " id=" + startId);
        stopSelf(startId);
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        Log.i(MainActivity.TAG, "[components] service destroyed");
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
