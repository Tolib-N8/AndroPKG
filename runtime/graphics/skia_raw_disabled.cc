// andropkg: Skia's RAW codec (dng_sdk/piex) isn't built; RAW streams are "unimplemented".
#include "src/codec/SkRawCodec.h"

std::unique_ptr<SkCodec> SkRawCodec::MakeFromStream(std::unique_ptr<SkStream>, Result* result) {
  if (result) *result = kUnimplemented;
  return nullptr;
}
