package dev.andropkg.test.ndk;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

/**
 * Android NDK libraries (arm64 ELF, built with the real NDK) through andropkg's NDK layer:
 * a C library (JNI_OnLoad + RegisterNatives, liblog, printf formats, variadic JNI calls,
 * pthreads, files) and a C++ one on libc++_shared (exceptions, std::thread, thread_local).
 * Logs "[ndk] ..." lines for scripts/test-mac.sh.
 */
public class MainActivity extends Activity {
    static final String TAG = "NdkTest";

    static native String hello(int n, double d);
    static native int callback(MainActivity a);
    static native int threads(int n);
    static native String files(String dir);
    static native String cpp(int n);

    /** Called from native code through CallIntMethod(obj, mid, int, long, float, String). */
    int combine(int a, long b, float c, String s) {
        return a + (int) b + (int) (c * 10) + s.length();
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        TextView text = new TextView(this);
        text.setText("ndk");
        setContentView(text);
        try {
            System.loadLibrary("ndktest");
            Log.i(TAG, "[ndk] hello: " + hello(42, 2.5));
            Log.i(TAG, "[ndk] callback: " + callback(this));
            Log.i(TAG, "[ndk] threads: " + threads(4));
            Log.i(TAG, "[ndk] files: " + files(getFilesDir().getPath()));
            System.loadLibrary("ndkcpp");
            Log.i(TAG, "[ndk] cpp: " + cpp(5));
        } catch (Throwable t) {
            Log.e(TAG, "[ndk] failed", t);
        }
        if (System.getProperty("andropkg.test.autofinish") != null) finish();
    }

    @Override
    protected void onDestroy() {
        Log.i(TAG, "[ndk] done");
        super.onDestroy();
    }
}
