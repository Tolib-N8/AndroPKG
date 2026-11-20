package dev.andropkg.test.components;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.IBinder;
import android.util.Log;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Toast;

/**
 * App components through andropkg's ActivityManager: a started service (stopSelf), a bound
 * service, dynamic + manifest broadcast receivers, a PendingIntent, a toast and a WebView.
 * Logs "[components] ..." lines for scripts/test-mac.sh.
 */
public class MainActivity extends Activity {
    static final String TAG = "ComponentsTest";
    static final String PING = "dev.andropkg.test.PING";

    private final BroadcastReceiver dynamic = new BroadcastReceiver() {
        @Override
        public void onReceive(Context c, Intent i) {
            Log.i(TAG, "[components] dynamic receiver got " + i.getAction());
        }
    };

    private final ServiceConnection connection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder binder) {
            Log.i(TAG, "[components] bound: " + ((BoundService.Local) binder).ping());
            unbindService(this);
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {}
    };

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        WebView web = new WebView(this);
        web.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView view, String url) {
                Log.i(TAG, "[components] webview page finished");
            }
        });
        web.loadData("<h1>Components</h1><p>andropkg <b>WebView</b></p>", "text/html", "utf-8");
        setContentView(web);

        startService(new Intent(this, StartedService.class).putExtra("n", 7));
        bindService(new Intent(this, BoundService.class), connection, BIND_AUTO_CREATE);
        registerReceiver(dynamic, new IntentFilter(PING), RECEIVER_NOT_EXPORTED);
        sendBroadcast(new Intent(PING).setPackage(getPackageName()));
        try {
            PendingIntent.getBroadcast(this, 1, new Intent(this, PingReceiver.class).setAction("PI"),
                    PendingIntent.FLAG_IMMUTABLE).send();
        } catch (PendingIntent.CanceledException e) {
            Log.e(TAG, "pending intent canceled", e);
        }
        Toast.makeText(this, "hello from components", Toast.LENGTH_SHORT).show();
        if (System.getProperty("andropkg.test.autofinish") != null) {
            getWindow().getDecorView().postDelayed(this::finish, 3000);
        }
    }

    @Override
    protected void onDestroy() {
        unregisterReceiver(dynamic);
        super.onDestroy();
    }
}
