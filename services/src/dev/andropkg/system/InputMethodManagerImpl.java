package dev.andropkg.system;

/**
 * com.android.internal.view.IInputMethodManager. No Android input method is installed: text
 * comes from the host keyboard through HostIme. Focus changes are answered with
 * InputBindResult.NO_IME, what a device without an IME reports.
 */
final class InputMethodManagerImpl {
    public Object startInputOrWindowGainedFocus(Object[] args) {
        return Hidden.getField(Hidden.cls("com.android.internal.inputmethod.InputBindResult"), "NO_IME");
    }

    public java.util.List<Object> getInputMethodList(Object[] args) { return new java.util.ArrayList<>(); }
    public java.util.List<Object> getEnabledInputMethodList(Object[] args) { return new java.util.ArrayList<>(); }
}
