package dev.andropkg.test.windows;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.widget.Button;
import android.widget.TextView;

/**
 * Windows above the activity: a dialog (centered, dimmed, takes the touch on its button), then
 * a second one cancelled by a touch outside it. Logs "[windows] ..." for scripts/test-mac.sh.
 */
public class MainActivity extends Activity {
    private static final String TAG = "WindowsTest";

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        TextView text = new TextView(this);
        text.setText("activity under the dialogs");
        text.setGravity(Gravity.CENTER);
        setContentView(text);
        text.post(this::first);
    }

    private void first() {
        Button big = new Button(this);
        big.setText("dialog button");
        big.setMinHeight((int) (300 * getResources().getDisplayMetrics().density));
        AlertDialog d = new AlertDialog.Builder(this).setTitle("first").setView(big).create();
        big.setOnClickListener(v -> {
            Log.i(TAG, "[windows] dialog button clicked");
            d.dismiss();
        });
        d.setOnDismissListener(x -> second());
        d.show();
        Log.i(TAG, "[windows] first dialog shown");
    }

    private void second() {
        AlertDialog d = new AlertDialog.Builder(this).setTitle("second").setMessage("tap outside").create();
        d.setCanceledOnTouchOutside(true);
        d.setOnCancelListener(x -> {
            Log.i(TAG, "[windows] second dialog cancelled by outside touch");
            if (System.getProperty("andropkg.test.autofinish") != null) finish();
        });
        d.show();
        Log.i(TAG, "[windows] second dialog shown");
    }
}
