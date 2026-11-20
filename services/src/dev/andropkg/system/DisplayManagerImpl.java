package dev.andropkg.system;

import android.graphics.Point;
import android.view.Display;
import java.lang.reflect.Array;

/**
 * android.hardware.display.IDisplayManager: one built-in display, the iOS screen the app runs
 * on. Its geometry comes from system properties the host sets (andropkg.display.*).
 */
final class DisplayManagerImpl {
    final int width = intProp("andropkg.display.width", 1080);
    final int height = intProp("andropkg.display.height", 2400);
    final int dpi = intProp("andropkg.display.dpi", 420);
    final float refreshRate = 60f;
    // System bars: the host's safe area (iOS status bar / Dynamic Island, home indicator), in
    // pixels. Default (macOS dev loop): a 24 dp status bar, no navigation bar.
    final int insetTop = intProp("andropkg.display.insetTop", dpi * 24 / 160);
    final int insetBottom = intProp("andropkg.display.insetBottom", 0);

    private static int intProp(String key, int def) {
        try {
            return Integer.parseInt(System.getProperty(key, String.valueOf(def)));
        } catch (NumberFormatException e) {
            return def;
        }
    }

    public Object getDisplayInfo(int displayId) {
        if (displayId != Display.DEFAULT_DISPLAY) return null;
        Object info = Hidden.newInstance(Hidden.cls("android.view.DisplayInfo"));
        Object mode = Hidden.newInstance(Display.Mode.class, 1, width, height, refreshRate);
        Object modes = Array.newInstance(Display.Mode.class, 1);
        Array.set(modes, 0, mode);
        set(info, "layerStack", 0);
        set(info, "type", 1 /* Display.TYPE_INTERNAL */);
        set(info, "displayId", Display.DEFAULT_DISPLAY);
        set(info, "name", "Built-in Screen");
        set(info, "uniqueId", "local:0");
        set(info, "appWidth", width);
        set(info, "appHeight", height);
        set(info, "smallestNominalAppWidth", Math.min(width, height));
        set(info, "smallestNominalAppHeight", Math.min(width, height));
        set(info, "largestNominalAppWidth", Math.max(width, height));
        set(info, "largestNominalAppHeight", Math.max(width, height));
        set(info, "logicalWidth", width);
        set(info, "logicalHeight", height);
        set(info, "rotation", 0);
        set(info, "modeId", 1);
        set(info, "defaultModeId", 1);
        set(info, "renderFrameRate", refreshRate);
        set(info, "supportedModes", modes);
        set(info, "colorMode", 0);
        set(info, "supportedColorModes", new int[] {0});
        set(info, "userDisabledHdrTypes", new int[0]);
        set(info, "logicalDensityDpi", dpi);
        set(info, "physicalXDpi", (float) dpi);
        set(info, "physicalYDpi", (float) dpi);
        set(info, "presentationDeadlineNanos", 1_000_000_000L / 60);
        set(info, "state", Display.STATE_ON);
        set(info, "committedState", Display.STATE_ON);
        set(info, "ownerPackageName", "");
        set(info, "brightnessMinimum", 0f);
        set(info, "brightnessMaximum", 1f);
        set(info, "brightnessDefault", 0.5f);
        set(info, "hdrSdrRatio", 1f);
        set(info, "flags", 1 /* FLAG_SUPPORTS_PROTECTED_BUFFERS */ | 2 /* FLAG_SECURE */);
        return info;
    }

    public int[] getDisplayIds(boolean includeDisabled) { return new int[] {Display.DEFAULT_DISPLAY}; }
    public Point getStableDisplaySize() { return new Point(width, height); }
    public boolean isUidPresentOnDisplay(int uid, int displayId) { return true; }

    private static void set(Object target, String field, Object value) {
        try {
            Hidden.setField(target, field, value);
        } catch (IllegalStateException e) {
            // Field not in this release.
        }
    }
}
