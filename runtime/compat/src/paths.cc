#include "andropkg/paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// True if `path` is `prefix` or below it ("/storage" matches "/storage" and "/storage/x").
static bool Under(const char* path, const char* prefix, size_t n) {
  return strncmp(path, prefix, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

extern "C" const char* andropkg_translate_path(const char* path, char* buf, size_t len) {
  // "/data" too: framework code stats the partition itself (SQLiteGlobal, StorageManager).
  static const char* const kPartitions[] = {"/system", "/apex", "/product", "/system_ext",
                                            "/vendor", "/odm", "/data", "/storage"};
  const char* root = getenv("ANDROPKG_ANDROID_ROOT");
  if (path == nullptr || root == nullptr || *root == '\0' || path[0] != '/') return path;

  // A path into an earlier data container of this iOS app (/var/mobile/Containers/Data/
  // Application/<UUID>/...): iOS moves the container when the app is reinstalled, and apps keep
  // absolute paths they once got (settings locations, caches). Redirect to the current one ($HOME).
  for (const char* c : {"/private/var/mobile/Containers/Data/Application/",
                        "/var/mobile/Containers/Data/Application/"}) {
    size_t cn = strlen(c);
    const char* home = getenv("HOME");
    if (home == nullptr || strncmp(path, c, cn) != 0 || strlen(path) < cn + 36) continue;
    const char* rest = path + cn + 36;  // after the UUID
    if (*rest != '/' && *rest != '\0') continue;
    const char* hc = strstr(home, "/Containers/Data/Application/");
    if (hc == nullptr) continue;
    hc += strlen("/Containers/Data/Application/");
    if (strncmp(hc, path + cn, 36) == 0) return path;  // the current container
    int n = snprintf(buf, len, "%s%s", home, rest);
    return (n > 0 && static_cast<size_t>(n) < len) ? buf : path;
  }
  if (strncmp(path, root, strlen(root)) == 0) return path;  // already a host path

  // The app's private data: /data/user/0/<pkg> (alias /data/data/<pkg>) and the device-protected
  // /data/user_de/0/<pkg> are the host directory ANDROPKG_APP_DATA_HOST (+ "_de"). Apps keep these
  // paths (settings, caches), and the iOS container path changes when the app is reinstalled.
  const char* pkg = getenv("ANDROPKG_APP_DATA_PKG");
  const char* host = getenv("ANDROPKG_APP_DATA_HOST");
  if (pkg != nullptr && host != nullptr && *pkg != '\0') {
    static const char* const kData[] = {"/data/user/0/", "/data/data/", "/data/user_de/0/"};
    size_t pn = strlen(pkg);
    for (const char* d : kData) {
      size_t dn = strlen(d);
      if (strncmp(path, d, dn) == 0 && Under(path + dn, pkg, pn)) {
        const char* de = d[6] == '_' ? "_de" : "";
        int n = snprintf(buf, len, "%s%s%s", host, de, path + dn + pn);
        return (n > 0 && static_cast<size_t>(n) < len) ? buf : path;
      }
    }
  }

  // Shared storage: /storage/emulated/0 (and its /sdcard alias) is a host directory of its
  // own (ANDROPKG_STORAGE_DIR, e.g. the iOS app's Documents), else <root>/storage/emulated/0.
  static const char kPrimary[] = "/storage/emulated/0";
  const char* rest = nullptr;
  if (Under(path, kPrimary, sizeof(kPrimary) - 1)) rest = path + sizeof(kPrimary) - 1;
  else if (Under(path, "/sdcard", 7)) rest = path + 7;
  if (rest != nullptr) {
    const char* dir = getenv("ANDROPKG_STORAGE_DIR");
    int n = (dir != nullptr && *dir != '\0') ? snprintf(buf, len, "%s%s", dir, rest)
                                             : snprintf(buf, len, "%s%s%s", root, kPrimary, rest);
    return (n > 0 && static_cast<size_t>(n) < len) ? buf : path;
  }

  for (const char* p : kPartitions) {
    if (Under(path, p, strlen(p))) {
      int n = snprintf(buf, len, "%s%s", root, path);
      return (n > 0 && static_cast<size_t>(n) < len) ? buf : path;
    }
  }
  return path;
}
