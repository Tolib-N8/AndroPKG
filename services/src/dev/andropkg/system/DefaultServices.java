package dev.andropkg.system;

import android.os.IBinder;
import android.util.Log;
import java.util.HashMap;
import java.util.Map;

/**
 * System services an app may look up that andropkg doesn't implement (yet). Each gets an
 * in-process proxy of its AIDL interface whose methods all return neutral defaults — the same
 * answers a device gives an app without the corresponding hardware or permission, and better
 * than a null binder, which most managers turn into NullPointerExceptions or crashes.
 */
final class DefaultServices {
    private static final String TAG = "andropkg";

    private DefaultServices() {}

    private static final String[][] INTERFACES = {
        {"window", "android.view.IWindowManager"},
        {"power", "android.os.IPowerManager"},
        {"input", "android.hardware.input.IInputManager"},
        {"input_method", "com.android.internal.view.IInputMethodManager"},
        {"uimode", "android.app.IUiModeManager"},
        {"alarm", "android.app.IAlarmManager"},
        {"connectivity", "android.net.IConnectivityManager"},
        {"network_management", "android.os.INetworkManagementService"},
        {"game", "android.app.IGameManagerService"},
        {"audio", "android.media.IAudioService"},
        {"clipboard", "android.content.IClipboard"},
        {"notification", "android.app.INotificationManager"},
        {"user", "android.os.IUserManager"},
        {"appops", "com.android.internal.app.IAppOpsService"},
        {"accessibility", "android.view.accessibility.IAccessibilityManager"},
        {"autofill", "android.view.autofill.IAutoFillManager"},
        {"content_capture", "android.view.contentcapture.IContentCaptureManager"},
        {"textservices", "com.android.internal.textservice.ITextServicesManager"},
        {"vibrator_manager", "android.os.IVibratorManagerService"},
        {"statusbar", "com.android.internal.statusbar.IStatusBarService"},
        {"device_policy", "android.app.admin.IDevicePolicyManager"},
        {"jobscheduler", "android.app.job.IJobScheduler"},
        {"location", "android.location.ILocationManager"},
        {"wifi", "android.net.wifi.IWifiManager"},
        {"mount", "android.os.storage.IStorageManager"},
        {"account", "android.accounts.IAccountManager"},
        {"content", "android.content.IContentService"},
        {"media_session", "android.media.session.ISessionManager"},
        {"telephony.registry", "com.android.internal.telephony.ITelephonyRegistry"},
        {"phone", "com.android.internal.telephony.ITelephony"},
        {"isub", "com.android.internal.telephony.ISub"},
        {"platform_compat", "com.android.internal.compat.IPlatformCompat"},
        {"graphicsstats", "android.view.IGraphicsStats"},
        {"dropbox", "com.android.internal.os.IDropBoxManagerService"},
        {"deviceidle", "android.os.IDeviceIdleController"},
        {"thermalservice", "android.os.IThermalService"},
        {"sensor_privacy", "android.hardware.ISensorPrivacyManager"},
        {"color_display", "android.hardware.display.IColorDisplayManager"},
        {"batterystats", "com.android.internal.app.IBatteryStats"},
        {"usagestats", "android.app.usage.IUsageStatsManager"},
        {"wallpaper", "android.app.IWallpaperManager"},
        {"search", "android.app.ISearchManager"},
        {"locale", "android.app.ILocaleManager"},
        {"grammatical_inflection", "android.app.IGrammaticalInflectionManager"},
        {"font", "com.android.internal.graphics.fonts.IFontManager"},
        {"credential", "android.credentials.ICredentialManager"},
        {"netstats", "android.net.INetworkStatsService"},
        {"voiceinteraction", "com.android.internal.app.IVoiceInteractionManagerService"},
        {"permission_checker", "android.permission.IPermissionChecker"},
        {"legacy_permission", "android.permission.ILegacyPermissionManager"},
        {"device_state", "android.hardware.devicestate.IDeviceStateManager"},
        {"keystore", "android.security.IKeyChainService"},
    };

    private static final Map<String, String> BY_NAME = new HashMap<>();
    static {
        for (String[] e : INTERFACES) BY_NAME.put(e[0], e[1]);
    }

    /** A defaults-only service for `name`, or null when andropkg knows no interface for it. */
    static IBinder create(String name) {
        String iface = BY_NAME.get(name);
        if (iface == null) return null;
        try {
            Class.forName(iface);
        } catch (ClassNotFoundException e) {
            return null;
        }
        Log.i(TAG, "service \"" + name + "\": defaults only (" + iface + ")");
        return LocalService.create(iface, new Object());
    }
}
