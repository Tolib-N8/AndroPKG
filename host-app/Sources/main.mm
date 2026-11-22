// andropkg runner (M1): boots ART + Android 14 libcore inside an iOS app and runs a dex's
// main(). Output goes to stdout (devicectl --console) and to the screen.
#import <CoreMotion/CoreMotion.h>
#import <UIKit/UIKit.h>

#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <sys/stat.h>

#include <string>
#include <vector>

static NSString* g_status = @"starting ART…";
static UILabel* g_status_label;  // shown until the first frame
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
static void (*g_ime_insert_text)(const char* utf8);
static void (*g_ime_delete_backward)();
static void (*g_ime_enter)();
static void (*g_inject_key)(int action, int keycode);

// Touches on the Android window, in window pixels (MotionEvent actions; first finger only
// until multi-touch lands).
@interface AndroidWindowView : UIImageView <UIKeyInput>
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
// The iOS keyboard types into the focused Android text editor (andropkg's host IME).
- (BOOL)canBecomeFirstResponder {
  return YES;
}
- (BOOL)hasText {
  return YES;
}
- (void)insertText:(NSString*)text {
  if ([text isEqualToString:@"\n"]) {
    if (g_ime_enter) g_ime_enter();
  } else if (g_ime_insert_text) {
    g_ime_insert_text(text.UTF8String);
  }
}
- (void)deleteBackward {
  if (g_ime_delete_backward) g_ime_delete_backward();
}
// The iOS back gesture (swipe from the left edge) is Android's BACK key.
- (void)edgeSwipe:(UIScreenEdgePanGestureRecognizer*)g {
  if (g.state == UIGestureRecognizerStateEnded && g_inject_key) {
    g_inject_key(0, 4 /* KEYCODE_BACK */);
    g_inject_key(1, 4);
  }
}
- (UITextAutocorrectionType)autocorrectionType {
  return UITextAutocorrectionTypeNo;  // no marked/replaced text yet: plain key input
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
    NSString* caches = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
    [UIImagePNGRepresentation(image) writeToFile:[caches stringByAppendingPathComponent:@"frame.png"] atomically:YES];
    if (frames == 1) Log([NSString stringWithFormat:@"first frame %dx%d", w, h]);
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    g_frame_view.image = image;
    g_frame_view.hidden = NO;
    g_status_label.hidden = YES;  // windows may be translucent: no "starting ART…" behind them
  });
  }
}

// ---- Sensors: CoreMotion -> android.hardware.SensorManager ----------------------------------
// Started only while an app listens. iOS and Android share device axes (x right, y up, z out
// of the screen); iOS reports acceleration in g along gravity, Android in m/s^2 against it.
static CMMotionManager* g_motion;
static NSOperationQueue* g_motion_queue;
static void (*g_sensor_push)(int type, const float* values, int count);

static void SensorToggle(int type, int enabled, int period_us) {
  NSTimeInterval interval = MAX(period_us, 10000) / 1e6;  // SENSOR_DELAY_FASTEST (0) -> 100 Hz
  dispatch_async(dispatch_get_main_queue(), ^{
    switch (type) {
      case 1:  // TYPE_ACCELEROMETER
        if (!enabled) { [g_motion stopAccelerometerUpdates]; break; }
        g_motion.accelerometerUpdateInterval = interval;
        [g_motion startAccelerometerUpdatesToQueue:g_motion_queue withHandler:^(CMAccelerometerData* d, NSError*) {
          if (!d) return;
          const float g = 9.80665f;
          float v[3] = {(float)-d.acceleration.x * g, (float)-d.acceleration.y * g, (float)-d.acceleration.z * g};
          g_sensor_push(1, v, 3);
        }];
        break;
      case 4:  // TYPE_GYROSCOPE (rad/s, same axes and sign)
        if (!enabled) { [g_motion stopGyroUpdates]; break; }
        g_motion.gyroUpdateInterval = interval;
        [g_motion startGyroUpdatesToQueue:g_motion_queue withHandler:^(CMGyroData* d, NSError*) {
          if (!d) return;
          float v[3] = {(float)d.rotationRate.x, (float)d.rotationRate.y, (float)d.rotationRate.z};
          g_sensor_push(4, v, 3);
        }];
        break;
      case 2:  // TYPE_MAGNETIC_FIELD (microtesla)
        if (!enabled) { [g_motion stopMagnetometerUpdates]; break; }
        g_motion.magnetometerUpdateInterval = interval;
        [g_motion startMagnetometerUpdatesToQueue:g_motion_queue withHandler:^(CMMagnetometerData* d, NSError*) {
          if (!d) return;
          float v[3] = {(float)d.magneticField.x, (float)d.magneticField.y, (float)d.magneticField.z};
          g_sensor_push(2, v, 3);
        }];
        break;
    }
    Log([NSString stringWithFormat:@"sensor %d %@", type, enabled ? @"on" : @"off"]);
  });
}

