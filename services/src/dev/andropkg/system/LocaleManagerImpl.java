package dev.andropkg.system;

import android.os.LocaleList;

/**
 * android.app.ILocaleManager (LocaleManager, AppCompatDelegate.getApplicationLocales): the
 * per-app language. Nothing is set until the app sets it; then it is kept for this process.
 */
final class LocaleManagerImpl {
    private LocaleList appLocales = LocaleList.getEmptyLocaleList();

    public synchronized void setApplicationLocales(String pkg, int userId, LocaleList locales, boolean fromDelegate) {
        appLocales = locales != null ? locales : LocaleList.getEmptyLocaleList();
    }

    public synchronized LocaleList getApplicationLocales(String pkg, int userId) {
        return appLocales;
    }

    public LocaleList getSystemLocales() {
        return LocaleList.getDefault();
    }
}
