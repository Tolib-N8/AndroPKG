package dev.andropkg.test.multi;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;

/** M3 test: own Application, own ContentProvider, startActivityForResult and the back stack. */
public class MainActivity extends Activity {
    private boolean started;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        Trace.log("Main.onCreate");
        try (Cursor c = getContentResolver().query(
                Uri.parse("content://dev.andropkg.test.multi.notes/notes"), null, null, null, null)) {
            StringBuilder sb = new StringBuilder();
            while (c.moveToNext()) sb.append(c.getString(1)).append(';');
            Trace.log("provider rows: " + sb);
        }
    }
    @Override protected void onStart() { super.onStart(); Trace.log("Main.onStart"); }
    @Override protected void onRestart() { super.onRestart(); Trace.log("Main.onRestart"); }
    @Override protected void onResume() {
        super.onResume();
        Trace.log("Main.onResume");
        if (!started) {
            started = true;
            startActivityForResult(new Intent("dev.andropkg.test.multi.PICK"), 7);
        }
    }
    @Override protected void onPause() { super.onPause(); Trace.log("Main.onPause"); }
    @Override protected void onStop() { super.onStop(); Trace.log("Main.onStop"); }
    @Override protected void onDestroy() { super.onDestroy(); Trace.log("Main.onDestroy"); }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        Trace.log("Main.onActivityResult request=" + request + " result=" + result
                + " picked=" + (data != null ? data.getStringExtra("picked") : null));
        finish();
    }
}
