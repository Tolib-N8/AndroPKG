// andropkg: Android filesystem layout on iOS. Framework code uses absolute device paths
// (/system/framework/framework-res.apk, /apex/com.android.i18n/...); they live under the
// runtime's root directory ($ANDROPKG_ANDROID_ROOT) instead.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
/// Returns `path` itself, or a translated copy written into `buf` when `path` is under an
/// Android system partition (/system, /apex, /product, /system_ext, /vendor, /odm) or /data.
const char* andropkg_translate_path(const char* path, char* buf, size_t len);
#ifdef __cplusplus
}

#include <string>
namespace andropkg {
inline std::string TranslateAndroidPath(const char* path) {
  char buf[4096];
  return andropkg_translate_path(path, buf, sizeof buf);
}
}  // namespace andropkg
#endif
