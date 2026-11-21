package dev.andropkg.test.platform;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

/** singleTop: the second intent of startActivities() arrives through onNewIntent. */
public class SecondActivity extends Activity {
    static boolean finished;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        Log.i(MainActivity.TAG, "[platform] second created n=" + getIntent().getIntExtra("n", 0));
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        Log.i(MainActivity.TAG, "[platform] second new intent n=" + intent.getIntExtra("n", 0));
        finished = true;
        finish();
    }
}
