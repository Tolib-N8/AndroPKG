package dev.andropkg.system.webview;

import android.content.Context;
import android.graphics.Rect;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Base64;
import android.util.Log;
import android.view.View;
import android.view.ViewTreeObserver;
import android.webkit.JavascriptInterface;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * android.webkit.WebView on WKWebView (iOS; natives in runtime/webview/wkwebview.mm). The web
 * content is a real WKWebView over the Android frame in this view's rectangle: WebKit draws it
 * and takes its touches and keyboard. This WebViewProvider keeps that rectangle in step with the
 * view (layout, scrolling, visibility), forwards the API (loads, history, evaluateJavascript,
 * addJavascriptInterface) and turns WebKit's events into WebViewClient / WebChromeClient calls.
 *
 * Javascript interfaces are called synchronously from the page, as on Android: a shim object per
 * interface calls window.prompt("andropkg-bridge:{json}"), which WebKit hands to us on its
 * thread and returns our answer. Off iOS (no host window) TextWebView is used instead.
 */
public final class WkWebView implements InvocationHandler {
    private static final String TAG = "andropkg-webview";
    static volatile boolean sAvailable;  // set when the natives are registered (iOS)

    static boolean available() {
        return sAvailable;
    }

    private native long nativeCreate(String userAgent, boolean javascript);
    private native void nativeDestroy(long h);
    private native void nativeSetFrame(long h, int x, int y, int w, int hgt, int cx, int cy, int cw, int ch,
                                       boolean visible, int displayWidth);
    private native void nativeLoadUrl(long h, String url, String[] headers);
    private native void nativeLoadHtml(long h, String html, String baseUrl);
    private native void nativeEvaluate(long h, String script, int callback);
    private native void nativeAddUserScript(long h, String script);
    private native void nativeCommand(long h, int command);
    private native int nativeHistory(long h);
    private native void nativeSetUserAgent(long h, String ua);
    private native void nativeSetBackground(long h, int argb);
    static native void nativeSetCookie(String url, String value);
    static native String nativeGetCookie(String url);
    static native void nativeRemoveAllCookies();

    private final WebView view;
    private final Object access;  // WebView.PrivateAccess
    final Object provider;
    private final Object viewDelegate;
    private final Object scrollDelegate;
    private final StubWebSettings settings = new StubWebSettings();
    private final Handler ui = new Handler(Looper.getMainLooper());
    private long handle;
    private WebViewClient client = new WebViewClient();
    private WebChromeClient chrome;
    private volatile String url;
    private volatile String title;
    private volatile int progress;
    private String userAgent;
    private final Map<String, Object> interfaces = new HashMap<>();
    private final Map<Integer, ValueCallback<String>> evalCallbacks = new HashMap<>();
    private int nextEval = 1;
    private final int[] lastFrame = new int[10];
    private final ViewTreeObserver.OnPreDrawListener preDraw = () -> {
        updateFrame();
        return true;
    };
    private final ViewTreeObserver.OnScrollChangedListener scrolled = this::updateFrame;

    WkWebView(WebView view, Object access) {
        this.view = view;
        this.access = access;
        ClassLoader loader = WebView.class.getClassLoader();
        try {
            provider = proxy(loader, "android.webkit.WebViewProvider");
            viewDelegate = proxy(loader, "android.webkit.WebViewProvider$ViewDelegate");
            scrollDelegate = proxy(loader, "android.webkit.WebViewProvider$ScrollDelegate");
        } catch (ClassNotFoundException e) {
            throw new IllegalStateException(e);
        }
    }

    private Object proxy(ClassLoader loader, String iface) throws ClassNotFoundException {
        return Proxy.newProxyInstance(loader, new Class<?>[] {Class.forName(iface, false, loader)}, this);
    }

