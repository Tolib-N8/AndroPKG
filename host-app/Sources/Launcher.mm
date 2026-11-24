// andropkg host — the launcher: the Android apps this app can run, and APK import.
//
// Shown when AndroPkg opens without an APK to run (no ANDROPKG_APK from the runner, no
// AndroPkgAPK of a converted IPA). Apps come from two places:
//   - built in: android-root/test/<name>.apk or <name>/ (a split package) in the bundle;
//   - imported: Application Support/andropkg/apps/<name>.apk, from the Files app, the share
//     sheet ("Open in AndroPkg") or the + button.
// An app's name and icon are written by the runtime the first time it runs (AndroPkgSystem,
// -Dandropkg.metaDir): Application Support/andropkg/meta/<name>/{label,icon.png}.
// An APK with native libraries needs them converted and signed: on a computer
// (android-root/native/<name>, scripts/convert-ipa.sh) or here, the first time it opens, with
// the signing certificate imported once (Sources/NativeInstaller.swift).
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "AndroPkgRunner-Swift.h"


#import "Launcher.h"

namespace {

NSString* SupportDir(NSString* sub) {
  NSString* support =
      NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
  NSString* dir = [[support stringByAppendingPathComponent:@"andropkg"] stringByAppendingPathComponent:sub];
  [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
  return dir;
}

NSString* AndroidRoot() { return [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"android-root"]; }

// True if the zip at `path` has entries under lib/arm64-v8a/ (its central directory).
bool HasNativeLibraries(NSString* path) {
  NSData* data = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];
  const uint8_t* b = static_cast<const uint8_t*>(data.bytes);
  size_t n = data.length;
  if (n < 22) return false;
  auto u16 = [&](size_t o) { return static_cast<uint32_t>(b[o] | b[o + 1] << 8); };
  auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
  size_t eocd = n - 22;
  size_t stop = n > 65557 ? n - 65557 : 0;
  while (eocd > stop && u32(eocd) != 0x06054b50) eocd--;
  if (u32(eocd) != 0x06054b50) return false;
  size_t off = u32(eocd + 16), count = u16(eocd + 10);
  for (size_t i = 0; i < count && off + 46 <= n && u32(off) == 0x02014b50; i++) {
    size_t name_len = u16(off + 28), extra = u16(off + 30), comment = u16(off + 32);
    if (off + 46 + name_len > n) break;
    const char* name = reinterpret_cast<const char*>(b + off + 46);
    static const char kPrefix[] = "lib/arm64-v8a/";
    if (name_len > sizeof kPrefix - 1 && memcmp(name, kPrefix, sizeof kPrefix - 1) == 0) return true;
    off += 46 + name_len + extra + comment;
  }
  return false;
}

}  // namespace

@interface AndropkgApp : NSObject
@property(nonatomic, copy) NSString* name;    // file name without .apk: the data directory's name
@property(nonatomic, copy) NSString* path;    // the APK (or a split package's directory)
@property(nonatomic) BOOL builtIn;
@property(nonatomic) BOOL needsNative;        // native libraries, not converted on a computer
@end
@implementation AndropkgApp
- (NSString*)metaDir { return [SupportDir(@"meta") stringByAppendingPathComponent:self.name]; }
- (NSString*)label {
  NSString* l = [NSString stringWithContentsOfFile:[self.metaDir stringByAppendingPathComponent:@"label"]
                                          encoding:NSUTF8StringEncoding error:nil];
  return l.length ? l : self.name;
}
- (UIImage*)icon { return [UIImage imageWithContentsOfFile:[self.metaDir stringByAppendingPathComponent:@"icon.png"]]; }
@end

