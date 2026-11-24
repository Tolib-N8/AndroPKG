package dev.andropkg.system.webview;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.net.Uri;
import android.text.Html;
import android.text.Layout;
import android.text.Spanned;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.util.Base64;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * andropkg's WebView provider until the WKWebView bridge: an app's WebView loads local and
 * inline content (data:, file:, android_asset, content:, loadData*) and shows it as styled
 * text (android.text.Html) — enough for help pages, licences and Markdown previews. Remote
 * pages show a placeholder. WebViewClient/WebChromeClient get the usual page callbacks.
 *
 * WebViewFactoryProvider, WebViewProvider and its delegates are hidden interfaces, so they
 * are implemented with java.lang.reflect.Proxy; anything not handled returns a default.
 */
public final class TextWebView implements InvocationHandler {
    private static final String TAG = "andropkg";
    static final String USER_AGENT = "Mozilla/5.0 (Linux; Android 14; andropkg) AppleWebKit/537.36 "
            + "(KHTML, like Gecko) Version/4.0 Chrome/120.0.0.0 Mobile Safari/537.36";

    /** The WebView package apps see (androidx.webkit's getCurrentWebViewPackage, PackageManager). */
    public static final String PACKAGE = "com.google.android.webview";
    private static android.content.pm.PackageInfo packageInfo;

    /**
     * Android System WebView, as recent as the engine behind it (WKWebView): apps refuse to show
     * web content, or send the user to "update WebView", when there is no WebView package.
     */
    public static synchronized android.content.pm.PackageInfo packageInfo() {
        if (packageInfo == null) {
            android.content.pm.PackageInfo pi = new android.content.pm.PackageInfo();
            pi.packageName = PACKAGE;
            pi.versionName = "142.0.7444.171";
            pi.setLongVersionCode(744417133L);
            android.content.pm.ApplicationInfo ai = new android.content.pm.ApplicationInfo();
            ai.packageName = PACKAGE;
            ai.enabled = true;
            ai.flags = android.content.pm.ApplicationInfo.FLAG_SYSTEM | android.content.pm.ApplicationInfo.FLAG_INSTALLED;
            ai.targetSdkVersion = 35;
            ai.minSdkVersion = 29;
            pi.applicationInfo = ai;
            packageInfo = pi;
        }
        return packageInfo;
    }

    /** Makes WebViewFactory return andropkg's provider (no WebView package is needed). */
    public static void install() {
        try {
            ClassLoader loader = WebView.class.getClassLoader();
            Class<?> factoryIface = Class.forName("android.webkit.WebViewFactoryProvider", false, loader);
            Class<?> staticsIface = Class.forName("android.webkit.WebViewFactoryProvider$Statics", false, loader);
            Object statics = Proxy.newProxyInstance(loader, new Class<?>[] {staticsIface}, (p, m, a) -> {
                switch (m.getName()) {
                    case "getDefaultUserAgent": return USER_AGENT;
                    case "getSafeBrowsingPrivacyPolicyUrl": return Uri.parse("about:blank");
                    default: return defaultFor(m);
                }
            });
            android.webkit.CookieManager cookies =
                    WkWebView.available() ? new WkWebView.Cookies() : new StubCookieManager();
            Object factory = Proxy.newProxyInstance(loader, new Class<?>[] {factoryIface}, (p, m, a) -> {
                switch (m.getName()) {
                    case "createWebView":  // WKWebView on iOS, text rendering elsewhere
                        return WkWebView.available() ? new WkWebView((WebView) a[0], a[1]).provider
                                                     : new TextWebView((WebView) a[0], a[1]).provider;
                    case "getStatics": return statics;
                    case "getCookieManager": return cookies;
                    case "getWebViewClassLoader": return TextWebView.class.getClassLoader();
                    default: return defaultFor(m);
                }
            });
            Field f = Class.forName("android.webkit.WebViewFactory", true, loader).getDeclaredField("sProviderInstance");
            f.setAccessible(true);
            f.set(null, factory);
            // WebView.getCurrentWebViewPackage() returns what WebViewFactory loaded.
            Field pf = Class.forName("android.webkit.WebViewFactory", true, loader).getDeclaredField("sPackageInfo");
            pf.setAccessible(true);
            pf.set(null, packageInfo());
        } catch (ReflectiveOperationException e) {
            Log.w(TAG, "WebView provider not installed", e);
        }
    }

