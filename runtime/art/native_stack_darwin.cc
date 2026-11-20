// andropkg: native stack collection/dumping with Darwin's <execinfo.h> instead of
// libunwindstack (Linux/ELF only). Replaces art/runtime/{backtrace_helper,native_stack_dump}.cc.
// Only the *current* thread can be unwound this way; dumping other threads prints a note.
#include "backtrace_helper.h"
#include "native_stack_dump.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>

#include <cstdlib>
#include <ostream>

#include "unwindstack/AndroidUnwinder.h"

namespace art {

void BacktraceCollector::Collect() {
  void* frames[128];
  int n = backtrace(frames, static_cast<int>(std::min<size_t>(128, max_depth_ + skip_count_ + 1)));
  num_frames_ = 0;
  for (int i = 1 + static_cast<int>(skip_count_); i < n && num_frames_ < max_depth_; i++) {
    out_frames_[num_frames_++] = reinterpret_cast<uintptr_t>(frames[i]);
  }
}

static bool IsCurrentThread(pid_t tid) {
  uint64_t self = 0;
  pthread_threadid_np(nullptr, &self);
  return tid == 0 || static_cast<uint64_t>(tid) == (self & 0x7fffffff) || tid == getpid();
}

void DumpNativeStack(std::ostream& os, unwindstack::AndroidLocalUnwinder&, pid_t tid,
                     const char* prefix, ArtMethod*, void*, bool skip_frames) {
  if (!IsCurrentThread(tid)) {
    os << prefix << "(native stack of other threads unavailable on Darwin)\n";
    return;
  }
  void* frames[128];
  int n = backtrace(frames, 128);
  for (int i = skip_frames ? 2 : 0; i < n; i++) {
    Dl_info info{};
    os << prefix << "native: #" << i << " pc " << frames[i];
    if (dladdr(frames[i], &info) != 0) {
      const char* name = info.dli_sname;
      int status = 0;
      char* demangled = name ? abi::__cxa_demangle(name, nullptr, nullptr, &status) : nullptr;
      os << "  " << (info.dli_fname ? info.dli_fname : "?") << " ("
         << (demangled ? demangled : (name ? name : "?")) << "+"
         << (reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(info.dli_saddr))
         << ")";
      free(demangled);
    }
    os << "\n";
  }
}

void DumpNativeStack(std::ostream& os, pid_t tid, const char* prefix, ArtMethod* current_method,
                     void* ucontext, bool skip_frames) {
  unwindstack::AndroidLocalUnwinder unwinder;
  DumpNativeStack(os, unwinder, tid, prefix, current_method, ucontext, skip_frames);
}

}  // namespace art
