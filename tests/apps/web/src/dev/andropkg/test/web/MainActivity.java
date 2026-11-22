package dev.andropkg.test.web;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.webkit.CookieManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/**
 * WebView on WKWebView (iOS): a page calling a Java interface synchronously, evaluateJavascript,
 * a remote page, cookies, and the WebView shown over the activity. Logs "[web] ...".
 */
public class MainActivity extends Activity {
    static final String TAG = "WebTest";
    private WebView web;
    private int stage;

    public class Bridge {
        @JavascriptInterface public int add(int a, int b) { return a + b; }
        @JavascriptInterface public String echo(String s) { return "java:" + s; }
        @JavascriptInterface public void report(String s) { Log.i(TAG, "[web] report " + s); }
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        web = new WebView(this);
        web.getSettings().setJavaScriptEnabled(true);
        web.addJavascriptInterface(new Bridge(), "Android");
        web.setWebChromeClient(new WebChromeClient() {
            @Override public void onReceivedTitle(WebView v, String title) { Log.i(TAG, "[web] title " + title); }
        });
        web.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView v, String url) {
                Log.i(TAG, "[web] finished " + url);
                if (stage == 0) {
                    stage = 1;
                    v.evaluateJavascript("document.title + '|' + (1 + 2)", r -> {
                        Log.i(TAG, "[web] eval " + r);
                        v.loadUrl("https://example.com/");
                    });
                } else if (stage == 1 && url.startsWith("https://example.com")) {
                    stage = 2;
                    v.evaluateJavascript("document.title", r -> {
                        Log.i(TAG, "[web] remote " + r);
                        CookieManager cm = CookieManager.getInstance();
                        cm.setCookie("https://example.com/", "andropkg=1; Path=/");
                        new Thread(() -> {
                            Log.i(TAG, "[web] cookie " + cm.getCookie("https://example.com/"));
                            Log.i(TAG, "[web] done");
                        }).start();
                    });
                }
            }

            @Override
            public void onReceivedError(WebView v, int code, String description, String url) {
                Log.i(TAG, "[web] error " + code + " " + description + " " + url);
            }
        });
        setContentView(web);
        web.loadDataWithBaseURL("https://andropkg.test/", "<html><head><title>start</title></head><body>"
                + "<h1 id=h>andropkg WebView</h1><script>"
                + "var s = Android.add(2, 3), e = Android.echo('hi');"
                + "document.title = 'sum=' + s + ' echo=' + e;"
                + "Android.report('typeof=' + typeof s);"
                + "</script></body></html>", "text/html", "utf-8", null);
    }

    @Override
    protected void onDestroy() {
        web.destroy();
        super.onDestroy();
    }
}
