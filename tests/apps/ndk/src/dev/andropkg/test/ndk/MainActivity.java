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
    static native java.nio.ByteBuffer directbuf();
    static native String directcheck();
    static native String aaudio();
    static native String opensl();
    static native String stackargs(long a, long b, int c, int d, int e, int f, int g, int h, int i, int j, byte k,
            short l, char m, float n, double o);
    static native String platform(android.content.res.AssetManager am, android.graphics.Bitmap bmp);

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
            android.graphics.Bitmap bmp = android.graphics.Bitmap.createBitmap(4, 3, android.graphics.Bitmap.Config.ARGB_8888);
            java.nio.ByteBuffer bb = directbuf();
            byte[] got = new byte[16];
            bb.get(got);
            int sum = 0;
            for (byte x : got) sum += x;
            bb.clear();
            bb.asIntBuffer().put(new int[] {0x11223344});  // big-endian view: bytes 11 22 33 44
            Log.i(TAG, "[ndk] aaudio: " + aaudio());
            Log.i(TAG, "[ndk] opensl: " + opensl());
            Log.i(TAG, "[ndk] stackargs: " + stackargs(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, (byte) -11, (short) 12, 'A',
                    1.5f, 2.5));
            Log.i(TAG, "[ndk] direct: cap=" + bb.capacity() + " sum=" + sum + " " + directcheck());
            Log.i(TAG, "[ndk] platform: " + platform(getAssets(), bmp) + " pixel=" + Integer.toHexString(bmp.getPixel(2, 1)));
            System.loadLibrary("ndkcpp");
            Log.i(TAG, "[ndk] cpp: " + cpp(5));
            for (int run = 1; run <= 2; run++) Log.i(TAG, "[ndk] exec" + run + ": " + exec());
        } catch (Throwable t) {
            Log.e(TAG, "[ndk] failed", t);
        }
        if (System.getProperty("andropkg.test.autofinish") != null) finish();
    }

    /** Runs the bundled executable libndkexe.so as a process (stdin, stdout, stderr, exit code). */
    private String exec() throws Exception {
        String exe = getApplicationInfo().nativeLibraryDir + "/libndkexe.so";
        Process p = new ProcessBuilder(exe, "hello").start();
        p.getOutputStream().write("from java\n".getBytes());
        p.getOutputStream().close();
        String out = new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream())).readLine();
        String err = new java.io.BufferedReader(new java.io.InputStreamReader(p.getErrorStream())).readLine();
        return out + " stderr=" + err + " exit=" + p.waitFor();
    }

    @Override
    protected void onDestroy() {
        Log.i(TAG, "[ndk] done");
        super.onDestroy();
    }
}
