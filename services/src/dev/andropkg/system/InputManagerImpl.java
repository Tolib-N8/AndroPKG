package dev.andropkg.system;

import android.view.InputDevice;
import android.view.KeyCharacterMap;

/**
 * android.hardware.input.IInputManager: the input devices an app can see. The virtual
 * keyboard (KeyCharacterMap.VIRTUAL_KEYBOARD) always exists on Android: KeyEvents from the
 * IME and KeyCharacterMap.load(-1) need it. Touches come from the iOS screen.
 */
final class InputManagerImpl {
    private Object virtualKeyboard;

    public synchronized Object getInputDevice(int id) {
        if (id != KeyCharacterMap.VIRTUAL_KEYBOARD) return null;
        if (virtualKeyboard == null) {
            Object b = Hidden.newInstance(Hidden.cls("android.view.InputDevice$Builder"));
            Hidden.call(b, "setId", id);
            Hidden.call(b, "setName", "Virtual");
            Hidden.call(b, "setDescriptor", "andropkg-virtual-keyboard");
            Hidden.call(b, "setSources", InputDevice.SOURCE_KEYBOARD);
            Hidden.call(b, "setKeyboardType", InputDevice.KEYBOARD_TYPE_ALPHABETIC);
            Hidden.call(b, "setKeyCharacterMap", Hidden.callStatic(KeyCharacterMap.class, "obtainEmptyMap", id));
            virtualKeyboard = Hidden.call(b, "build");
        }
        return virtualKeyboard;
    }

    public int[] getInputDeviceIds() { return new int[] {KeyCharacterMap.VIRTUAL_KEYBOARD}; }
    public boolean isInTabletMode() { return false; }
    public int isInTabletModeState() { return 0; }  // SWITCH_STATE_OFF
}
