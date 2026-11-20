#include "andropkg/paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" const char* andropkg_translate_path(const char* path, char* buf, size_t len) {
  // "/data" too: framework code stats the partition itself (SQLiteGlobal, StorageManager).
  static const char* const kPartitions[] = {"/system/", "/apex/", "/product/", "/system_ext/",
                                            "/vendor/", "/odm/", "/data/"};
  const char* root = getenv("ANDROPKG_ANDROID_ROOT");
  if (path == nullptr || root == nullptr || *root == '\0') return path;
  for (const char* p : kPartitions) {
    size_t n = strlen(p);
    bool partition = strncmp(path, p, n - 1) == 0 && (path[n - 1] == '/' || path[n - 1] == '\0');
    if (partition && strncmp(path, root, strlen(root)) != 0) {
      int n = snprintf(buf, len, "%s%s", root, path);
      return (n > 0 && static_cast<size_t>(n) < len) ? buf : path;
    }
  }
  return path;
}
