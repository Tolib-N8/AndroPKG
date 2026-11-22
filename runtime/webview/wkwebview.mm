// android.webkit.WebView on WKWebView (iOS): natives of dev.andropkg.system.webview.WkWebView.
//
// The web content is a real WKWebView placed over the Android frame, in the WebView's rectangle
// (clipped like the view): WebKit draws it and gets its touches and keyboard directly, as
// Chromium's own view would on a device. The Java side (services) implements WebViewProvider and
// reports the view's place on screen; this file drives WebKit and calls back:
//   page lifecycle -> WebViewClient (started, finished, shouldOverrideUrlLoading, errors),
//   progress/title -> WebChromeClient, evaluateJavascript results,
//   addJavascriptInterface: synchronous calls from JS through window.prompt("andropkg-bridge:...")
//     (WKUIDelegate answers a prompt synchronously, as Android's Java bridge returns values).
// WebKit is used on the main thread only; Java callbacks run there too (attached to the VM).
#include <jni.h>

#include <TargetConditionals.h>

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>

namespace {

JavaVM* gVm = nullptr;
jclass gCls = nullptr;  // dev.andropkg.system.webview.WkWebView
jmethodID gOnPageStarted, gOnPageFinished, gOnProgress, gOnTitle, gOnError, gShouldOverride, gOnJsCall,
    gOnEvalResult, gOnUrl;

JNIEnv* Env() {
  JNIEnv* env = nullptr;
  if (gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    JavaVMAttachArgs args = {JNI_VERSION_1_6, "WebKit", nullptr};
    gVm->AttachCurrentThreadAsDaemon(&env, &args);
  }
  return env;
}

// For GCD worker threads (which come and go): attached for one call, then detached, since a
// thread that exits while attached aborts the VM.
struct ScopedEnv {
  JNIEnv* env = nullptr;
  bool attached = false;
  ScopedEnv() {
    if (gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
      JavaVMAttachArgs args = {JNI_VERSION_1_6, "WebKit bridge", nullptr};
      attached = gVm->AttachCurrentThread(&env, &args) == JNI_OK;
    }
  }
  ~ScopedEnv() {
    if (attached) gVm->DetachCurrentThread();
  }
};

NSString* NS(JNIEnv* env, jstring s) {
  if (s == nullptr) return nil;
  const char* c = env->GetStringUTFChars(s, nullptr);
  NSString* r = c ? [NSString stringWithUTF8String:c] : nil;
  if (c) env->ReleaseStringUTFChars(s, c);
  return r;
}

jstring J(JNIEnv* env, NSString* s) { return s ? env->NewStringUTF(s.UTF8String) : nullptr; }

void Check(JNIEnv* env) {
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
}

void OnMain(dispatch_block_t b) {
  if (NSThread.isMainThread) b();
  else dispatch_sync(dispatch_get_main_queue(), b);
}

dispatch_queue_t BridgeQueue() {  // Java calls WebKit waits on: app callbacks, JS interfaces
  static dispatch_queue_t q = dispatch_queue_create("andropkg.webview.bridge", DISPATCH_QUEUE_SERIAL);
  return q;
}

UIView* RootView() {
  for (UIScene* s in UIApplication.sharedApplication.connectedScenes) {
    if (![s isKindOfClass:UIWindowScene.class]) continue;
    for (UIWindow* w in ((UIWindowScene*)s).windows) {
      if (w.rootViewController.view) return w.rootViewController.view;
    }
  }
  return nil;
}

}  // namespace

@interface APWebView : NSObject <WKNavigationDelegate, WKUIDelegate> {
 @public
  jobject peer;  // global ref to the Java WkWebView
  UIView* clip;
  WKWebView* web;
  NSString* programmatic;  // URL of the last load the app asked for (not shouldOverride'd)
}
@end

@implementation APWebView

- (void)observeValueForKeyPath:(NSString*)path ofObject:(id)o change:(NSDictionary*)c context:(void*)ctx {
  JNIEnv* env = Env();
  if ([path isEqualToString:@"estimatedProgress"]) {
    env->CallVoidMethod(peer, gOnProgress, (jint)(web.estimatedProgress * 100));
  } else if ([path isEqualToString:@"title"]) {
    jstring t = J(env, web.title);
    env->CallVoidMethod(peer, gOnTitle, t);
    if (t) env->DeleteLocalRef(t);
  } else if ([path isEqualToString:@"URL"]) {
    jstring u = J(env, web.URL.absoluteString);
    env->CallVoidMethod(peer, gOnUrl, u);
    if (u) env->DeleteLocalRef(u);
  }
  Check(env);
}

