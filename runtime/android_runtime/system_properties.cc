// andropkg: system properties. On Android, init loads /system/build.prop & friends into the
// property area before zygote starts. Here libbase keeps properties in-process (its host
// implementation) and we seed them from Android 14's build.prop plus device-specific values.
#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <sys/sysctl.h>

#include <cstdlib>
#include <string>

#include "andropkg/paths.h"

namespace andropkg {

static std::string MachineModel() {
  char buf[64] = {};
  size_t len = sizeof buf;
  if (sysctlbyname("hw.machine", buf, &len, nullptr, 0) != 0) return "unknown";
  return buf;  // e.g. "iPhone17,1" (or "arm64" on a Mac)
}

void LoadSystemProperties() {
  std::string content;
  if (android::base::ReadFileToString(TranslateAndroidPath("/system/build.prop"), &content)) {
    for (const std::string& raw : android::base::Split(content, "\n")) {
      std::string line = android::base::Trim(raw);
      if (line.empty() || line[0] == '#') continue;
      size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      android::base::SetProperty(line.substr(0, eq), line.substr(eq + 1));
    }
  }
  // What the framework expects init/vendor to provide, describing this device.
  const std::string model = MachineModel();
  const std::pair<const char*, std::string> kDevice[] = {
      {"ro.product.cpu.abi", "arm64-v8a"},
      {"ro.product.cpu.abilist", "arm64-v8a"},
      {"ro.product.cpu.abilist64", "arm64-v8a"},
      {"ro.product.cpu.abilist32", ""},
      {"ro.product.manufacturer", "Apple"},
      {"ro.product.brand", "Apple"},
      {"ro.product.model", model},
      {"ro.product.device", "andropkg"},
      {"ro.product.name", "andropkg"},
      {"ro.product.board", "andropkg"},
      {"ro.hardware", "andropkg"},
      {"ro.build.type", "user"},
      {"ro.build.tags", "release-keys"},
      {"ro.debuggable", "0"},
      {"ro.secure", "1"},
      {"ro.zygote", ""},
  };
  for (const auto& [key, value] : kDevice) android::base::SetProperty(key, value);

  // Release (non-preview) Android 14 values the full /system/build.prop would carry;
  // only filled in when missing.
  const std::pair<const char*, const char*> kDefaults[] = {
      {"ro.build.version.sdk", "34"},
      {"ro.build.version.release", "14"},
      {"ro.build.version.release_or_codename", "14"},
      {"ro.build.version.codename", "REL"},
      {"ro.build.version.all_codenames", "REL"},
      {"ro.build.version.known_codenames",
       "Base,Base11,Cupcake,Donut,Eclair,Eclair01,EclairMr1,Froyo,Gingerbread,GingerbreadMr1,"
       "Honeycomb,HoneycombMr1,HoneycombMr2,IceCreamSandwich,IceCreamSandwichMr1,JellyBean,"
       "JellyBeanMr1,JellyBeanMr2,Kitkat,KitkatWatch,Lollipop,LollipopMr1,M,N,NMr1,O,OMr1,P,"
       "Q,R,S,Sv2,Tiramisu,UpsideDownCake"},
      {"ro.build.version.preview_sdk", "0"},
      {"ro.build.version.preview_sdk_fingerprint", "REL"},
      {"ro.build.version.min_supported_target_sdk", "23"},
      {"ro.build.version.base_os", ""},
      {"ro.product.first_api_level", "34"},
      {"ro.bootloader", "unknown"},
      {"ro.boot.qemu", "0"},
  };
  for (const auto& [key, value] : kDefaults) {
    if (android::base::GetProperty(key, "").empty()) android::base::SetProperty(key, value);
  }
}

}  // namespace andropkg
