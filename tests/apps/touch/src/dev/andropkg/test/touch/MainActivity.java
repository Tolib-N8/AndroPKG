package dev.andropkg.test.touch;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;

/**
 * Multi-touch: a two-finger pinch (ANDROPKG_TEST_SCRIPT "pinch") must arrive as DOWN,
 * POINTER_DOWN, moves with both fingers, POINTER_UP, UP, and make ScaleGestureDetector zoom.
 */
public class MainActivity extends Activity {
    static final String TAG = "TouchTest";
    int maxPointers, pointerDowns, pointerUps, moves;
    float scale = 1f;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        ScaleGestureDetector detector = new ScaleGestureDetector(this, new ScaleGestureDetector.SimpleOnScaleGestureListener() {
            @Override public boolean onScale(ScaleGestureDetector d) {
                scale *= d.getScaleFactor();
                return true;
            }
        });
        View view = new View(this) {
            @Override public boolean onTouchEvent(MotionEvent e) {
                detector.onTouchEvent(e);
                maxPointers = Math.max(maxPointers, e.getPointerCount());
                switch (e.getActionMasked()) {
                    case MotionEvent.ACTION_POINTER_DOWN: pointerDowns++; break;
                    case MotionEvent.ACTION_POINTER_UP: pointerUps++; break;
                    case MotionEvent.ACTION_MOVE: if (e.getPointerCount() == 2) moves += 1 + e.getHistorySize(); break;
                    case MotionEvent.ACTION_UP:
                        Log.i(TAG, "[touch] pointers=" + maxPointers + " pointerDown=" + pointerDowns + " pointerUp="
                                + pointerUps + " twoFingerMoves=" + (moves > 5) + " zoomed=" + (scale > 1.5f));
                        break;
                }
                return true;
            }
        };
        view.setBackgroundColor(0xff202020);
        setContentView(view);
    }
}