static void InstallSensors() {
  using SetHost = void (*)(const int*, int, void (*)(int, int, int));
  auto set_host = reinterpret_cast<SetHost>(dlsym(RTLD_DEFAULT, "andropkg_sensors_set_host"));
  g_sensor_push = reinterpret_cast<void (*)(int, const float*, int)>(dlsym(RTLD_DEFAULT, "andropkg_sensor_push"));
  if (!set_host || !g_sensor_push) return;
  g_motion = [[CMMotionManager alloc] init];
  g_motion_queue = [[NSOperationQueue alloc] init];
  g_motion_queue.maxConcurrentOperationCount = 1;
  std::vector<int> types;
  if (g_motion.accelerometerAvailable) types.push_back(1);
  if (g_motion.gyroAvailable) types.push_back(4);
  if (g_motion.magnetometerAvailable) types.push_back(2);
  set_host(types.data(), static_cast<int>(types.size()), SensorToggle);
}

static std::vector<std::string> g_extra_vm_options;  // e.g. -Dandropkg.nativeLibraryDir=...

static int RunDex(NSString* dex_path, const char* main_class, const std::vector<std::string>& main_args) {
  NSString* root = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"android-root"];
  // /data lives in Library/Application Support: tmp/ is purged by iOS (logins, databases lost).
  NSString* support =
      NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
  [NSFileManager.defaultManager createDirectoryAtPath:support withIntermediateDirectories:YES attributes:nil error:nil];
  NSString* data = [support stringByAppendingPathComponent:@"android-data"];
  NSString* old_data = [NSTemporaryDirectory() stringByAppendingPathComponent:@"android-data"];
  if (access(data.fileSystemRepresentation, F_OK) != 0 && access(old_data.fileSystemRepresentation, F_OK) == 0)
    rename(old_data.fileSystemRepresentation, data.fileSystemRepresentation);
  mkdir(data.fileSystemRepresentation, 0700);
  [[NSURL fileURLWithPath:data] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
  mkdir([data stringByAppendingPathComponent:@"dalvik-cache"].fileSystemRepresentation, 0700);

  auto path = [&](NSString* rel) { return std::string([root stringByAppendingPathComponent:rel].fileSystemRepresentation); };
  setenv("ANDROID_ROOT", path(@"").c_str(), 1);
  setenv("ANDROID_ART_ROOT", path(@"apex/com.android.art").c_str(), 1);
  setenv("ANDROID_I18N_ROOT", path(@"apex/com.android.i18n").c_str(), 1);
  setenv("ANDROID_TZDATA_ROOT", path(@"apex/com.android.tzdata").c_str(), 1);
  setenv("ANDROID_DATA", data.fileSystemRepresentation, 1);
  setenv("ANDROPKG_ANDROID_ROOT", root.fileSystemRepresentation, 1);  // /system/... -> bundle
  // Shared storage (/storage/emulated/0, /sdcard): the app's Documents, shown in the Files app.
  NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
  setenv("ANDROPKG_STORAGE_DIR", docs.fileSystemRepresentation, 1);

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
      "-Xbootclasspath:" + bcp, "-Xbootclasspath-locations:" + bcp, "-Xusejit:false",
      // -Xint keeps every method in the switch interpreter; nterp (ANDROPKG_NTERP) needs it off.
      getenv("ANDROPKG_NTERP") ? "-Xusejit:false" : "-Xint",
      // A phone's heap configuration (dalvik.vm.heapgrowthlimit / heapsize).
      "-Xms8m", "-Xmx512m", "-XX:HeapGrowthLimit=256m", "-XX:HeapTargetUtilization=0.75",
      "-XX:HeapMinFree=2m", "-XX:HeapMaxFree=8m",
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
  for (const auto& o : g_extra_vm_options) opts.push_back(o);
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
  g_ime_insert_text = reinterpret_cast<void (*)(const char*)>(dlsym(RTLD_DEFAULT, "andropkg_ime_insert_text"));
  g_ime_delete_backward = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "andropkg_ime_delete_backward"));
  g_ime_enter = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "andropkg_ime_enter"));
  g_inject_key = reinterpret_cast<void (*)(int, int)>(dlsym(RTLD_DEFAULT, "andropkg_inject_key"));
  if (auto set_ime = reinterpret_cast<void (*)(void (*)(int))>(dlsym(RTLD_DEFAULT, "andropkg_set_ime_listener"))) {
    set_ime([](int visible) {
      dispatch_async(dispatch_get_main_queue(), ^{
        if (visible) {
          [g_frame_view becomeFirstResponder];
        } else {
          [g_frame_view resignFirstResponder];
        }
        Log(visible ? @"keyboard shown" : @"keyboard hidden");
      });
    });
  }

  InstallSensors();

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
  // The APK to run: ANDROPKG_APK (scripts/runner-ios.sh), else the last one run that way, so
  // tapping the home-screen icon opens that Android app.
  NSString* lastApk = [NSUserDefaults.standardUserDefaults stringForKey:@"andropkg.lastApk"];
  if (const char* env_apk = getenv("ANDROPKG_APK")) {
    [NSUserDefaults.standardUserDefaults setObject:@(env_apk) forKey:@"andropkg.lastApk"];
    lastApk = @(env_apk);
  }
  bool plain_java = getenv("ANDROPKG_DEX") || getenv("ANDROPKG_MAIN");
  if (const char* apk = (lastApk && !plain_java) ? lastApk.UTF8String : nullptr) {
    // An Android app: andropkg's system services boot ActivityThread for the APK.
    dex_rel = "system/framework/andropkg-services.dex";
    main_class = "dev/andropkg/system/AndroPkgSystem";
    NSString* name = [@(apk).lastPathComponent stringByDeletingPathExtension];
    // App-private data (/data/user/0/<pkg>): Library/Application Support, not user-visible.
    NSString* support =
        NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
    NSString* data = [[support stringByAppendingPathComponent:@"andropkg/data/app"] stringByAppendingPathComponent:name];
    [NSFileManager.defaultManager createDirectoryAtPath:data withIntermediateDirectories:YES attributes:nil error:nil];
    main_args = {[root stringByAppendingPathComponent:@(apk)].fileSystemRepresentation, data.fileSystemRepresentation};
    // Converted, signed native libraries of this APK (scripts/prepare-native-libs.sh).
    NSString* native = [[root stringByAppendingPathComponent:@"native"] stringByAppendingPathComponent:name];
    if ([NSFileManager.defaultManager fileExistsAtPath:native]) {
      g_extra_vm_options.push_back(std::string("-Dandropkg.nativeLibraryDir=") + native.fileSystemRepresentation);
    }
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

// iOS app lifecycle -> the Android task (top activity paused+stopped / restarted+resumed).
static void SetAppVisible(int visible) {
  auto set = reinterpret_cast<void (*)(int)>(dlsym(RTLD_DEFAULT, "andropkg_app_visibility"));  // after ART is up
  if (set) set(visible);
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
  g_status_label = label;
  UIViewController* vc = [UIViewController new];
  UIView* root = [UIView new];
  root.backgroundColor = UIColor.blackColor;  // what shows through translucent windows
  label.frame = root.bounds;
  label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [root addSubview:label];
  vc.view = root;
  [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer*) { label.text = g_status; }];
  g_frame_view = [[AndroidWindowView alloc] initWithFrame:root.bounds];
  g_frame_view.userInteractionEnabled = YES;
  g_frame_view.multipleTouchEnabled = NO;
  UIScreenEdgePanGestureRecognizer* back =
      [[UIScreenEdgePanGestureRecognizer alloc] initWithTarget:g_frame_view action:@selector(edgeSwipe:)];
  back.edges = UIRectEdgeLeft;
  [g_frame_view addGestureRecognizer:back];
  g_frame_view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  g_frame_view.contentMode = UIViewContentModeScaleToFill;
  g_frame_view.hidden = YES;
  [root addSubview:g_frame_view];
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
  [NSNotificationCenter.defaultCenter addObserverForName:UISceneDidEnterBackgroundNotification object:scene
                                                   queue:nil usingBlock:^(NSNotification*) { SetAppVisible(0); }];
  [NSNotificationCenter.defaultCenter addObserverForName:UISceneWillEnterForegroundNotification object:scene
                                                   queue:nil usingBlock:^(NSNotification*) { SetAppVisible(1); }];
  self.window.rootViewController = vc;
  [self.window makeKeyAndVisible];
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)opts {
  UIScreen* screen = UIScreen.mainScreen;
  // Android renders on the CPU, so it draws at (at most) 2 pixels per point and the window is
  // scaled to the screen: a 3x iPhone has 2.25x the pixels to fill per frame otherwise.
  // ANDROPKG_RENDER_SCALE overrides (e.g. 3 for native resolution).
  CGFloat scale = MIN(screen.nativeScale, 2.0);
  if (const char* env = getenv("ANDROPKG_RENDER_SCALE")) {
    if (atof(env) >= 1.0) scale = MIN(atof(env), screen.nativeScale);
  }
  g_screen_w = static_cast<int>(lround(screen.bounds.size.width * scale));
  g_screen_h = static_cast<int>(lround(screen.bounds.size.height * scale));
  g_screen_dpi = static_cast<int>(160 * scale);  // Android dp == iOS point
  // Safe area isn't known before a window exists; the key window of a fresh launch has it
  // after the first layout. iPhones with a Dynamic Island: 59 pt top, 34 pt bottom.
  UIWindow* probe = [[UIWindow alloc] initWithFrame:screen.bounds];
  UIEdgeInsets safe = probe.safeAreaInsets;
  if (safe.top == 0) safe = UIEdgeInsetsMake(59, 0, 34, 0);
  g_inset_top = static_cast<int>(safe.top * scale);
  g_inset_bottom = static_cast<int>(safe.bottom * scale);
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
