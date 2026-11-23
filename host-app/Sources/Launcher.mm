// andropkg host — the launcher: the Android apps this app can run, and APK import.
//
// Shown when AndroPkg opens without an APK to run (no ANDROPKG_APK from the runner, no
// AndroPkgAPK of a converted IPA). Apps come from two places:
//   - built in: android-root/test/<name>.apk or <name>/ (a split package) in the bundle;
//   - imported: Application Support/andropkg/apps/<name>.apk, from the Files app, the share
//     sheet ("Open in AndroPkg") or the + button.
// An app's name and icon are written by the runtime the first time it runs (AndroPkgSystem,
// -Dandropkg.metaDir): Application Support/andropkg/meta/<name>/{label,icon.png}.
// An APK with native libraries runs only if they were converted and signed on a computer
// (android-root/native/<name>); converting them on the device is not done yet.
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>


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
@property(nonatomic) BOOL needsComputer;      // native libraries not converted for this device
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
    a.needsComputer = HasNativeLibraries(base) && ![fm fileExistsAtPath:[native stringByAppendingPathComponent:a.name]];
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
    a.needsComputer = HasNativeLibraries(base);
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
  cfg.secondaryText = a.needsComputer ? @"Нативный код: пока только через компьютер (convert-ipa.sh)"
                                      : (a.builtIn ? @"Встроенное" : @"Установлено");
  cfg.secondaryTextProperties.color = a.needsComputer ? UIColor.systemOrangeColor : UIColor.secondaryLabelColor;
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
  if (a.needsComputer) {
    UIAlertController* alert = [UIAlertController
        alertControllerWithTitle:a.label
                         message:@"В этом приложении есть нативные библиотеки. Их пока нужно сконвертировать и "
                                 @"подписать на компьютере: scripts/convert-ipa.sh. Конвертация на телефоне — "
                                 @"следующий этап."
                  preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
    return;
  }
  if (self.open) self.open(a.path);
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

UIViewController* AndropkgLauncherController(void (^open)(NSString* apkPath)) {
  AndropkgLauncher* l = [[AndropkgLauncher alloc] initWithStyle:UITableViewStyleInsetGrouped];
  l.open = open;
  return [[UINavigationController alloc] initWithRootViewController:l];
}
