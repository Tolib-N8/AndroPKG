// andropkg: nterp (ART's hand-written arm64 assembly interpreter) is disabled until its
// assembly is ported to Mach-O. The C++ switch interpreter runs all bytecode.
// Replaces art/runtime/interpreter/mterp/{nterp.cc,nterp_impl.cc} and :libart_mterp.arm64ng.
#include "interpreter/mterp/nterp.h"

#include <android-base/logging.h>

extern "C" void* artNterpAsmInstructionStart[] = {nullptr};
extern "C" void* artNterpAsmInstructionEnd[] = {nullptr};

namespace art {
namespace interpreter {

extern "C" void ExecuteNterpImpl() { LOG(FATAL) << "andropkg: nterp is not available"; }
extern "C" void ExecuteNterpWithClinitImpl() { LOG(FATAL) << "andropkg: nterp is not available"; }

bool IsNterpSupported() { return false; }
bool CanRuntimeUseNterp() { return false; }
const void* GetNterpEntryPoint() { return reinterpret_cast<const void*>(ExecuteNterpImpl); }
const void* GetNterpWithClinitEntryPoint() {
  return reinterpret_cast<const void*>(ExecuteNterpWithClinitImpl);
}
void CheckNterpAsmConstants() {}

}  // namespace interpreter
}  // namespace art
