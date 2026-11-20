// M0 spike runner for iOS. Results go to stdout (captured by `devicectl ... --console`)
// and to an on-screen text view.
#import <UIKit/UIKit.h>
#include "spikes.h"

static NSMutableString *g_out;
static spike_summary g_sum;

static void log_line(const char *line) {
    printf("%s\n", line);
    fflush(stdout);
    [g_out appendFormat:@"%s\n", line];
}

static void run_spikes(void) {
    g_out = [NSMutableString new];
    NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
    log_line([NSString stringWithFormat:@"[SPIKE] INFO device: %@ iOS %ld.%ld.%ld",
                 UIDevice.currentDevice.model, (long)v.majorVersion, (long)v.minorVersion,
                 (long)v.patchVersion].UTF8String);
    spike_run_cpu(log_line, &g_sum);
    NSString *fw = NSBundle.mainBundle.privateFrameworksPath;
    NSArray *libs = @[ @[ @"rela", @"libspike.elf.dylib" ], @[ @"aps2+relr", @"libspike_relr.elf.dylib" ] ];
    for (NSArray *l in libs)
        spike_run_elf([fw stringByAppendingPathComponent:l[1]].fileSystemRepresentation,
                      [l[0] UTF8String], log_line, &g_sum);
    log_line([NSString stringWithFormat:@"[SPIKE] SUMMARY passed=%d failed=%d info=%d", g_sum.passed,
                 g_sum.failed, g_sum.info].UTF8String);
}

@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation SceneDelegate
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options {
    UITextView *text = [UITextView new];
    text.editable = NO;
    text.font = [UIFont monospacedSystemFontOfSize:10 weight:UIFontWeightRegular];
    text.text = g_out;
    UIViewController *vc = [UIViewController new];
    vc.view = text;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts {
    run_spikes();
    if (getenv("ANDROPKG_SPIKE_EXIT")) exit(g_sum.failed ? 1 : 0);
    return YES;
}
- (UISceneConfiguration *)application:(UIApplication *)app
    configurationForConnectingSceneSession:(UISceneSession *)session
                                   options:(UISceneConnectionOptions *)options {
    UISceneConfiguration *c = [UISceneConfiguration configurationWithName:@"Default"
                                                              sessionRole:session.role];
    c.delegateClass = SceneDelegate.class;
    return c;
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
