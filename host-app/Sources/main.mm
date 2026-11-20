// andropkg runner (M1): boots ART + Android 14 libcore inside an iOS app and runs a dex's
// main(). Output goes to stdout (devicectl --console) and to the screen.
#import <UIKit/UIKit.h>

#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <sys/stat.h>

#include <string>
#include <vector>

static NSString* g_status = @"starting ART…";
static int g_exit_code = 1;
// The screen the Android display maps to (read on the main thread before ART starts).
static int g_screen_w = 1080, g_screen_h = 2400, g_screen_dpi = 420;
static int g_inset_top = 0, g_inset_bottom = 0;  // safe area, pixels

static void Log(NSString* s) {
  printf("[ART-RUNNER] %s\n", s.UTF8String);
  fflush(stdout);
}

// ---- Android window frames -> screen ----------------------------------------------------
// The Android display is the native screen (same pixel size), so a frame maps 1:1.
static UIImageView* g_frame_view;
static void (*g_inject_touch)(int action, float x, float y);

// Touches on the Android window, in window pixels (MotionEvent actions; first finger only
// until multi-touch lands).
@interface AndroidWindowView : UIImageView
@end
@implementation AndroidWindowView {
  UITouch* _tracked;
}
- (void)send:(int)action touch:(UITouch*)t {
  if (!g_inject_touch || !self.image) return;
  CGPoint p = [t locationInView:self];
  CGFloat sx = self.image.size.width * self.image.scale / self.bounds.size.width;
  CGFloat sy = self.image.size.height * self.image.scale / self.bounds.size.height;
  g_inject_touch(action, static_cast<float>(p.x * sx), static_cast<float>(p.y * sy));
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)e {
  if (_tracked) return;
  _tracked = touches.anyObject;
  [self send:0 touch:_tracked];
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)e {
  if ([touches containsObject:_tracked]) [self send:2 touch:_tracked];
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)e {
  if (![touches containsObject:_tracked]) return;
  [self send:1 touch:_tracked];
  _tracked = nil;
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)e {
  if (![touches containsObject:_tracked]) return;
  [self send:3 touch:_tracked];
  _tracked = nil;
}
@end

static void FrameSink(const void* rgba, int w, int h, int stride, void*) {
  // Runs on the app's UI thread (an ART thread, no run loop): drain per frame.
  @autoreleasepool {
  CFDataRef data = CFDataCreate(nullptr, static_cast<const UInt8*>(rgba), static_cast<CFIndex>(stride) * h);
  if (data == nullptr) return;
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGImageRef cg = CGImageCreate(w, h, 8, 32, stride, cs,
                                kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big, provider,
                                nullptr, false, kCGRenderingIntentDefault);
  UIImage* image = [UIImage imageWithCGImage:cg];
  CGImageRelease(cg);
  CGColorSpaceRelease(cs);
  CGDataProviderRelease(provider);
  CFRelease(data);
  static int frames = 0;
  // Keep the first frame (scripts/runner-ios.sh pulls it for checking); with a test tap,
  // keep the latest so the result of the tap can be checked too.
  if (frames++ == 0 || getenv("ANDROPKG_TEST_TAP")) {
    NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    [UIImagePNGRepresentation(image) writeToFile:[docs stringByAppendingPathComponent:@"frame.png"] atomically:YES];
    if (frames == 1) Log([NSString stringWithFormat:@"first frame %dx%d", w, h]);
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    g_frame_view.image = image;
    g_frame_view.hidden = NO;
  });
  }
}

