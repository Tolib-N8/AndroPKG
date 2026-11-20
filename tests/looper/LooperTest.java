// M2a smoke test: the Android framework's message loop (Looper/Handler/MessageQueue, which
// sits on libutils' Looper -> epoll) plus basic framework natives, without system services.
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

public class LooperTest {
    public static void main(String[] args) {
        try {
            run();
        } catch (Throwable t) {
            System.out.println("FAILED: " + t);
            t.printStackTrace(System.out);
        }
    }

    static void run() {
        Log.i("andropkg", "framework Log works, uptime=" + SystemClock.uptimeMillis());
        Looper.prepare();  // (the main looper may not quit, as on Android)
        Handler h = new Handler(Looper.myLooper());
        long start = SystemClock.uptimeMillis();
        final int[] count = {0};
        Runnable tick = new Runnable() {
            public void run() {
                count[0]++;
                System.out.println("tick " + count[0] + " at +" + (SystemClock.uptimeMillis() - start) + " ms");
                if (count[0] < 3) h.postDelayed(this, 50); else Looper.myLooper().quitSafely();
            }
        };
        h.post(tick);
        // A message posted from another thread wakes the loop through the MessageQueue fd.
        new Thread(() -> h.post(() -> System.out.println("message from background thread"))).start();
        Looper.loop();
        System.out.println("looper done: " + count[0] + " ticks");
    }
}