NSString* AndropkgMetaDir(NSString* name) {
  NSString* dir = [SupportDir(@"meta") stringByAppendingPathComponent:name];
  [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
  return dir;
}

static NSArray<AndropkgApp*>* InstalledApps() {
  NSMutableArray<AndropkgApp*>* apps = [NSMutableArray array];
  NSFileManager* fm = NSFileManager.defaultManager;
  NSString* test = [AndroidRoot() stringByAppendingPathComponent:@"test"];
  NSString* native = [AndroidRoot() stringByAppendingPathComponent:@"native"];
  for (NSString* f in [[fm contentsOfDirectoryAtPath:test error:nil] sortedArrayUsingSelector:@selector(compare:)]) {
    NSString* p = [test stringByAppendingPathComponent:f];
    BOOL dir = NO;
    [fm fileExistsAtPath:p isDirectory:&dir];
    if (dir ? ![fm fileExistsAtPath:[p stringByAppendingPathComponent:@"base.apk"]] : ![f hasSuffix:@".apk"]) continue;
    AndropkgApp* a = [AndropkgApp new];
    a.name = dir ? f : f.stringByDeletingPathExtension;
    a.path = p;
    a.builtIn = YES;
    NSString* base = dir ? [p stringByAppendingPathComponent:@"base.apk"] : p;
    a.needsNative = HasNativeLibraries(base) && ![fm fileExistsAtPath:[native stringByAppendingPathComponent:a.name]];
    [apps addObject:a];
  }
  NSString* imported = SupportDir(@"apps");
  for (NSString* f in [[fm contentsOfDirectoryAtPath:imported error:nil] sortedArrayUsingSelector:@selector(compare:)]) {
    // <name>.apk, or a split install from an app store: <package>/base.apk + split APKs.
    NSString* p = [imported stringByAppendingPathComponent:f];
    BOOL dir = NO;
    [fm fileExistsAtPath:p isDirectory:&dir];
    NSString* base = dir ? [p stringByAppendingPathComponent:@"base.apk"] : p;
    if (dir ? ![fm fileExistsAtPath:base] : ![f hasSuffix:@".apk"]) continue;
    AndropkgApp* a = [AndropkgApp new];
    a.name = dir ? f : f.stringByDeletingPathExtension;
    a.path = p;  // (a directory: the runtime parses the cluster)
    a.needsNative = HasNativeLibraries(base);
    [apps addObject:a];
  }
  return apps;
}

BOOL AndropkgImportApk(NSURL* url, NSString** error) {
  BOOL scoped = [url startAccessingSecurityScopedResource];
  NSString* name = url.lastPathComponent.stringByDeletingPathExtension;
  // A file-system friendly name: it names the app's data directory too.
  NSCharacterSet* bad = [NSCharacterSet characterSetWithCharactersInString:@"/\\:?*\"<>| "];
  name = [[name componentsSeparatedByCharactersInSet:bad] componentsJoinedByString:@"_"];
  if (name.length == 0) name = @"app";
  NSString* dest = [[SupportDir(@"apps") stringByAppendingPathComponent:name] stringByAppendingPathExtension:@"apk"];
  NSFileManager* fm = NSFileManager.defaultManager;
  [fm removeItemAtPath:dest error:nil];
  NSError* e = nil;
  BOOL ok = [fm copyItemAtURL:url toURL:[NSURL fileURLWithPath:dest] error:&e];
  if (scoped) [url stopAccessingSecurityScopedResource];
  if (!ok && error) *error = e.localizedDescription;
  // A new version: its name and icon are read again on its next start.
  if (ok) [fm removeItemAtPath:AndropkgMetaDir(name) error:nil];
  return ok;
}

@interface AndropkgLauncher : UITableViewController <UIDocumentPickerDelegate>
@property(nonatomic, copy) void (^open)(NSString* apkPath);
@property(nonatomic, copy) NSArray<AndropkgApp*>* apps;
@end

@implementation AndropkgLauncher
- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = @"AndroPkg";
  self.navigationController.navigationBar.prefersLargeTitles = YES;
  self.tableView.rowHeight = 72;
  self.navigationItem.rightBarButtonItem =
      [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd target:self action:@selector(add)];
  [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(reload) name:AndropkgAppsChanged object:nil];
  [self reload];
}
- (void)reload {
  self.apps = InstalledApps();
  [self.tableView reloadData];
  if (self.apps.count == 0) {
    UILabel* empty = [UILabel new];
    empty.numberOfLines = 0;
    empty.textAlignment = NSTextAlignmentCenter;
    empty.textColor = UIColor.secondaryLabelColor;
    empty.text = @"Нет приложений.\nОткройте файл .apk в AndroPkg\n(«Файлы», «Поделиться») или нажмите +.";
    self.tableView.backgroundView = empty;
  } else {
    self.tableView.backgroundView = nil;
  }
}
- (NSInteger)tableView:(UITableView*)t numberOfRowsInSection:(NSInteger)s { return self.apps.count; }
- (UITableViewCell*)tableView:(UITableView*)t cellForRowAtIndexPath:(NSIndexPath*)ip {
  UITableViewCell* c = [t dequeueReusableCellWithIdentifier:@"app"]
                           ?: [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"app"];
  AndropkgApp* a = self.apps[ip.row];
  UIListContentConfiguration* cfg = c.defaultContentConfiguration;
  cfg.text = a.label;
  BOOL pending = a.needsNative && ![AndropkgNativeInstaller isPreparedWithApp:a.name];
  BOOL noCert = pending && !AndropkgNativeInstaller.hasCertificate;
  cfg.secondaryText = noCert ? @"Нативный код: нужен сертификат подписи"
                      : pending ? @"Нативный код подготовится при первом запуске"
                                : (a.builtIn ? @"Встроенное" : @"Установлено");
  cfg.secondaryTextProperties.color = noCert ? UIColor.systemOrangeColor : UIColor.secondaryLabelColor;
  UIImage* icon = a.icon;
  if (!icon) icon = [UIImage systemImageNamed:@"app.dashed"];
  cfg.image = icon;
  cfg.imageProperties.maximumSize = CGSizeMake(52, 52);
  cfg.imageProperties.reservedLayoutSize = CGSizeMake(52, 52);
  cfg.imageProperties.cornerRadius = 12;
  c.contentConfiguration = cfg;
  c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  return c;
}
- (void)tableView:(UITableView*)t didSelectRowAtIndexPath:(NSIndexPath*)ip {
  [t deselectRowAtIndexPath:ip animated:YES];
  AndropkgApp* a = self.apps[ip.row];
  if (a.needsNative && ![AndropkgNativeInstaller isPreparedWithApp:a.name]) {
    if (!AndropkgNativeInstaller.hasCertificate) {
      [self askForCertificate:a.label];
    } else {
      [self prepare:a];
    }
    return;
  }
  if (self.open) self.open(a.path);
}
// The app's native libraries: converted and signed here, then it opens.
- (void)prepare:(AndropkgApp*)a {
  UIAlertController* progress = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"Подготовка «%@»", a.label]
                                                                    message:@"Конвертация нативных библиотек…"
                                                             preferredStyle:UIAlertControllerStyleAlert];
  [self presentViewController:progress animated:YES completion:nil];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    NSString* err = [AndropkgNativeInstaller prepareWithApk:a.path app:a.name progress:^(NSString* lib, NSInteger done, NSInteger total) {
      dispatch_async(dispatch_get_main_queue(), ^{
        progress.message = [NSString stringWithFormat:@"%@\n(%ld из %ld)", lib, (long)done + 1, (long)total];
      });
    }];
    dispatch_async(dispatch_get_main_queue(), ^{
      [progress dismissViewControllerAnimated:YES completion:^{
        if (err) {
          [self message:@"Не удалось подготовить" text:err];
        } else if (self.open) {
          self.open(a.path);
        }
      }];
    });
  });
}
- (void)message:(NSString*)title text:(NSString*)text {
  UIAlertController* alert = [UIAlertController alertControllerWithTitle:title message:text
                                                          preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
  UIViewController* top = self;
  while (top.presentedViewController) top = top.presentedViewController;
  [top presentViewController:alert animated:YES completion:nil];
}
- (void)askForCertificate:(NSString*)label {
  NSString* team = AndropkgNativeInstaller.appTeam ?: @"?";
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:@"Нужен сертификат подписи"
                       message:[NSString stringWithFormat:
                                   @"В «%@» есть нативные библиотеки. iOS загрузит их, только если они подписаны тем же "
                                   @"сертификатом, что и AndroPkg (команда %@).\n\nЭкспортируйте его с закрытым ключом "
                                   @"(.p12) из «Связки ключей» на Mac и откройте файл в AndroPkg. Это нужно один раз.",
                                   label, team]
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:@"Выбрать файл .p12" style:UIAlertActionStyleDefault
                                          handler:^(UIAlertAction*) { [self pickCertificate]; }]];
  [alert addAction:[UIAlertAction actionWithTitle:@"Отмена" style:UIAlertActionStyleCancel handler:nil]];
  [self presentViewController:alert animated:YES completion:nil];
}
- (void)pickCertificate {
  UTType* p12 = [UTType typeWithIdentifier:@"com.rsa.pkcs-12"] ?: UTTypeData;
  UIDocumentPickerViewController* picker =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ p12, UTTypeData ] asCopy:YES];
  picker.delegate = self;
  [self presentViewController:picker animated:YES completion:nil];
}
// A .p12 (picked, or opened from Files): its password, then the import.
- (void)importCertificate:(NSString*)path {
  UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Пароль сертификата"
                                                                 message:path.lastPathComponent
                                                          preferredStyle:UIAlertControllerStyleAlert];
  [alert addTextFieldWithConfigurationHandler:^(UITextField* f) {
    f.secureTextEntry = YES;
    f.placeholder = @"Пароль .p12";
  }];
  __weak UIAlertController* weak = alert;
  [alert addAction:[UIAlertAction actionWithTitle:@"Импортировать" style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
    NSString* err = [AndropkgNativeInstaller importCertificate:path password:weak.textFields.firstObject.text ?: @""];
    [NSFileManager.defaultManager removeItemAtPath:path error:nil];
    if (err) {
      [self message:@"Сертификат не импортирован" text:err];
    } else {
      [self message:@"Сертификат импортирован" text:@"Приложения с нативным кодом теперь подготавливаются на телефоне."];
      [self reload];
    }
  }]];
  [alert addAction:[UIAlertAction actionWithTitle:@"Отмена" style:UIAlertActionStyleCancel handler:^(UIAlertAction*) {
    [NSFileManager.defaultManager removeItemAtPath:path error:nil];
  }]];
  UIViewController* top = self;
  while (top.presentedViewController) top = top.presentedViewController;
  [top presentViewController:alert animated:YES completion:nil];
}
- (UISwipeActionsConfiguration*)tableView:(UITableView*)t trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath*)ip {
  AndropkgApp* a = self.apps[ip.row];
  if (a.builtIn) return nil;
  UIContextualAction* del = [UIContextualAction
      contextualActionWithStyle:UIContextualActionStyleDestructive
                          title:@"Удалить"
                        handler:^(UIContextualAction*, UIView*, void (^done)(BOOL)) {
                          NSFileManager* fm = NSFileManager.defaultManager;
                          [fm removeItemAtPath:a.path error:nil];
                          [fm removeItemAtPath:a.metaDir error:nil];
                          [fm removeItemAtPath:[SupportDir(@"data/app") stringByAppendingPathComponent:a.name] error:nil];
                          [self reload];
                          done(YES);
                        }];
  return [UISwipeActionsConfiguration configurationWithActions:@[ del ]];
}
- (void)add {
  UTType* apk = [UTType typeWithIdentifier:@"com.android.package-archive"] ?: UTTypeData;
  UIDocumentPickerViewController* picker =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ apk, UTTypeData ] asCopy:YES];
  picker.delegate = self;
  [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController*)p didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  for (NSURL* u in urls) {
    NSString* ext = u.pathExtension.lowercaseString;
    if ([ext isEqualToString:@"p12"] || [ext isEqualToString:@"pfx"]) {
      AndropkgCertificateOpened(u);
      continue;
    }
    NSString* err = nil;
    if (!AndropkgImportApk(u, &err)) {
      UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Не удалось установить"
                                                                     message:err preferredStyle:UIAlertControllerStyleAlert];
      [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
      [self presentViewController:alert animated:YES completion:nil];
    }
  }
  [self reload];
}
@end

