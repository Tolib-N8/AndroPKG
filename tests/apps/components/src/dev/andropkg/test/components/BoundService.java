package dev.andropkg.test.components;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.util.Log;

public class BoundService extends Service {
    final class Local extends Binder {
        String ping() {
            return "pong";
        }
    }

    @Override
    public IBinder onBind(Intent intent) {
        return new Local();
    }

    @Override
    public void onDestroy() {
        Log.i(MainActivity.TAG, "[components] bound service destroyed");
    }
}
