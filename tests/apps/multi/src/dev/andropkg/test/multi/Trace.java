package dev.andropkg.test.multi;

import android.util.Log;

final class Trace {
    static void log(String s) {
        Log.i("MultiTest", s);
        System.out.println("[multi] " + s);
    }
}