NSString* const AndropkgAppsChanged = @"AndropkgAppsChanged";

static __weak AndropkgLauncher* g_shown_launcher;

void AndropkgCertificateOpened(NSURL* url) {
  // A private copy (the file may be security-scoped or in the Inbox), removed after the import.
  BOOL scoped = [url startAccessingSecurityScopedResource];
  NSString* copy = [NSTemporaryDirectory() stringByAppendingPathComponent:url.lastPathComponent];
  [NSFileManager.defaultManager removeItemAtPath:copy error:nil];
  BOOL ok = [NSFileManager.defaultManager copyItemAtPath:url.path toPath:copy error:nil];
  if (scoped) [url stopAccessingSecurityScopedResource];
  AndropkgLauncher* l = g_shown_launcher;
  if (!ok || !l) {
    NSLog(@"andropkg: certificate %@ not imported (%@)", url.lastPathComponent, ok ? @"no launcher" : @"unreadable");
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{ [l importCertificate:copy]; });
}

NSString* AndropkgDeviceNativeDir(NSString* name) { return [AndropkgNativeInstaller nativeDirForApp:name]; }

UIViewController* AndropkgLauncherController(void (^open)(NSString* apkPath)) {
  AndropkgLauncher* l = [[AndropkgLauncher alloc] initWithStyle:UITableViewStyleInsetGrouped];
  g_shown_launcher = l;
  l.open = open;
  return [[UINavigationController alloc] initWithRootViewController:l];
}

