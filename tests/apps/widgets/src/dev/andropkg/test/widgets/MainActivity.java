package dev.andropkg.test.widgets;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/** M3 test: stock Material widgets, an action bar and a scrolling list. */
public class MainActivity extends Activity {
    private static final String TAG = "WidgetsTest";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.main);
        LinearLayout list = findViewById(R.id.list);
        for (int i = 1; i <= 30; i++) {
            TextView row = new TextView(this);
            row.setText("Row " + i);
            row.setTextSize(20);
            row.setPadding(0, 24, 0, 24);
            list.addView(row);
        }
        Button button = findViewById(R.id.button);
        button.setOnClickListener(v -> {
            button.setText("Clicked");
            Log.i(TAG, "button clicked");
        });
        Log.i(TAG, "onCreate done");
        if (System.getProperty("andropkg.test.autofinish") != null) {
            getWindow().getDecorView().postDelayed(this::finish, 2500);
        }
    }
}
