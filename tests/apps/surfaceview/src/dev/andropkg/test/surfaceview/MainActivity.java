package dev.andropkg.test.surfaceview;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

/** A SurfaceView (software, from its own thread) under a plain view: the window punches a hole
 * where the SurfaceView is, and its layer is composed below the window. A TextView overlaps it. */
public class MainActivity extends Activity implements SurfaceHolder.Callback {
    static final String TAG = "SurfaceViewTest";

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        TextView top = new TextView(this);
        top.setText("above the SurfaceView");
        top.setBackgroundColor(Color.WHITE);
        root.addView(top, new LinearLayout.LayoutParams(-1, 400));
        FrameLayout frame = new FrameLayout(this);
        SurfaceView sv = new SurfaceView(this);
        sv.getHolder().addCallback(this);
        frame.addView(sv, new FrameLayout.LayoutParams(-1, -1));
        TextView overlay = new TextView(this);  // a view over the SurfaceView (window content)
        overlay.setBackgroundColor(Color.YELLOW);
        frame.addView(overlay, new FrameLayout.LayoutParams(200, 200, Gravity.BOTTOM | Gravity.END));
        root.addView(frame, new LinearLayout.LayoutParams(-1, 0, 1));
        if (System.getProperty("andropkg.test.autofinish") != null) {  // after the pixel checks
            new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(this::finish, 6000);
        }
        setContentView(root);
    }

    @Override public void surfaceCreated(SurfaceHolder h) { Log.i(TAG, "[sv] created"); }

    @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int hh) {
        Log.i(TAG, "[sv] changed " + w + "x" + hh);
        new Thread(() -> {
            Canvas c = h.lockCanvas();
            if (c == null) { Log.i(TAG, "[sv] lockCanvas null"); return; }
            c.drawColor(Color.RED);
            Paint p = new Paint();
            p.setColor(Color.GREEN);
            c.drawRect(0, 0, 100, 100, p);
            h.unlockCanvasAndPost(c);
            Log.i(TAG, "[sv] drawn");
        }).start();
    }

    @Override public void surfaceDestroyed(SurfaceHolder h) { Log.i(TAG, "[sv] destroyed"); }
}