// ---- WKNavigationDelegate -----------------------------------------------------------------

- (void)webView:(WKWebView*)w decidePolicyForNavigationAction:(WKNavigationAction*)a
    decisionHandler:(void (^)(WKNavigationActionPolicy))decide {
  NSURL* url = a.request.URL;
  NSString* s = url.absoluteString;
  bool main = a.targetFrame == nil || a.targetFrame.isMainFrame;
  NSString* scheme = url.scheme.lowercaseString ?: @"";
  bool loadable = [@[ @"http", @"https", @"about", @"data", @"file", @"blob" ] containsObject:scheme];
  if (main && programmatic && [s isEqualToString:programmatic]) {
    programmatic = nil;  // the app's own loadUrl: not offered to shouldOverrideUrlLoading
    decide(WKNavigationActionPolicyAllow);
    return;
  }
  if (main || !loadable) {
    // Asked off the main thread (the app answers on its UI thread and may read cookies, which
    // WebKit's store delivers on the main thread); WebKit waits for the decision.
    bool gesture = a.navigationType == WKNavigationTypeLinkActivated;
    jobject p = Env()->NewGlobalRef(peer);  // (kept alive past a destroy while the app answers)
    dispatch_async(BridgeQueue(), ^{
      ScopedEnv scoped;
      JNIEnv* env = scoped.env;
      jstring js = J(env, s);
      jboolean override = env->CallBooleanMethod(p, gShouldOverride, js, (jboolean)main, (jboolean)gesture);
      Check(env);
      env->DeleteGlobalRef(p);
      if (js) env->DeleteLocalRef(js);
      dispatch_async(dispatch_get_main_queue(), ^{
        decide(override || !loadable ? WKNavigationActionPolicyCancel : WKNavigationActionPolicyAllow);
      });
    });
    return;
  }
  decide(WKNavigationActionPolicyAllow);
}

- (void)webView:(WKWebView*)w didStartProvisionalNavigation:(WKNavigation*)n {
  JNIEnv* env = Env();
  jstring u = J(env, w.URL.absoluteString);
  env->CallVoidMethod(peer, gOnPageStarted, u);
  Check(env);
  if (u) env->DeleteLocalRef(u);
}

- (void)webView:(WKWebView*)w didFinishNavigation:(WKNavigation*)n {
  JNIEnv* env = Env();
  jstring u = J(env, w.URL.absoluteString);
  env->CallVoidMethod(peer, gOnPageFinished, u);
  Check(env);
  if (u) env->DeleteLocalRef(u);
}

- (void)fail:(NSError*)e {
  if (e.code == NSURLErrorCancelled || e.code == 102 /* WebKitErrorFrameLoadInterruptedByPolicyChange */) return;
  int code = -1;  // WebViewClient.ERROR_UNKNOWN
  switch (e.code) {
    case NSURLErrorCannotFindHost: case NSURLErrorDNSLookupFailed: code = -2; break;  // ERROR_HOST_LOOKUP
    case NSURLErrorCannotConnectToHost: case NSURLErrorNetworkConnectionLost:
    case NSURLErrorNotConnectedToInternet: code = -6; break;  // ERROR_CONNECT
    case NSURLErrorTimedOut: code = -8; break;  // ERROR_TIMEOUT
    case NSURLErrorBadURL: case NSURLErrorUnsupportedURL: code = -12; break;  // ERROR_BAD_URL
    case NSURLErrorSecureConnectionFailed: case NSURLErrorServerCertificateUntrusted:
    case NSURLErrorServerCertificateHasBadDate: code = -11; break;  // ERROR_FAILED_SSL_HANDSHAKE
    default: break;
  }
  NSString* failing = e.userInfo[NSURLErrorFailingURLStringErrorKey] ?: web.URL.absoluteString;
  JNIEnv* env = Env();
  jstring d = J(env, e.localizedDescription);
  jstring u = J(env, failing);
  env->CallVoidMethod(peer, gOnError, (jint)code, d, u);
  Check(env);
  if (d) env->DeleteLocalRef(d);
  if (u) env->DeleteLocalRef(u);
}

