package dev.andropkg.system;

import android.content.pm.ProviderInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.Process;
import java.util.HashMap;
import java.util.Map;

/**
 * The "settings" content provider (Settings.Global/Secure/System) in process. Settings is read
 * through IContentProvider.call("GET_<table>", name) and written with "PUT_<table>"; values the
 * app writes persist for the process lifetime.
 */
final class SettingsProvider {
    static final String AUTHORITY = "settings";

    private final Map<String, Map<String, String>> tables = new HashMap<>();

    SettingsProvider() {
        Map<String, String> global = table("global");
        global.put("device_provisioned", "1");
        global.put("window_animation_scale", "1.0");
        global.put("transition_animation_scale", "1.0");
        global.put("animator_duration_scale", "1.0");
        global.put("auto_time", "1");
        global.put("auto_time_zone", "1");
        global.put("adb_enabled", "0");
        global.put("development_settings_enabled", "0");
        global.put("airplane_mode_on", "0");
        Map<String, String> secure = table("secure");
        secure.put("user_setup_complete", "1");
        secure.put("android_id", "a11d0a2f0d0e0001");
        secure.put("default_input_method", "");
        Map<String, String> system = table("system");
        system.put("font_scale", "1.0");
        system.put("screen_brightness", "128");
        system.put("time_12_24", "24");
        system.put("haptic_feedback_enabled", "1");
        system.put("sound_effects_enabled", "0");
    }

    private Map<String, String> table(String name) {
        return tables.computeIfAbsent(name, k -> new HashMap<>());
    }

    // IContentProvider.call(AttributionSource, String authority, String method, String arg, Bundle)
    public synchronized Bundle call(Object attributionSource, String authority, String method, String arg,
            Bundle extras) {
        int sep = method.indexOf('_');
        if (sep < 0) return null;
        String op = method.substring(0, sep);
        Map<String, String> t = table(method.substring(sep + 1).toLowerCase());
        Bundle result = new Bundle();
        switch (op) {
            case "GET":
                result.putString("value", t.get(arg));
                return result;
            case "PUT":
                t.put(arg, extras == null ? null : extras.getString("value"));
                return result;
            case "DELETE":
                result.putInt("result", t.remove(arg) == null ? 0 : 1);
                return result;
            default:
                return null;
        }
    }

    /** android.app.ContentProviderHolder handed out by IActivityManager.getContentProvider. */
    Object holder() {
        ProviderInfo info = new ProviderInfo();
        info.authority = AUTHORITY;
        info.name = "com.android.providers.settings.SettingsProvider";
        info.packageName = "com.android.providers.settings";
        info.exported = true;
        info.applicationInfo = new android.content.pm.ApplicationInfo();
        info.applicationInfo.packageName = info.packageName;
        info.applicationInfo.uid = Process.SYSTEM_UID;
        Object holder = Hidden.newInstance(Hidden.cls("android.app.ContentProviderHolder"), info);
        LocalService.LocalBinder binder = LocalService.create("android.content.IContentProvider", this);
        Hidden.setField(holder, "provider", binder.iface);
        Hidden.setField(holder, "connection", new Binder());
        Hidden.setField(holder, "noReleaseNeeded", true);
        return holder;
    }
}