    @Override
    @SuppressWarnings("unchecked")
    public Object invoke(Object proxy, Method m, Object[] a) {
        switch (m.getName()) {
            // WebViewProvider
            case "init":
                view.setWillNotDraw(false);
                view.setFocusable(true);
                view.setFocusableInTouchMode(true);
                userAgent = settings.getUserAgentString();
                handle = nativeCreate(userAgent, true);
                return null;
            case "getViewDelegate": return viewDelegate;
            case "getScrollDelegate": return scrollDelegate;
            case "getSettings": return settings;
            case "setWebViewClient": client = a[0] != null ? (WebViewClient) a[0] : new WebViewClient(); return null;
            case "getWebViewClient": return client;
            case "setWebChromeClient": chrome = (WebChromeClient) a[0]; return null;
            case "getWebChromeClient": return chrome;
            case "loadUrl":
                load((String) a[0], a.length > 1 ? (Map<String, String>) a[1] : null);
                return null;
            case "postUrl": load((String) a[0], null); return null;
            case "loadData":
                html(decode((String) a[0], (String) a[2]), null);
                return null;
            case "loadDataWithBaseURL": html((String) a[1], (String) a[0]); return null;
            case "evaluateJavaScript": case "evaluateJavascript":
                evaluate((String) a[0], (ValueCallback<String>) a[1]);
                return null;
            case "addJavascriptInterface": addInterface(a[0], (String) a[1]); return null;
            case "removeJavascriptInterface": synchronized (interfaces) { interfaces.remove((String) a[0]); } return null;
            case "reload": command(2); return null;
            case "stopLoading": command(3); return null;
            case "goBack": command(0); return null;
            case "goForward": command(1); return null;
            case "canGoBack": return handle != 0 && (nativeHistory(handle) & 1) != 0;
            case "canGoForward": return handle != 0 && (nativeHistory(handle) & 2) != 0;
            case "goBackOrForward": command((int) a[0] < 0 ? 0 : 1); return null;
            case "canGoBackOrForward": return handle != 0 && (nativeHistory(handle) & ((int) a[0] < 0 ? 1 : 2)) != 0;
            case "getUrl": case "getOriginalUrl": return url;
            case "getTitle": return title;
            case "getProgress": return progress;
            case "getContentHeight": return view.getHeight();
            case "getContentWidth": return view.getWidth();
            case "getScale": return view.getResources().getDisplayMetrics().density;
            case "destroy": destroy(); return null;
            case "requestFocus": command(4); return superOrDefault(m, a);
            // ViewDelegate
            case "onAttachedToWindow":
                view.getViewTreeObserver().addOnPreDrawListener(preDraw);
                view.getViewTreeObserver().addOnScrollChangedListener(scrolled);
                updateFrame();
                return superOrDefault(m, a);
            case "onDetachedFromWindow":
                view.getViewTreeObserver().removeOnPreDrawListener(preDraw);
                view.getViewTreeObserver().removeOnScrollChangedListener(scrolled);
                hide();
                return superOrDefault(m, a);
            case "onVisibilityChanged": case "onWindowVisibilityChanged": case "onSizeChanged":
            case "onDraw": case "onScrollChanged":
                updateFrame();
                return superOrDefault(m, a);
            case "setBackgroundColor":
                if (handle != 0) nativeSetBackground(handle, (int) a[0]);
                return null;
            case "toString": return "andropkg WkWebView";
            case "hashCode": return System.identityHashCode(proxy);
            case "equals": return proxy == a[0];
            default: return superOrDefault(m, a);
        }
    }

    /** What Chromium does for the View methods it doesn't change: WebView.PrivateAccess.super_X. */
    private Object superOrDefault(Method m, Object[] a) {
        try {
            Method sup = access.getClass().getMethod("super_" + m.getName(), m.getParameterTypes());
            return sup.invoke(access, a);
        } catch (NoSuchMethodException e) {
            return TextWebView.defaultFor(m);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(e);
        }
    }

    // ---- placement ----------------------------------------------------------------------