- (void)webView:(WKWebView*)w didFailProvisionalNavigation:(WKNavigation*)n withError:(NSError*)e {
  [self fail:e];
}

- (void)webView:(WKWebView*)w didFailNavigation:(WKNavigation*)n withError:(NSError*)e {
  [self fail:e];
}

// ---- WKUIDelegate -------------------------------------------------------------------------

- (void)webView:(WKWebView*)w runJavaScriptTextInputPanelWithPrompt:(NSString*)prompt
          defaultText:(NSString*)text initiatedByFrame:(WKFrameInfo*)frame
    completionHandler:(void (^)(NSString*))done {
  static NSString* const kBridge = @"andropkg-bridge:";
  if ([prompt hasPrefix:kBridge]) {
    // On a bridge thread, as Android's JavaBridge thread (the page waits for the answer).
    NSString* json = [prompt substringFromIndex:kBridge.length];
    jobject p = Env()->NewGlobalRef(peer);  // (kept alive past a destroy while the app answers)
    dispatch_async(BridgeQueue(), ^{
      ScopedEnv scoped;
      JNIEnv* env = scoped.env;
      jstring call = J(env, json);
      jstring r = static_cast<jstring>(env->CallObjectMethod(p, gOnJsCall, call));
      Check(env);
      env->DeleteGlobalRef(p);
      NSString* answer = r ? NS(env, r) : nil;
      if (call) env->DeleteLocalRef(call);
      if (r) env->DeleteLocalRef(r);
      dispatch_async(dispatch_get_main_queue(), ^{ done(answer); });
    });
    return;
  }
  done(text);
}

- (void)webView:(WKWebView*)w runJavaScriptAlertPanelWithMessage:(NSString*)m initiatedByFrame:(WKFrameInfo*)f
    completionHandler:(void (^)(void))done {
  done();
}

- (void)webView:(WKWebView*)w runJavaScriptConfirmPanelWithMessage:(NSString*)m initiatedByFrame:(WKFrameInfo*)f
    completionHandler:(void (^)(BOOL))done {
  done(YES);
}

// window.open / target=_blank: in this same view (no multiple-window support).
- (WKWebView*)webView:(WKWebView*)w createWebViewWithConfiguration:(WKWebViewConfiguration*)c
      forNavigationAction:(WKNavigationAction*)a windowFeatures:(WKWindowFeatures*)f {
  if (a.request.URL) [w loadRequest:a.request];
  return nil;
}

@end

