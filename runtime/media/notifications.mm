// andropkg runtime — Android notifications as iOS (or macOS) notifications.
//
// services' NotificationManagerImpl posts an app's notifications here (title, text, a key);
// they become local notifications of the host (UNUserNotificationCenter), with permission asked
// the first time. Tapping one opens the host, which tells the runtime
// (andropkg_notification_opened), and the app's contentIntent is sent from Java.
// Media and ongoing notifications are not posted: the lock screen's Now Playing stands for a
// player's notification (audio_track.mm).
#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include <jni.h>

#include <mutex>
#include <string>
#include <vector>

namespace {

JavaVM* gVm;
jclass gNotifications;   // dev.andropkg.system.NotificationManagerImpl (global)
jmethodID gOpened;       // static void hostOpened(String key)
std::mutex gLock;
std::vector<std::string> gPendingOpens;  // taps before the runtime was bound

std::string Utf(JNIEnv* env, jstring s) {
  if (s == nullptr) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string r = c ? c : "";
  if (c) env->ReleaseStringUTFChars(s, c);
  return r;
}

// UNUserNotificationCenter needs an app bundle (a plain process, as the Mac's dalvikvm, would
// throw): there, notifications are only logged.
bool HostHasNotifications() {
  static const bool ok = NSBundle.mainBundle.bundleIdentifier != nil;
  return ok;
}

// Runs `post` once notifications are allowed: the first one asks (and waits for the answer: a
// notification added before it fails, UNErrorDomain 2003).
void WhenAuthorized(void (^post)(void)) {
  UNUserNotificationCenter* center = UNUserNotificationCenter.currentNotificationCenter;
  [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound |
                                           UNAuthorizationOptionBadge)
                        completionHandler:^(BOOL granted, NSError* error) {
                          if (granted) {
                            post();
                          } else {
                            NSLog(@"andropkg: notifications not allowed (%@)", error);
                          }
                        }];
}

void n_post(JNIEnv* env, jclass, jstring jkey, jstring jtitle, jstring jtext, jboolean silent) {
  std::string key = Utf(env, jkey), title = Utf(env, jtitle), text = Utf(env, jtext);
  if (!HostHasNotifications()) {
    NSLog(@"andropkg: notification %s: %s — %s", key.c_str(), title.c_str(), text.c_str());
    return;
  }
  UNMutableNotificationContent* content = [UNMutableNotificationContent new];
  content.title = @(title.c_str());
  content.body = @(text.c_str());
  if (!silent) content.sound = UNNotificationSound.defaultSound;
  content.userInfo = @{@"andropkg.key" : @(key.c_str())};
  NSString* ident = [@"andropkg." stringByAppendingString:@(key.c_str())];
  UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:ident content:content trigger:nil];
  WhenAuthorized(^{
    [UNUserNotificationCenter.currentNotificationCenter addNotificationRequest:request
                                                         withCompletionHandler:^(NSError* error) {
                                                           if (error) NSLog(@"andropkg: notification: %@", error);
                                                         }];
  });
}

void n_cancel(JNIEnv* env, jclass, jstring jkey) {
  if (!HostHasNotifications()) return;
  NSString* ident = [@"andropkg." stringByAppendingString:@(Utf(env, jkey).c_str())];
  UNUserNotificationCenter* c = UNUserNotificationCenter.currentNotificationCenter;
  [c removeDeliveredNotificationsWithIdentifiers:@[ ident ]];
  [c removePendingNotificationRequestsWithIdentifiers:@[ ident ]];
}

void DeliverOpen(const std::string& key) {
  JNIEnv* env = nullptr;
  if (gVm == nullptr || gVm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
  jstring k = env->NewStringUTF(key.c_str());
  env->CallStaticVoidMethod(gNotifications, gOpened, k);
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(k);
}

}  // namespace

// Called once the services' classes are loadable (andropkg_media_bind_services).
void andropkg_bind_notifications(JNIEnv* env, jclass c) {
  static const JNINativeMethod kMethods[] = {
      {const_cast<char*>("nativePost"), const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)V"),
       reinterpret_cast<void*>(n_post)},
      {const_cast<char*>("nativeCancel"), const_cast<char*>("(Ljava/lang/String;)V"), reinterpret_cast<void*>(n_cancel)},
  };
  if (env->RegisterNatives(c, kMethods, 2) != JNI_OK) {
    env->ExceptionClear();
    return;
  }
  env->GetJavaVM(&gVm);
  std::vector<std::string> pending;
  {
    std::lock_guard<std::mutex> g(gLock);
    gNotifications = static_cast<jclass>(env->NewGlobalRef(c));
    gOpened = env->GetStaticMethodID(c, "hostOpened", "(Ljava/lang/String;)V");
    pending.swap(gPendingOpens);
  }
  for (const std::string& key : pending) DeliverOpen(key);
}

// The host: the user tapped a notification posted above.
extern "C" __attribute__((visibility("default"))) void andropkg_notification_opened(const char* key) {
  std::string k = key ? key : "";
  {
    std::lock_guard<std::mutex> g(gLock);
    if (gNotifications == nullptr) {
      gPendingOpens.push_back(k);
      return;
    }
  }
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ DeliverOpen(k); });
}
