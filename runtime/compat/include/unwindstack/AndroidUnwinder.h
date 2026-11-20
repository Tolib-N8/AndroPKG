// andropkg: libunwindstack is Linux/ELF-specific. ART only passes an AndroidLocalUnwinder
// through to DumpNativeStack/BacktraceCollector, which andropkg implements with Darwin's
// backtrace() (runtime/art/native_stack_dump_darwin.cc).
#pragma once
namespace unwindstack {
class Unwinder;
class AndroidUnwinder {
 public:
  virtual ~AndroidUnwinder() = default;
};
class AndroidLocalUnwinder : public AndroidUnwinder {
 public:
  AndroidLocalUnwinder() = default;
  template <typename... Args>
  explicit AndroidLocalUnwinder(Args&&...) {}
};
}  // namespace unwindstack