static int RunDex(NSString* dex_path, const char* main_class, const std::vector<std::string>& main_args) {
  NSString* root = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"android-root"];
  NSString* data = [NSTemporaryDirectory() stringByAppendingPathComponent:@"android-data"];
  mkdir(data.fileSystemRepresentation, 0700);
  mkdir([data stringByAppendingPathComponent:@"dalvik-cache"].fileSystemRepresentation, 0700);

  auto path = [&](NSString* rel) { return std::string([root stringByAppendingPathComponent:rel].fileSystemRepresentation); };
  setenv("ANDROID_ROOT", path(@"").c_str(), 1);
  setenv("ANDROID_ART_ROOT", path(@"apex/com.android.art").c_str(), 1);
  setenv("ANDROID_I18N_ROOT", path(@"apex/com.android.i18n").c_str(), 1);
  setenv("ANDROID_TZDATA_ROOT", path(@"apex/com.android.tzdata").c_str(), 1);
  setenv("ANDROID_DATA", data.fileSystemRepresentation, 1);
  setenv("ANDROPKG_ANDROID_ROOT", root.fileSystemRepresentation, 1);  // /system/... -> bundle

  // Full Android 14 BOOTCLASSPATH (device paths, staged into the bundle).
  std::string bcp;
  NSString* list = [NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"bootclasspath.txt"]
                                             encoding:NSUTF8StringEncoding
                                                error:nil];
  for (NSString* dev in [list componentsSeparatedByString:@"\n"]) {
    if (dev.length == 0) continue;
    if (!bcp.empty()) bcp += ":";
    bcp += path([dev substringFromIndex:1]);
  }
  std::vector<std::string> opts = {
      "-Xbootclasspath:" + bcp, "-Xbootclasspath-locations:" + bcp, "-Xusejit:false", "-Xint",
      std::string("-Djava.class.path=") + dex_path.fileSystemRepresentation,
      // andropkg's system services are the platform: they may use @hide APIs.
      "-Xhidden-api-policy:disabled",
      "-Dandropkg.display.width=" + std::to_string(g_screen_w),
      "-Dandropkg.display.height=" + std::to_string(g_screen_h),
      "-Dandropkg.display.dpi=" + std::to_string(g_screen_dpi),
      "-Dandropkg.display.insetTop=" + std::to_string(g_inset_top),
      "-Dandropkg.display.insetBottom=" + std::to_string(g_inset_bottom),
  };
  if (getenv("ANDROPKG_AUTOFINISH")) opts.push_back("-Dandropkg.test.autofinish=1");
  std::vector<JavaVMOption> vm_opts;
  for (auto& o : opts) vm_opts.push_back({const_cast<char*>(o.c_str()), nullptr});
  JavaVMInitArgs args = {JNI_VERSION_1_6, static_cast<jint>(vm_opts.size()), vm_opts.data(), JNI_FALSE};

  CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
  JavaVM* vm = nullptr;
  JNIEnv* env = nullptr;
  if (JNI_CreateJavaVM(&vm, &env, &args) != JNI_OK) {
    Log(@"JNI_CreateJavaVM failed");
    return 1;
  }
  Log([NSString stringWithFormat:@"VM started in %.0f ms", (CFAbsoluteTimeGetCurrent() - t0) * 1000]);
  // libandroid_runtime is loaded by now (Runtime::InitNativeMethods).
  using SetSink = void (*)(void (*)(const void*, int, int, int, void*), void*);
  if (auto set_sink = reinterpret_cast<SetSink>(dlsym(RTLD_DEFAULT, "andropkg_set_frame_sink"))) {
    set_sink(FrameSink, nullptr);
  }
  g_inject_touch = reinterpret_cast<void (*)(int, float, float)>(dlsym(RTLD_DEFAULT, "andropkg_inject_touch"));

  jclass cls = env->FindClass(main_class);
  jmethodID main = cls ? env->GetStaticMethodID(cls, "main", "([Ljava/lang/String;)V") : nullptr;
  if (main == nullptr) {
    env->ExceptionDescribe();
    Log(@"main class/method not found");
    return 1;
  }
  jobjectArray argv = env->NewObjectArray(static_cast<jsize>(main_args.size()),
                                          env->FindClass("java/lang/String"), nullptr);
  for (size_t i = 0; i < main_args.size(); i++) {
    env->SetObjectArrayElement(argv, static_cast<jsize>(i), env->NewStringUTF(main_args[i].c_str()));
  }
  t0 = CFAbsoluteTimeGetCurrent();
  env->CallStaticVoidMethod(cls, main, argv);
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    Log(@"main() threw");
    return 1;
  }
  Log([NSString stringWithFormat:@"main() returned after %.0f ms", (CFAbsoluteTimeGetCurrent() - t0) * 1000]);
  return 0;
}