namespace {

APWebView* Get(jlong h) { return (__bridge APWebView*)reinterpret_cast<void*>(h); }

jlong wk_create(JNIEnv* env, jobject thiz, jstring jua, jboolean js) {
  NSString* ua = NS(env, jua);
  APWebView* v = [APWebView new];
  v->peer = env->NewGlobalRef(thiz);
  OnMain(^{
    WKWebViewConfiguration* config = [WKWebViewConfiguration new];
    config.websiteDataStore = WKWebsiteDataStore.defaultDataStore;
    config.allowsInlineMediaPlayback = YES;
    config.defaultWebpagePreferences.allowsContentJavaScript = js;
    config.preferences.javaScriptCanOpenWindowsAutomatically = YES;
    v->web = [[WKWebView alloc] initWithFrame:CGRectZero configuration:config];
    if (ua.length) v->web.customUserAgent = ua;
    v->web.navigationDelegate = v;
    v->web.UIDelegate = v;
    v->web.opaque = NO;
    v->web.backgroundColor = UIColor.whiteColor;
    if (@available(iOS 16.4, *)) v->web.inspectable = YES;
    for (NSString* k in @[ @"estimatedProgress", @"title", @"URL" ]) {
      [v->web addObserver:v forKeyPath:k options:NSKeyValueObservingOptionNew context:nullptr];
    }
    v->clip = [[UIView alloc] initWithFrame:CGRectZero];
    v->clip.clipsToBounds = YES;
    v->clip.hidden = YES;
    [v->clip addSubview:v->web];
    [RootView() addSubview:v->clip];
  });
  return reinterpret_cast<jlong>((__bridge_retained void*)v);
}

void wk_destroy(JNIEnv* env, jobject, jlong h) {
  APWebView* v = (__bridge_transfer APWebView*)reinterpret_cast<void*>(h);
  OnMain(^{
    for (NSString* k in @[ @"estimatedProgress", @"title", @"URL" ]) [v->web removeObserver:v forKeyPath:k];
    [v->web stopLoading];
    v->web.navigationDelegate = nil;
    v->web.UIDelegate = nil;
    [v->clip removeFromSuperview];
  });
  env->DeleteGlobalRef(v->peer);
  v->peer = nullptr;
}

// The view's full rectangle and its visible (clipped) part, in display pixels.
void wk_setFrame(JNIEnv*, jobject, jlong h, jint x, jint y, jint w, jint hgt, jint cx, jint cy, jint cw, jint ch,
                 jboolean visible, jint displayWidth) {
  APWebView* v = Get(h);
  dispatch_async(dispatch_get_main_queue(), ^{
    UIView* root = v->clip.superview;
    if (!root || displayWidth <= 0) return;
    CGFloat s = root.bounds.size.width / displayWidth;
    v->clip.frame = CGRectMake(cx * s, cy * s, cw * s, ch * s);
    v->web.frame = CGRectMake((x - cx) * s, (y - cy) * s, w * s, hgt * s);
    bool show = visible && cw > 0 && ch > 0;
    if (show && v->clip.hidden) [root bringSubviewToFront:v->clip];
    v->clip.hidden = !show;
  });
}

void wk_loadUrl(JNIEnv* env, jobject, jlong h, jstring jurl, jobjectArray jheaders) {
  APWebView* v = Get(h);
  NSString* s = NS(env, jurl);
  NSURL* url = s ? [NSURL URLWithString:s] : nil;
  if (!url) return;
  NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:url];
  jsize n = jheaders ? env->GetArrayLength(jheaders) : 0;
  for (jsize i = 0; i + 1 < n; i += 2) {
    jstring k = static_cast<jstring>(env->GetObjectArrayElement(jheaders, i));
    jstring val = static_cast<jstring>(env->GetObjectArrayElement(jheaders, i + 1));
    NSString* ks = NS(env, k);
    NSString* vs = NS(env, val);
    if (ks && vs) [req setValue:vs forHTTPHeaderField:ks];
    env->DeleteLocalRef(k);
    env->DeleteLocalRef(val);
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    v->programmatic = url.absoluteString;
    [v->web loadRequest:req];
  });
}

void wk_loadHtml(JNIEnv* env, jobject, jlong h, jstring jhtml, jstring jbase) {
  APWebView* v = Get(h);
  NSString* html = NS(env, jhtml) ?: @"";
  NSString* base = NS(env, jbase);
  dispatch_async(dispatch_get_main_queue(), ^{
    NSURL* b = base.length ? [NSURL URLWithString:base] : nil;
    v->programmatic = b.absoluteString ?: @"about:blank";
    [v->web loadHTMLString:html baseURL:b];
  });
}

void wk_evaluate(JNIEnv* env, jobject, jlong h, jstring jscript, jint callback) {
  APWebView* v = Get(h);
  NSString* script = NS(env, jscript) ?: @"";
  dispatch_async(dispatch_get_main_queue(), ^{
    [v->web evaluateJavaScript:script completionHandler:^(id result, NSError* error) {
      NSString* json = @"null";  // as Android: the JSON value of the result
      if (result && ![result isKindOfClass:NSNull.class]) {
        NSData* d = [NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingFragmentsAllowed error:nil];
        if (d) json = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
      }
      if (v->peer == nullptr) return;
      JNIEnv* e = Env();
      jstring j = J(e, json);
      e->CallVoidMethod(v->peer, gOnEvalResult, callback, j);
      Check(e);
      if (j) e->DeleteLocalRef(j);
    }];
  });
}

// Scripts that run before every page's own (JS interface shims), and once in the current page.
void wk_addUserScript(JNIEnv* env, jobject, jlong h, jstring jsrc) {
  APWebView* v = Get(h);
  NSString* src = NS(env, jsrc) ?: @"";
  dispatch_async(dispatch_get_main_queue(), ^{
    WKUserScript* us = [[WKUserScript alloc] initWithSource:src injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                           forMainFrameOnly:NO];
    [v->web.configuration.userContentController addUserScript:us];
    [v->web evaluateJavaScript:src completionHandler:nil];
  });
}

