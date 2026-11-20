// andropkg: cpu_features::GetAarch64Info() backed by Darwin sysctl hw.optional.*.
#pragma once
#include <sys/sysctl.h>

namespace cpu_features {
struct Aarch64Features {
  int fp = 1, asimd = 1, crc32 = 0, atomics = 0, fphp = 0, asimdhp = 0, asimddp = 0, sve = 0;
};
struct Aarch64Info {
  Aarch64Features features;
};
namespace andropkg_detail {
inline int sysctl_flag(const char* name) {
  int v = 0;
  size_t n = sizeof v;
  return sysctlbyname(name, &v, &n, nullptr, 0) == 0 && v != 0;
}
}  // namespace andropkg_detail
inline Aarch64Info GetAarch64Info() {
  using andropkg_detail::sysctl_flag;
  Aarch64Info info;
  info.features.crc32 = sysctl_flag("hw.optional.armv8_crc32");
  info.features.atomics = sysctl_flag("hw.optional.arm.FEAT_LSE");
  info.features.fphp = info.features.asimdhp = sysctl_flag("hw.optional.arm.FEAT_FP16");
  info.features.asimddp = sysctl_flag("hw.optional.arm.FEAT_DotProd");
  return info;  // no SVE on Apple silicon
}
}  // namespace cpu_features
