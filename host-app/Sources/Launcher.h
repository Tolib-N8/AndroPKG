// andropkg host — the launcher (Launcher.mm).
#pragma once
#import <UIKit/UIKit.h>

// The list of Android apps and APK import; `open` gets the chosen APK (a path).
UIViewController* AndropkgLauncherController(void (^open)(NSString* apkPath));
// Copies an APK (a file the app was asked to open, or one picked) into the installed apps.
BOOL AndropkgImportApk(NSURL* url, NSString** error);
// Where the runtime writes an app's name and icon the first time it runs (created).
NSString* AndropkgMetaDir(NSString* name);
// Posted after an import: the launcher reloads.
extern NSString* const AndropkgAppsChanged;
