// andropkg: xz/LZMA is only used for mini-debug-info of compiled (JIT/AOT) code, which the
// interpreter-only runtime never produces or consumes. Replaces art/libelffile/elf/xz_utils.cc.
#include "elf/xz_utils.h"

#include <android-base/logging.h>

namespace art {

void XzCompress(ArrayRef<const uint8_t>, std::vector<uint8_t>*, int, size_t) {
  LOG(FATAL) << "andropkg: XzCompress is not available";
}

void XzDecompress(ArrayRef<const uint8_t>, std::vector<uint8_t>*) {
  LOG(FATAL) << "andropkg: XzDecompress is not available";
}

}  // namespace art
