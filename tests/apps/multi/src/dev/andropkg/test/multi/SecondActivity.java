package dev.andropkg.test.multi;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

public class SecondActivity extends Activity {
    @Override protected void onCreate(Bundle b) { super.onCreate(b); Trace.log("Second.onCreate"); }
    @Override protected void onResume() {
        super.onResume();
        Trace.log("Second.onResume -> setResult + finish");
        setResult(RESULT_OK, new Intent().putExtra("picked", "second note"));
        finish();
    }
    @Override protected void onPause() { super.onPause(); Trace.log("Second.onPause"); }
    @Override protected void onStop() { super.onStop(); Trace.log("Second.onStop"); }
    @Override protected void onDestroy() { super.onDestroy(); Trace.log("Second.onDestroy"); }
}