// ---- the way back to the launcher ----
// One ART per process: going home ends the app (paused and stopped first, so it saves its
// state), the host goes to the iOS home screen and exits; opening AndroPkg again shows the list.

@interface AndropkgHomeButton : UIButton
@property(nonatomic, copy) void (^home)(void);
@property(nonatomic) CGPoint grab;
@property(nonatomic) BOOL dragged;
@end

@implementation AndropkgHomeButton
- (instancetype)init {
  if ((self = [super initWithFrame:CGRectMake(0, 0, 44, 44)])) {
    self.backgroundColor = [UIColor colorWithWhite:0.1 alpha:0.35];
    self.layer.cornerRadius = 22;
    self.tintColor = [UIColor colorWithWhite:1 alpha:0.9];
    UIImageSymbolConfiguration* c = [UIImageSymbolConfiguration configurationWithPointSize:18 weight:UIImageSymbolWeightSemibold];
    [self setImage:[UIImage systemImageNamed:@"square.grid.2x2" withConfiguration:c] forState:UIControlStateNormal];
    self.accessibilityLabel = @"К списку приложений";
    [self addTarget:self action:@selector(tapped) forControlEvents:UIControlEventTouchUpInside];
    UIPanGestureRecognizer* pan = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(drag:)];
    [self addGestureRecognizer:pan];
  }
  return self;
}
- (void)drag:(UIPanGestureRecognizer*)g {
  UIView* sv = self.superview;
  CGPoint p = [g locationInView:sv];
  CGRect safe = UIEdgeInsetsInsetRect(sv.bounds, sv.safeAreaInsets);
  CGFloat r = 22;
  p.x = MIN(MAX(p.x, CGRectGetMinX(safe) + r), CGRectGetMaxX(safe) - r);
  p.y = MIN(MAX(p.y, CGRectGetMinY(safe) + r), CGRectGetMaxY(safe) - r);
  if (g.state == UIGestureRecognizerStateEnded) {  // to the nearer side
    p.x = p.x < CGRectGetMidX(safe) ? CGRectGetMinX(safe) + r : CGRectGetMaxX(safe) - r;
    [UIView animateWithDuration:0.2 animations:^{ self.center = p; }];
    [NSUserDefaults.standardUserDefaults setObject:NSStringFromCGPoint(CGPointMake(
        p.x < CGRectGetMidX(safe) ? 0 : 1, (p.y - CGRectGetMinY(safe)) / MAX(1, safe.size.height)))
                                            forKey:@"andropkg.homeButton"];
  } else {
    self.center = p;
  }
}
- (void)layoutInSuperview {
  UIView* sv = self.superview;
  CGRect safe = UIEdgeInsetsInsetRect(sv.bounds, sv.safeAreaInsets);
  NSString* saved = [NSUserDefaults.standardUserDefaults stringForKey:@"andropkg.homeButton"];
  CGPoint rel = saved ? CGPointFromString(saved) : CGPointMake(1, 0.6);
  self.center = CGPointMake(rel.x < 0.5 ? CGRectGetMinX(safe) + 22 : CGRectGetMaxX(safe) - 22,
                            CGRectGetMinY(safe) + 22 + rel.y * MAX(0, safe.size.height - 44));
}
- (void)tapped {
  UIViewController* top = self.window.rootViewController;
  while (top.presentedViewController) top = top.presentedViewController;
  UIAlertController* a = [UIAlertController alertControllerWithTitle:@"К списку приложений?"
                                                             message:@"Приложение будет закрыто."
                                                      preferredStyle:UIAlertControllerStyleActionSheet];
  [a addAction:[UIAlertAction actionWithTitle:@"К списку" style:UIAlertActionStyleDestructive
                                      handler:^(UIAlertAction*) { if (self.home) self.home(); }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Отмена" style:UIAlertActionStyleCancel handler:nil]];
  a.popoverPresentationController.sourceView = self;
  a.popoverPresentationController.sourceRect = self.bounds;
  [top presentViewController:a animated:YES completion:nil];
}
@end

void AndropkgAddHomeButton(UIView* root) {
  AndropkgHomeButton* b = [AndropkgHomeButton new];
  __weak AndropkgHomeButton* weak = b;
  b.home = ^{
    weak.hidden = YES;
    // To the iOS home screen (the scene's background notification pauses and stops the app),
    // then end the process once the app has had a moment to save.
    // (A background task keeps the process running there: a suspended app's timers wait until it
    // is opened again, which then ended it under the user.)
    UIApplication* app = UIApplication.sharedApplication;
    [app beginBackgroundTaskWithName:@"andropkg.home" expirationHandler:^{ exit(0); }];
    SEL suspend = NSSelectorFromString(@"suspend");
    if ([app respondsToSelector:suspend]) {
      ((void (*)(id, SEL))[app methodForSelector:suspend])(app, suspend);
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)),
                   dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ exit(0); });
  };
  [root addSubview:b];
  dispatch_async(dispatch_get_main_queue(), ^{ [b layoutInSuperview]; });
  b.autoresizingMask = UIViewAutoresizingNone;
}
