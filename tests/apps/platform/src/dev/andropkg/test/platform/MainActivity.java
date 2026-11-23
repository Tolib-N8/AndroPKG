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
        // Config.HARDWARE (image loaders such as Coil use it): pixels must survive the copy.
        android.graphics.Bitmap src = android.graphics.Bitmap.createBitmap(4, 4, android.graphics.Bitmap.Config.ARGB_8888);
        src.eraseColor(0xff00ff00);
        android.graphics.Bitmap hw = src.copy(android.graphics.Bitmap.Config.HARDWARE, false);
        android.graphics.Bitmap back = hw == null ? null : hw.copy(android.graphics.Bitmap.Config.ARGB_8888, false);
        Log.i(TAG, "[platform] hardware bitmap pixel=" + (back == null ? "null" : Integer.toHexString(back.getPixel(1, 1))));
        // Bitmaps in parcels (MediaSession queues, notifications, Intents carry them).
        android.os.Parcel parcel = android.os.Parcel.obtain();
        src.writeToParcel(parcel, 0);
        parcel.setDataPosition(0);
        android.graphics.Bitmap copy = android.graphics.Bitmap.CREATOR.createFromParcel(parcel);
        parcel.recycle();
        Log.i(TAG, "[platform] parcel bitmap " + copy.getWidth() + "x" + copy.getHeight() + " pixel="
                + Integer.toHexString(copy.getPixel(2, 2)));
        // RenderNodes drawn into a software canvas (Compose's GraphicsLayer does this).
        android.graphics.RenderNode outer = new android.graphics.RenderNode("outer");
        android.graphics.RenderNode inner = new android.graphics.RenderNode("inner");
        inner.setPosition(0, 0, 4, 4);
        inner.setTranslationX(4);
        android.graphics.RecordingCanvas ic = inner.beginRecording();
        ic.drawColor(0xff0000ff);
        inner.endRecording();
        outer.setPosition(0, 0, 8, 4);
        android.graphics.RecordingCanvas oc = outer.beginRecording();
        oc.drawColor(0xffff0000);
        oc.drawRenderNode(inner);
        outer.endRecording();
        android.graphics.Bitmap target = android.graphics.Bitmap.createBitmap(8, 4, android.graphics.Bitmap.Config.ARGB_8888);
        String rn;
        try {
            new android.graphics.Canvas(target).drawRenderNode(outer);
            rn = Integer.toHexString(target.getPixel(1, 1)) + "," + Integer.toHexString(target.getPixel(6, 1));
        } catch (RuntimeException e) {
            rn = e.toString();
        }
        Log.i(TAG, "[platform] software renderNode " + rn);
        // A RenderEffect only applies through a layer: the node turns green if it has one.
        android.graphics.RenderNode fx = new android.graphics.RenderNode("fx");
        fx.setPosition(0, 0, 8, 4);
        android.graphics.RecordingCanvas fc = fx.beginRecording();
        fc.drawColor(0xffff0000);
        fx.endRecording();
        fx.setRenderEffect(android.graphics.RenderEffect.createColorFilterEffect(
                new android.graphics.PorterDuffColorFilter(0xff00ff00, android.graphics.PorterDuff.Mode.SRC_IN)));
        android.graphics.Bitmap fxTarget = android.graphics.Bitmap.createBitmap(8, 4, android.graphics.Bitmap.Config.ARGB_8888);
        new android.graphics.Canvas(fxTarget).drawRenderNode(fx);
        Log.i(TAG, "[platform] renderNode layer effect " + Integer.toHexString(fxTarget.getPixel(3, 2)));
        // Network service discovery: a manager (no exception storm), discovery that finds nothing.
        android.net.nsd.NsdManager nsd = getSystemService(android.net.nsd.NsdManager.class);
        Log.i(TAG, "[platform] nsd manager=" + (nsd != null));
        try {  // Spotify: discovery through the INsdServiceConnector sub-service
            nsd.discoverServices("_spotify-connect._tcp", android.net.nsd.NsdManager.PROTOCOL_DNS_SD,
                    new android.net.nsd.NsdManager.DiscoveryListener() {
                        public void onStartDiscoveryFailed(String t, int e) {}
                        public void onStopDiscoveryFailed(String t, int e) {}
                        public void onDiscoveryStarted(String t) {}
                        public void onDiscoveryStopped(String t) {}
                        public void onServiceFound(android.net.nsd.NsdServiceInfo i) {}
                        public void onServiceLost(android.net.nsd.NsdServiceInfo i) {}
                    });
            Log.i(TAG, "[platform] nsd discover=true");
        } catch (RuntimeException e) {
            Log.i(TAG, "[platform] nsd discover=" + e);
        }

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
