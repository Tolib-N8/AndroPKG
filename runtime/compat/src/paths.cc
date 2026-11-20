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
  if (strncmp(path, root, strlen(root)) == 0) return path;  // already a host path

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