    static Object defaultFor(Method m) {
        Class<?> t = m.getReturnType();
        if (!t.isPrimitive() || t == void.class) return null;
        if (t == boolean.class) return false;
        if (t == int.class) return 0;
        if (t == long.class) return 0L;
        if (t == float.class) return 0f;
        if (t == double.class) return 0.0;
        if (t == short.class) return (short) 0;
        if (t == byte.class) return (byte) 0;
        return (char) 0;
    }

    private final WebView view;
    private final Object access;  // WebView.PrivateAccess
    final Object provider;
    private final Object viewDelegate;
    private final Object scrollDelegate;
    private final StubWebSettings settings = new StubWebSettings();
    private WebViewClient client;
    private WebChromeClient chrome;
    private String url;
    private String title;
    private CharSequence content = "";
    private StaticLayout layout;
    private int layoutWidth = -1;
    private int background = Color.WHITE;
    private int scrollY;
    private float lastTouchY;

    private TextWebView(WebView view, Object access) {
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
            case "init":  // what WebViewChromium.init sets up on the view
                view.setWillNotDraw(false);
                view.setFocusable(true);
                view.setFocusableInTouchMode(true);
                return null;
            case "getViewDelegate": return viewDelegate;
            case "getScrollDelegate": return scrollDelegate;
            case "getSettings": return settings;
            case "setWebViewClient": client = (WebViewClient) a[0]; return null;
            case "getWebViewClient": return client;
            case "setWebChromeClient": chrome = (WebChromeClient) a[0]; return null;
            case "getWebChromeClient": return chrome;
            case "loadUrl": load((String) a[0]); return null;
            case "postUrl": load((String) a[0]); return null;
            case "loadData": show(decode((String) a[0], (String) a[2]), (String) a[1], "about:blank"); return null;
            case "loadDataWithBaseURL":
                show((String) a[1], (String) a[2], a[4] != null ? (String) a[4] : a[0] != null ? (String) a[0] : "about:blank");
                return null;
            case "reload": if (url != null) load(url); return null;
            case "getUrl": case "getOriginalUrl": return url;
            case "getTitle": return title;
            case "getProgress": return 100;
            case "getContentHeight": return (int) (contentHeight() / density());
            case "getContentWidth": return (int) (view.getWidth() / density());
            case "getScale": return density();
            case "evaluateJavascript":
                if (a[1] != null) view.post(() -> ((ValueCallback<String>) a[1]).onReceiveValue("null"));
                return null;
            case "pageUp": scrollBy((Boolean) a[0] ? 1 << 20 : view.getHeight(), false); return true;
            case "pageDown": scrollBy((Boolean) a[0] ? 1 << 20 : view.getHeight(), true); return true;
            // ViewDelegate
            case "onMeasure": measure((int) a[0], (int) a[1]); return null;
            case "onDraw": draw((Canvas) a[0]); return null;
            case "onSizeChanged": layoutWidth = -1; return null;
            case "onTouchEvent": return touch((MotionEvent) a[0]);
            case "setBackgroundColor": background = (int) a[0]; view.invalidate(); return null;
            // ScrollDelegate
            case "computeVerticalScrollRange": return contentHeight();
            case "computeVerticalScrollOffset": return scrollY;
            case "computeVerticalScrollExtent": return view.getHeight();
            case "computeHorizontalScrollRange": case "computeHorizontalScrollExtent": return view.getWidth();
            case "toString": return "andropkg TextWebView";
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
            return defaultFor(m);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(e);
        }
    }

    // ---- content ------------------------------------------------------------------------

    private void load(String target) {
        Log.i(TAG, "load " + (target != null && target.length() > 200 ? target.substring(0, 200) + "..." : target));
        String html;
        String mime = "text/html";
        try {
            Uri uri = Uri.parse(target);
            String scheme = uri.getScheme() == null ? "" : uri.getScheme();
            if (target.equals("about:blank")) {
                html = "";
            } else if (scheme.equals("data")) {
                int comma = target.indexOf(',');
                String meta = target.substring(5, Math.max(5, comma));
                String payload = comma >= 0 ? target.substring(comma + 1) : "";
                mime = meta.split(";")[0].isEmpty() ? "text/plain" : meta.split(";")[0];
                html = meta.endsWith(";base64") ? decode(payload, "base64") : Uri.decode(payload);
            } else if (scheme.equals("file") && uri.getPath() != null && uri.getPath().startsWith("/android_asset/")) {
                html = read(view.getContext().getAssets().open(uri.getPath().substring("/android_asset/".length())));
            } else if (scheme.equals("file")) {
                html = read(new FileInputStream(uri.getPath()));
            } else if (scheme.equals("content")) {
                html = read(view.getContext().getContentResolver().openInputStream(uri));
            } else {
                html = "<h3>Web page</h3><p>" + Html.escapeHtml(target)
                        + "</p><p><i>andropkg: remote web pages are not available yet.</i></p>";
            }
            if (!mime.contains("html") && (target.endsWith(".txt") || target.endsWith(".md"))) mime = "text/plain";
        } catch (IOException | RuntimeException e) {
            html = "<p>Cannot load " + Html.escapeHtml(target) + "</p>";
        }
        show(html, mime, target);
    }

    private void show(String data, String mime, String pageUrl) {
        url = pageUrl;
        if (data == null) data = "";
        if (mime == null || mime.contains("html")) {
            Matcher t = Pattern.compile("(?is)<title[^>]*>(.*?)</title>").matcher(data);
            title = t.find() ? Html.fromHtml(t.group(1), Html.FROM_HTML_MODE_COMPACT).toString() : pageUrl;
            String body = data.replaceAll("(?is)<(script|style|head|title)[^>]*>.*?</\\1>", "");
            Spanned s = Html.fromHtml(body, Html.FROM_HTML_MODE_COMPACT);
            int end = s.length();
            while (end > 0 && Character.isWhitespace(s.charAt(end - 1))) end--;
            content = s.subSequence(0, end);
        } else {
            title = pageUrl;
            content = data;
        }
        layoutWidth = -1;
        scrollY = 0;
        view.requestLayout();
        view.invalidate();
        final String u = pageUrl;
        view.post(() -> {
            if (client != null) {
                client.onPageStarted(view, u, null);
                client.onPageFinished(view, u);
            }
            if (chrome != null) {
                chrome.onProgressChanged(view, 100);
                if (title != null) chrome.onReceivedTitle(view, title);
            }
        });
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

    // ---- layout, drawing, scrolling -------------------------------------------------------

    private float density() {
        return view.getResources().getDisplayMetrics().density;
    }

    private int padding() {
        return (int) (8 * density());
    }

    private StaticLayout layout(int width) {
        int w = Math.max(1, width - 2 * padding());
        if (layout == null || layoutWidth != w) {
            TextPaint paint = new TextPaint(TextPaint.ANTI_ALIAS_FLAG);
            Context c = view.getContext();
            paint.setTextSize(settings.getDefaultFontSize() * settings.getTextZoom() / 100f
                    * c.getResources().getDisplayMetrics().scaledDensity);
            paint.setColor(0xff202124);
            paint.linkColor = 0xff1a0dab;
            layout = StaticLayout.Builder.obtain(content, 0, content.length(), paint, w)
                    .setAlignment(Layout.Alignment.ALIGN_NORMAL).setLineSpacing(0, 1.15f).build();
            layoutWidth = w;
        }
        return layout;
    }

    private int contentHeight() {
        int w = view.getWidth() > 0 ? view.getWidth() : view.getResources().getDisplayMetrics().widthPixels;
        return layout(w).getHeight() + 2 * padding();
    }

    private void measure(int widthSpec, int heightSpec) {
        int w = View.MeasureSpec.getSize(widthSpec);
        if (View.MeasureSpec.getMode(widthSpec) == View.MeasureSpec.UNSPECIFIED) {
            w = view.getResources().getDisplayMetrics().widthPixels;
        }
        int h = View.MeasureSpec.getSize(heightSpec);
        int wanted = layout(w).getHeight() + 2 * padding();
        switch (View.MeasureSpec.getMode(heightSpec)) {
            case View.MeasureSpec.UNSPECIFIED: h = wanted; break;
            case View.MeasureSpec.AT_MOST: h = Math.min(h, wanted); break;
            default: break;
        }
        try {
            Method set = access.getClass().getMethod("setMeasuredDimension", int.class, int.class);
            set.invoke(access, w, h);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(e);
        }
    }

    private void draw(Canvas canvas) {
        canvas.drawColor(background);
        StaticLayout l = layout(view.getWidth());
        int save = canvas.save();
        canvas.translate(padding(), padding() - scrollY);
        l.draw(canvas);
        canvas.restoreToCount(save);
    }

    private void scrollBy(int dy, boolean down) {
        int max = Math.max(0, contentHeight() - view.getHeight());
        scrollY = Math.max(0, Math.min(max, scrollY + (down ? dy : -dy)));
        view.invalidate();
    }

    private boolean touch(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                lastTouchY = e.getY();
                return true;
            case MotionEvent.ACTION_MOVE:
                int dy = (int) (lastTouchY - e.getY());
                lastTouchY = e.getY();
                scrollBy(Math.abs(dy), dy > 0);
                return true;
            default:
                return true;
        }
    }
}
