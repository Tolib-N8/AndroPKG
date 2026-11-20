// andropkg runner (M1): boots ART + Android 14 libcore inside an iOS app and runs a dex's
// main(). Output goes to stdout (devicectl --console) and to the screen.
#import <UIKit/UIKit.h>

#include <jni.h>
#include <pthread.h>
#include <sys/stat.h>

#include <string>
#include <vector>

static NSString* g_status = @"starting ART…";
static int g_exit_code = 1;

static void Log(NSString* s) {
  printf("[ART-RUNNER] %s\n", s.UTF8String);
  fflush(stdout);
}

static int RunDex(NSString* dex_path, const char* main_class) {
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

  std::string bcp;
  for (NSString* jar in @[ @"apex/com.android.art/javalib/core-oj.jar", @"apex/com.android.art/javalib/core-libart.jar",
                           @"apex/com.android.art/javalib/okhttp.jar", @"apex/com.android.art/javalib/bouncycastle.jar",
                           @"apex/com.android.art/javalib/apache-xml.jar", @"apex/com.android.i18n/javalib/core-icu4j.jar" ]) {
    if (!bcp.empty()) bcp += ":";
    bcp += path(jar);
  }
  std::vector<std::string> opts = {
      "-Xbootclasspath:" + bcp, "-Xbootclasspath-locations:" + bcp, "-Xusejit:false", "-Xint",
      std::string("-Djava.class.path=") + dex_path.fileSystemRepresentation,
  };
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

  jclass cls = env->FindClass(main_class);
  jmethodID main = cls ? env->GetStaticMethodID(cls, "main", "([Ljava/lang/String;)V") : nullptr;
  if (main == nullptr) {
    env->ExceptionDescribe();
    Log(@"main class/method not found");
    return 1;
  }
  jobjectArray argv = env->NewObjectArray(0, env->FindClass("java/lang/String"), nullptr);
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
  NSString* dex = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"android-root/test/hello.dex"];
  g_exit_code = RunDex(dex, "Hello");
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
  vc.view = label;
  label.backgroundColor = UIColor.systemBackgroundColor;
  [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer*) { label.text = g_status; }];
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
  self.window.rootViewController = vc;
  [self.window makeKeyAndVisible];
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)opts {
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
