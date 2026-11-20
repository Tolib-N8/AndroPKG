package dev.andropkg.test.hello;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.util.Log;
import android.widget.TextView;

/** M2d test: a real layout (LinearLayout, TextViews, a colored View) drawn into a window. */
public class MainActivity extends Activity {
    private static final String TAG = "HelloTest";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.main);
        DisplayMetrics dm = getResources().getDisplayMetrics();
        ((TextView) findViewById(R.id.info)).setText(
                "Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")\n"
                + dm.widthPixels + "x" + dm.heightPixels + " px, " + dm.densityDpi + " dpi");
        Log.i(TAG, "onCreate: content set, display " + dm.widthPixels + "x" + dm.heightPixels);
        getWindow().getDecorView().getViewTreeObserver().addOnDrawListener(() -> Log.i(TAG, "onDraw"));
        // Tests exit on their own; an interactive run keeps the window up.
        if (System.getProperty("andropkg.test.autofinish") != null) {
            getWindow().getDecorView().postDelayed(this::finish, 1500);
        }
    }
}