static void* VMThread(void*) {
  NSString* root = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"android-root"];
  std::vector<std::string> main_args;
  const char* dex_rel;
  const char* main_class;
  if (const char* apk = getenv("ANDROPKG_APK")) {
    // An Android app: andropkg's system services boot ActivityThread for the APK.
    dex_rel = "system/framework/andropkg-services.dex";
    main_class = "dev/andropkg/system/AndroPkgSystem";
    NSString* name = [@(apk).lastPathComponent stringByDeletingPathExtension];
    NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString* data = [[docs stringByAppendingPathComponent:@"data/app"] stringByAppendingPathComponent:name];
    main_args = {[root stringByAppendingPathComponent:@(apk)].fileSystemRepresentation, data.fileSystemRepresentation};
  } else {
    // ANDROPKG_DEX / ANDROPKG_MAIN select a plain-Java test (default: test/hello.dex, Hello).
    dex_rel = getenv("ANDROPKG_DEX") ?: "test/hello.dex";
    main_class = getenv("ANDROPKG_MAIN") ?: "Hello";
  }
  NSString* dex = [root stringByAppendingPathComponent:@(dex_rel)];
  g_exit_code = RunDex(dex, main_class, main_args);
  g_status = g_exit_code == 0 ? @"Hello ran on ART ✓ (see console)" : @"ART run failed (see console)";
  Log([NSString stringWithFormat:@"exit code %d", g_exit_code]);
  if (getenv("ANDROPKG_EXIT")) exit(g_exit_code);
  return nullptr;
}

@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end
@implementation SceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)s options:(UISceneConnectionOptions*)o {
  UILabel* label = [UILabel new];
  label.numberOfLines = 0;
  label.textAlignment = NSTextAlignmentCenter;
  label.text = g_status;
  UIViewController* vc = [UIViewController new];
  UIView* root = [UIView new];
  root.backgroundColor = UIColor.systemBackgroundColor;
  label.frame = root.bounds;
  label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [root addSubview:label];
  vc.view = root;
  [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer*) { label.text = g_status; }];
  g_frame_view = [[AndroidWindowView alloc] initWithFrame:root.bounds];
  g_frame_view.userInteractionEnabled = YES;
  g_frame_view.multipleTouchEnabled = NO;
  g_frame_view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  g_frame_view.contentMode = UIViewContentModeScaleToFill;
  g_frame_view.hidden = YES;
  [root addSubview:g_frame_view];
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
  self.window.rootViewController = vc;
  [self.window makeKeyAndVisible];
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)opts {
  UIScreen* screen = UIScreen.mainScreen;
  g_screen_w = static_cast<int>(screen.nativeBounds.size.width);
  g_screen_h = static_cast<int>(screen.nativeBounds.size.height);
  g_screen_dpi = static_cast<int>(160 * screen.nativeScale);  // Android dp == iOS point
  // Safe area isn't known before a window exists; the key window of a fresh launch has it
  // after the first layout. iPhones with a Dynamic Island: 59 pt top, 34 pt bottom.
  UIWindow* probe = [[UIWindow alloc] initWithFrame:screen.bounds];
  UIEdgeInsets safe = probe.safeAreaInsets;
  if (safe.top == 0) safe = UIEdgeInsetsMake(59, 0, 34, 0);
  g_inset_top = static_cast<int>(safe.top * screen.nativeScale);
  g_inset_bottom = static_cast<int>(safe.bottom * screen.nativeScale);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8 << 20);  // ART expects a large main-thread stack
  pthread_t t;
  pthread_create(&t, &attr, VMThread, nullptr);
  return YES;
}
- (UISceneConfiguration*)application:(UIApplication*)app
    configurationForConnectingSceneSession:(UISceneSession*)session
                                   options:(UISceneConnectionOptions*)options {
  UISceneConfiguration* c = [UISceneConfiguration configurationWithName:@"Default" sessionRole:session.role];
  c.delegateClass = SceneDelegate.class;
  return c;
}
@end

int main(int argc, char* argv[]) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
  }
}