    private void updateFrame() {
        if (handle == 0) return;
        boolean visible = view.isAttachedToWindow() && view.isShown()
                && view.getWindowVisibility() == View.VISIBLE && view.getWidth() > 0 && view.getHeight() > 0;
        int[] screen = new int[2], window = new int[2];
        view.getLocationOnScreen(screen);
        view.getLocationInWindow(window);
        Rect clip = new Rect();
        if (!view.getGlobalVisibleRect(clip)) visible = false;
        clip.offset(screen[0] - window[0], screen[1] - window[1]);  // (window -> screen)
        int[] f = {screen[0], screen[1], view.getWidth(), view.getHeight(), clip.left, clip.top, clip.width(),
                   clip.height(), visible ? 1 : 0, view.getResources().getDisplayMetrics().widthPixels};
        if (java.util.Arrays.equals(f, lastFrame)) return;
        System.arraycopy(f, 0, lastFrame, 0, f.length);
        nativeSetFrame(handle, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], visible, f[9]);
    }

    private void hide() {
        if (handle == 0) return;
        lastFrame[8] = 0;
        nativeSetFrame(handle, 0, 0, 0, 0, 0, 0, 0, 0, false, 1);
    }

    private void destroy() {
        if (handle == 0) return;
        long h = handle;
        handle = 0;
        nativeDestroy(h);
    }

    // ---- loading ------------------------------------------------------------------------

    private void syncUserAgent() {
        String ua = settings.getUserAgentString();
        if (handle != 0 && ua != null && !ua.equals(userAgent)) {
            userAgent = ua;
            nativeSetUserAgent(handle, ua);
        }
    }

    private void load(String target, Map<String, String> headers) {
        if (handle == 0 || target == null) return;
        syncUserAgent();
        Uri uri = Uri.parse(target);
        String scheme = uri.getScheme() == null ? "" : uri.getScheme().toLowerCase();
        try {
            if (scheme.equals("javascript")) {
                evaluate(Uri.decode(target.substring("javascript:".length())), null);
            } else if (scheme.equals("file") && uri.getPath() != null && uri.getPath().startsWith("/android_asset/")) {
                html(read(view.getContext().getAssets().open(uri.getPath().substring("/android_asset/".length()))), target);
            } else if (scheme.equals("file") && uri.getPath() != null && uri.getPath().startsWith("/android_res/")) {
                html("", target);
            } else if (scheme.equals("file")) {
                html(read(new FileInputStream(uri.getPath())), target);
            } else if (scheme.equals("content")) {
                html(read(view.getContext().getContentResolver().openInputStream(uri)), target);
            } else {
                List<String> h = new ArrayList<>();
                if (headers != null) {
                    for (Map.Entry<String, String> e : headers.entrySet()) {
                        h.add(e.getKey());
                        h.add(e.getValue());
                    }
                }
                url = target;
                nativeLoadUrl(handle, target, h.toArray(new String[0]));
            }
        } catch (IOException e) {
            Log.w(TAG, "cannot load " + target, e);
            html("<p>Cannot load " + android.text.Html.escapeHtml(target) + "</p>", "about:blank");
        }
    }

    private void html(String data, String baseUrl) {
        if (handle == 0) return;
        syncUserAgent();
        url = baseUrl != null ? baseUrl : "about:blank";
        nativeLoadHtml(handle, data != null ? data : "", baseUrl);
    }

    private void command(int c) {
        if (handle != 0) nativeCommand(handle, c);
    }

    private void evaluate(String script, ValueCallback<String> callback) {
        if (handle == 0) return;
        int id = 0;
        if (callback != null) {
            synchronized (evalCallbacks) {
                id = nextEval++;
                evalCallbacks.put(id, callback);
            }
        }
        nativeEvaluate(handle, script, id);
    }

    private static String decode(String data, String encoding) {
        if (data == null) return "";
        if ("base64".equalsIgnoreCase(encoding)) {
            return new String(Base64.decode(data, Base64.DEFAULT), StandardCharsets.UTF_8);
        }
        return data;
    }

    private static String read(InputStream in) throws IOException {
        try (InputStream s = in) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            for (int n; (n = s.read(buf)) > 0; ) out.write(buf, 0, n);
            return out.toString("UTF-8");
        }
    }

    // ---- Javascript interfaces --------------------------------------------------------------

    private void addInterface(Object obj, String name) {
        if (obj == null || name == null || handle == 0) return;
        synchronized (interfaces) {
            interfaces.put(name, obj);
        }
        JSONArray methods = new JSONArray();
        for (Method m : obj.getClass().getMethods()) {
            if (m.isAnnotationPresent(JavascriptInterface.class)) methods.put(m.getName());
        }
        String n = JSONObject.quote(name);
        nativeAddUserScript(handle, "(function(){var n=" + n + ",ms=" + methods + ";"
                + "var o=(window[n]&&window[n].__andropkg)?window[n]:{__andropkg:true};"
                + "ms.forEach(function(m){o[m]=function(){var r=prompt('andropkg-bridge:'+JSON.stringify("
                + "{o:n,m:m,a:Array.prototype.slice.call(arguments)}));if(r===null||r===undefined)return undefined;"
                + "try{return JSON.parse(r).v;}catch(e){return undefined;}};});window[n]=o;})();");
    }

    /** From the page, on WebKit's thread: {o: interface, m: method, a: [args]} -> {"v": result}. */
    @SuppressWarnings("unused")
    String onJsCall(String json) {
        try {
            JSONObject call = new JSONObject(json);
            Object obj;
            synchronized (interfaces) {
                obj = interfaces.get(call.getString("o"));
            }
            if (obj == null) return null;
            String name = call.getString("m");
            JSONArray args = call.optJSONArray("a");
            int n = args == null ? 0 : args.length();
            for (Method m : obj.getClass().getMethods()) {
                if (!m.getName().equals(name) || m.getParameterCount() != n
                        || !m.isAnnotationPresent(JavascriptInterface.class)) continue;
                Class<?>[] types = m.getParameterTypes();
                Object[] values = new Object[n];
                for (int i = 0; i < n; i++) values[i] = convert(args.isNull(i) ? null : args.get(i), types[i]);
                Object r = m.invoke(obj, values);
                JSONObject out = new JSONObject();
                if (m.getReturnType() != void.class) out.put("v", r == null ? JSONObject.NULL : JSONObject.wrap(r));
                return out.toString();
            }
            Log.w(TAG, "no @JavascriptInterface " + call.getString("o") + "." + name + "/" + n);
        } catch (Throwable t) {
            Log.w(TAG, "javascript interface call " + json, t);
        }
        return null;
    }

    private static Object convert(Object v, Class<?> t) {
        if (t == String.class) return v == null ? null : v.toString();
        if (v == null && !t.isPrimitive()) return null;
        if (t == boolean.class || t == Boolean.class) {
            return v instanceof Boolean ? v : v != null && Boolean.parseBoolean(v.toString());
        }
        double d = v == null ? 0 : v instanceof Number ? ((Number) v).doubleValue() : parse(v.toString());
        if (t == int.class || t == Integer.class) return (int) d;
        if (t == long.class || t == Long.class) return (long) d;
        if (t == double.class || t == Double.class) return d;
        if (t == float.class || t == Float.class) return (float) d;
        if (t == short.class || t == Short.class) return (short) d;
        if (t == byte.class || t == Byte.class) return (byte) d;
        if (t == char.class || t == Character.class) return (char) d;
        return t.isInstance(v) ? v : null;
    }

    private static double parse(String s) {
        try {
            return Double.parseDouble(s);
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    // ---- WebKit events (WebKit's thread) ----------------------------------------------------

    @SuppressWarnings("unused")
    void onPageStarted(String u) {
        url = u;
        ui.post(() -> client.onPageStarted(view, u, null));
    }

    @SuppressWarnings("unused")
    void onPageFinished(String u) {
        url = u;
        ui.post(() -> client.onPageFinished(view, u));
    }

    @SuppressWarnings("unused")
    void onProgress(int p) {
        progress = p;
        ui.post(() -> {
            if (chrome != null) chrome.onProgressChanged(view, p);
        });
    }

    @SuppressWarnings("unused")
    void onTitle(String t) {
        title = t;
        ui.post(() -> {
            if (chrome != null && t != null) chrome.onReceivedTitle(view, t);
        });
    }

    @SuppressWarnings("unused")
    void onUrl(String u) {
        if (u != null) url = u;
    }

    @SuppressWarnings({"unused", "deprecation"})
    void onError(int code, String description, String failingUrl) {
        ui.post(() -> client.onReceivedError(view, code, description, failingUrl));
    }

    /** Navigations the page starts (links, redirects, scripts): the app may take them over. */
    @SuppressWarnings("unused")
    boolean shouldOverride(String u, boolean mainFrame, boolean gesture) {
        Uri uri = Uri.parse(u);
        WebResourceRequest request = new WebResourceRequest() {
            @Override public Uri getUrl() { return uri; }
            @Override public boolean isForMainFrame() { return mainFrame; }
            @Override public boolean isRedirect() { return false; }
            @Override public boolean hasGesture() { return gesture; }
            @Override public String getMethod() { return "GET"; }
            @Override public Map<String, String> getRequestHeaders() { return new HashMap<>(); }
        };
        try {
            return client.shouldOverrideUrlLoading(view, request);
        } catch (Throwable t) {
            Log.w(TAG, "shouldOverrideUrlLoading " + u, t);
            return false;
        }
    }

    @SuppressWarnings("unused")
    void onEvalResult(int id, String json) {
        ValueCallback<String> cb;
        synchronized (evalCallbacks) {
            cb = evalCallbacks.remove(id);
        }
        if (cb != null) ui.post(() -> cb.onReceiveValue(json));
    }

    /** The CookieManager of WKWebView's cookie store (WebKit's default data store). */
    static final class Cookies extends android.webkit.CookieManager {
        private volatile boolean accept = true;

        @Override public boolean acceptCookie() { return accept; }
        @Override public void setAcceptCookie(boolean a) { accept = a; }
        @Override public boolean acceptThirdPartyCookies(WebView v) { return true; }
        @Override public void setAcceptThirdPartyCookies(WebView v, boolean a) {}
        @Override public void flush() {}
        @Override public void setCookie(String url, String value) { nativeSetCookie(url, value); }

        @Override
        public void setCookie(String url, String value, ValueCallback<Boolean> callback) {
            nativeSetCookie(url, value);
            if (callback != null) new Handler(Looper.getMainLooper()).post(() -> callback.onReceiveValue(true));
        }

        @Override public String getCookie(String url) { return nativeGetCookie(url); }
        @Override public boolean hasCookies() { return true; }
        @Override public void removeAllCookie() { nativeRemoveAllCookies(); }

        @Override
        public void removeAllCookies(ValueCallback<Boolean> callback) {
            nativeRemoveAllCookies();
            if (callback != null) new Handler(Looper.getMainLooper()).post(() -> callback.onReceiveValue(true));
        }

        @Override public void removeExpiredCookie() {}
        @Override public void removeSessionCookie() {}

        @Override
        public void removeSessionCookies(ValueCallback<Boolean> callback) {
            if (callback != null) new Handler(Looper.getMainLooper()).post(() -> callback.onReceiveValue(true));
        }
    }
}