void wk_command(JNIEnv*, jobject, jlong h, jint cmd) {
  APWebView* v = Get(h);
  dispatch_async(dispatch_get_main_queue(), ^{
    switch (cmd) {
      case 0: [v->web goBack]; break;
      case 1: [v->web goForward]; break;
      case 2: [v->web reload]; break;
      case 3: [v->web stopLoading]; break;
      case 4: [v->web becomeFirstResponder]; break;
      default: break;
    }
  });
}

jint wk_history(JNIEnv*, jobject, jlong h) {  // bit 0: can go back, bit 1: can go forward
  APWebView* v = Get(h);
  __block jint r = 0;
  OnMain(^{ r = (v->web.canGoBack ? 1 : 0) | (v->web.canGoForward ? 2 : 0); });
  return r;
}

void wk_setUserAgent(JNIEnv* env, jobject, jlong h, jstring jua) {
  APWebView* v = Get(h);
  NSString* ua = NS(env, jua);
  dispatch_async(dispatch_get_main_queue(), ^{ v->web.customUserAgent = ua; });
}

void wk_setBackground(JNIEnv*, jobject, jlong h, jint argb) {
  APWebView* v = Get(h);
  dispatch_async(dispatch_get_main_queue(), ^{
    UIColor* c = [UIColor colorWithRed:((argb >> 16) & 0xff) / 255.0 green:((argb >> 8) & 0xff) / 255.0
                                  blue:(argb & 0xff) / 255.0 alpha:((uint32_t)argb >> 24) / 255.0];
    v->web.backgroundColor = c;
    v->web.scrollView.backgroundColor = c;
  });
}

// ---- CookieManager: WKHTTPCookieStore -------------------------------------------------------

WKHTTPCookieStore* Store() { return WKWebsiteDataStore.defaultDataStore.httpCookieStore; }

void ck_set(JNIEnv* env, jclass, jstring jurl, jstring jvalue) {
  NSURL* url = [NSURL URLWithString:NS(env, jurl) ?: @""];
  NSString* value = NS(env, jvalue);
  if (!url || !value) return;
  NSArray<NSHTTPCookie*>* cookies = [NSHTTPCookie cookiesWithResponseHeaderFields:@{@"Set-Cookie" : value}
                                                                           forURL:url];
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  __block NSInteger pending = cookies.count;
  dispatch_async(dispatch_get_main_queue(), ^{
    if (pending == 0) dispatch_semaphore_signal(done);
    for (NSHTTPCookie* c in cookies) {
      [Store() setCookie:c completionHandler:^{
        if (--pending == 0) dispatch_semaphore_signal(done);
      }];
    }
  });
  if (!NSThread.isMainThread) dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
}

bool Matches(NSHTTPCookie* c, NSURL* url) {
  NSString* host = url.host.lowercaseString ?: @"";
  NSString* domain = c.domain.lowercaseString;
  bool hostOk = [domain hasPrefix:@"."] ? ([host hasSuffix:domain] || [host isEqualToString:[domain substringFromIndex:1]])
                                         : [host isEqualToString:domain];
  NSString* path = url.path.length ? url.path : @"/";
  bool pathOk = [path hasPrefix:c.path ?: @"/"];
  bool secureOk = !c.isSecure || [url.scheme.lowercaseString isEqualToString:@"https"];
  bool fresh = c.expiresDate == nil || c.expiresDate.timeIntervalSinceNow > 0;
  return hostOk && pathOk && secureOk && fresh;
}

jstring ck_get(JNIEnv* env, jclass, jstring jurl) {
  NSURL* url = [NSURL URLWithString:NS(env, jurl) ?: @""];
  if (!url || NSThread.isMainThread) return nullptr;  // (the store answers on the main thread)
  __block NSString* header = nil;
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  dispatch_async(dispatch_get_main_queue(), ^{
    [Store() getAllCookies:^(NSArray<NSHTTPCookie*>* all) {
      NSMutableArray* parts = [NSMutableArray array];
      for (NSHTTPCookie* c in all) {
        if (Matches(c, url)) [parts addObject:[NSString stringWithFormat:@"%@=%@", c.name, c.value]];
      }
      header = parts.count ? [parts componentsJoinedByString:@"; "] : nil;
      dispatch_semaphore_signal(done);
    }];
  });
  dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
  return J(env, header);
}

