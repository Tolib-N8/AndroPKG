package dev.andropkg.test.lifecycle;

import android.app.Activity;
import android.app.Application;
import android.os.Bundle;
import android.util.Log;

/** M2c: a real APK's Activity driven through ActivityThread's lifecycle by andropkg. */
public class MainActivity extends Activity {
    private static final String TAG = "LifecycleTest";

    private void step(String s) {
        Log.i(TAG, s);
        System.out.println("[lifecycle] " + s);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Application app = getApplication();
        int id = getResources().getIdentifier("greeting", "string", getPackageName());
        step("onCreate package=" + getPackageName() + " app=" + app.getClass().getName()
                + " label=\"" + getApplicationInfo().loadLabel(getPackageManager()) + "\"");
        step("own resource: \"" + getString(id) + "\"; filesDir=" + getFilesDir());
    }

    @Override protected void onStart() { super.onStart(); step("onStart"); }

    @Override
    protected void onResume() {
        super.onResume();
        step("onResume -> finish()");
        finish();
    }

    @Override protected void onPause() { super.onPause(); step("onPause"); }
    @Override protected void onStop() { super.onStop(); step("onStop"); }
    @Override protected void onDestroy() { super.onDestroy(); step("onDestroy"); }
}
