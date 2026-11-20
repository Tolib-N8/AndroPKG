package dev.andropkg.system.webview;

import android.net.Uri;
import android.webkit.ValueCallback;
import android.webkit.WebView;
import java.util.LinkedHashMap;
import java.util.Map;

/** android.webkit.CookieManager for andropkg's WebView: an in-memory jar, per host. */
final class StubCookieManager extends android.webkit.CookieManager {
    private final Map<String, Map<String, String>> jar = new LinkedHashMap<>();
    private boolean accept = true;

    private static String host(String url) {
        String h = Uri.parse(url).getHost();
        return h != null ? h : url;
    }

    @Override public synchronized boolean acceptCookie() { return accept; }
    @Override public synchronized void setAcceptCookie(boolean a) { accept = a; }
    @Override public boolean acceptThirdPartyCookies(WebView v) { return false; }
    @Override public void setAcceptThirdPartyCookies(WebView v, boolean a) {}
    @Override public void flush() {}

    @Override
    public synchronized void setCookie(String url, String value) {
        String pair = value.split(";", 2)[0];
        int eq = pair.indexOf('=');
        if (eq <= 0) return;
        jar.computeIfAbsent(host(url), k -> new LinkedHashMap<>()).put(pair.substring(0, eq).trim(),
                pair.substring(eq + 1).trim());
    }

    @Override
    public void setCookie(String url, String value, ValueCallback<Boolean> callback) {
        setCookie(url, value);
        if (callback != null) callback.onReceiveValue(true);
    }

    @Override
    public synchronized String getCookie(String url) {
        Map<String, String> c = jar.get(host(url));
        if (c == null || c.isEmpty()) return null;
        StringBuilder b = new StringBuilder();
        for (Map.Entry<String, String> e : c.entrySet()) {
            if (b.length() > 0) b.append("; ");
            b.append(e.getKey()).append('=').append(e.getValue());
        }
        return b.toString();
    }

    @Override public synchronized boolean hasCookies() { return !jar.isEmpty(); }
    @Override public synchronized void removeAllCookie() { jar.clear(); }

    @Override
    public void removeAllCookies(ValueCallback<Boolean> callback) {
        boolean had = hasCookies();
        removeAllCookie();
        if (callback != null) callback.onReceiveValue(had);
    }

    @Override public void removeExpiredCookie() {}
    @Override public void removeSessionCookie() {}

    @Override
    public void removeSessionCookies(ValueCallback<Boolean> callback) {
        if (callback != null) callback.onReceiveValue(false);
    }
}