void ck_removeAll(JNIEnv*, jclass) {
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  dispatch_async(dispatch_get_main_queue(), ^{
    [WKWebsiteDataStore.defaultDataStore removeDataOfTypes:[NSSet setWithObject:WKWebsiteDataTypeCookies]
                                             modifiedSince:NSDate.distantPast
                                         completionHandler:^{ dispatch_semaphore_signal(done); }];
  });
  if (!NSThread.isMainThread) dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
}

}  // namespace

extern "C" void andropkg_register_webview(JNIEnv* env, jclass cls) {
  if (gCls != nullptr || cls == nullptr) return;
  env->GetJavaVM(&gVm);
  static const JNINativeMethod kMethods[] = {
      {"nativeCreate", "(Ljava/lang/String;Z)J", reinterpret_cast<void*>(wk_create)},
      {"nativeDestroy", "(J)V", reinterpret_cast<void*>(wk_destroy)},
      {"nativeSetFrame", "(JIIIIIIIIZI)V", reinterpret_cast<void*>(wk_setFrame)},
      {"nativeLoadUrl", "(JLjava/lang/String;[Ljava/lang/String;)V", reinterpret_cast<void*>(wk_loadUrl)},
      {"nativeLoadHtml", "(JLjava/lang/String;Ljava/lang/String;)V", reinterpret_cast<void*>(wk_loadHtml)},
      {"nativeEvaluate", "(JLjava/lang/String;I)V", reinterpret_cast<void*>(wk_evaluate)},
      {"nativeAddUserScript", "(JLjava/lang/String;)V", reinterpret_cast<void*>(wk_addUserScript)},
      {"nativeCommand", "(JI)V", reinterpret_cast<void*>(wk_command)},
      {"nativeHistory", "(J)I", reinterpret_cast<void*>(wk_history)},
      {"nativeSetUserAgent", "(JLjava/lang/String;)V", reinterpret_cast<void*>(wk_setUserAgent)},
      {"nativeSetBackground", "(JI)V", reinterpret_cast<void*>(wk_setBackground)},
      {"nativeSetCookie", "(Ljava/lang/String;Ljava/lang/String;)V", reinterpret_cast<void*>(ck_set)},
      {"nativeGetCookie", "(Ljava/lang/String;)Ljava/lang/String;", reinterpret_cast<void*>(ck_get)},
      {"nativeRemoveAllCookies", "()V", reinterpret_cast<void*>(ck_removeAll)},
  };
  if (env->RegisterNatives(cls, kMethods, sizeof kMethods / sizeof *kMethods) != JNI_OK) {
    env->ExceptionClear();
    return;
  }
  gOnPageStarted = env->GetMethodID(cls, "onPageStarted", "(Ljava/lang/String;)V");
  gOnPageFinished = env->GetMethodID(cls, "onPageFinished", "(Ljava/lang/String;)V");
  gOnProgress = env->GetMethodID(cls, "onProgress", "(I)V");
  gOnTitle = env->GetMethodID(cls, "onTitle", "(Ljava/lang/String;)V");
  gOnUrl = env->GetMethodID(cls, "onUrl", "(Ljava/lang/String;)V");
  gOnError = env->GetMethodID(cls, "onError", "(ILjava/lang/String;Ljava/lang/String;)V");
  gShouldOverride = env->GetMethodID(cls, "shouldOverride", "(Ljava/lang/String;ZZ)Z");
  gOnJsCall = env->GetMethodID(cls, "onJsCall", "(Ljava/lang/String;)Ljava/lang/String;");
  gOnEvalResult = env->GetMethodID(cls, "onEvalResult", "(ILjava/lang/String;)V");
  gCls = static_cast<jclass>(env->NewGlobalRef(cls));
  if (jfieldID f = env->GetStaticFieldID(cls, "sAvailable", "Z")) env->SetStaticBooleanField(cls, f, JNI_TRUE);
  env->ExceptionClear();
}

#else  // macOS dev loop: no host window; apps keep the text WebView.

extern "C" void andropkg_register_webview(JNIEnv*, jclass) {}

#endif
