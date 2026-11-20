package dev.andropkg.system;

import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import java.util.ArrayList;

/**
 * The host's keyboard as the app's input method. There is no IME process: the focused text
 * editor's InputConnection is used directly, the way an IME's session would use it.
 * Called by native code (runtime/graphics/window_jni.cc) on the app's main thread.
 */
final class HostIme {
    private static View connectedView;
    private static InputConnection connection;
    private static boolean keyboardShown;

    private HostIme() {}

    /** After each touch: show the host keyboard iff a text editor has focus. */
    static void onTouchUp() {
        View focused = focusedView();
        boolean editor = focused != null && focused.isEnabled() && focused.onCheckIsTextEditor();
        if (editor != keyboardShown) {
            keyboardShown = editor;
            nativeSetKeyboardVisible(editor);
        }
    }

    static void commitText(String text) {
        InputConnection ic = connection();
        if (ic != null) ic.commitText(text, 1);
    }

    static void deleteBackward() {
        InputConnection ic = connection();
        if (ic == null) return;
        CharSequence selected = ic.getSelectedText(0);
        if (selected != null && selected.length() > 0) {
            ic.commitText("", 1);
        } else {
            ic.deleteSurroundingText(1, 0);
        }
    }

    static void enter() {
        InputConnection ic = connection();
        if (ic == null) return;
        ic.sendKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER));
        ic.sendKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER));
    }

    private static InputConnection connection() {
        View focused = focusedView();
        if (focused == null || !focused.onCheckIsTextEditor()) return null;
        if (focused != connectedView) {
            connectedView = focused;
            connection = focused.onCreateInputConnection(new EditorInfo());
        }
        return connection;
    }

    /** The focused view of the top window (WindowManagerGlobal's last root). */
    @SuppressWarnings("unchecked")
    private static View focusedView() {
        Object wmg = Hidden.callStatic(Hidden.cls("android.view.WindowManagerGlobal"), "getInstance");
        ArrayList<View> views = (ArrayList<View>) Hidden.getField(wmg, "mViews");
        synchronized (Hidden.getField(wmg, "mLock")) {
            for (int i = views.size() - 1; i >= 0; i--) {
                View root = views.get(i);
                if (root.hasWindowFocus() || i == views.size() - 1) {
                    View f = root.findFocus();
                    if (f != null) return f;
                }
            }
        }
        return null;
    }

    private static native void nativeSetKeyboardVisible(boolean visible);
}
