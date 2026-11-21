package dev.andropkg.test.platform;

import android.app.Activity;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.Intent;
import android.content.res.AssetFileDescriptor;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

/**
 * Platform services an app expects from the system: connectivity (+ callback), notification
 * channels, uncompressed-asset file descriptors, the accelerometer, and startActivities with
 * a singleTop activity. Logs "[platform] ..." lines for scripts/test-mac.sh (whose script
 * feeds an "accel" sample).
 */
public class MainActivity extends Activity {
    static final String TAG = "PlatformTest";
    private SensorManager sensors;

    private final SensorEventListener accel = new SensorEventListener() {
        @Override
        public void onSensorChanged(SensorEvent e) {
            Log.i(TAG, "[platform] accel " + Math.round(e.values[0]) + "," + Math.round(e.values[1]) + ","
                    + Math.round(e.values[2]));
            sensors.unregisterListener(this);
        }

        @Override
        public void onAccuracyChanged(Sensor s, int accuracy) {}
    };

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        TextView text = new TextView(this);
        text.setText("platform");
        setContentView(text);

        ConnectivityManager cm = getSystemService(ConnectivityManager.class);
        Network net = cm.getActiveNetwork();
        NetworkCapabilities caps = net != null ? cm.getNetworkCapabilities(net) : null;
        Log.i(TAG, "[platform] network internet=" + (caps != null
                && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)));
        cm.registerDefaultNetworkCallback(new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network n) {
                Log.i(TAG, "[platform] network callback available");
                cm.unregisterNetworkCallback(this);
            }
        });

        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel("c1", "Channel one", NotificationManager.IMPORTANCE_LOW));
        NotificationChannel c1 = nm.getNotificationChannel("c1");
        Log.i(TAG, "[platform] channel " + (c1 != null ? c1.getName() + " importance=" + c1.getImportance() : "missing"));

        try (AssetFileDescriptor fd = getAssets().openFd("data.bin")) {
            Log.i(TAG, "[platform] asset fd length=" + fd.getLength());
        } catch (Exception e) {
            Log.e(TAG, "[platform] asset fd failed", e);
        }

        sensors = getSystemService(SensorManager.class);
        Sensor a = sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        Log.i(TAG, "[platform] accelerometer " + (a != null ? a.getName() : "missing"));
        if (a != null) sensors.registerListener(accel, a, SensorManager.SENSOR_DELAY_GAME);

        getWindow().getDecorView().postDelayed(() -> startActivities(new Intent[] {
            new Intent(this, SecondActivity.class).putExtra("n", 1),
            new Intent(this, SecondActivity.class).putExtra("n", 2),
        }), 1500);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (SecondActivity.finished && System.getProperty("andropkg.test.autofinish") != null) finish();
    }

    @Override
    protected void onDestroy() {
        Log.i(TAG, "[platform] done");
        super.onDestroy();
    }
}
