// andropkg: GPU rendering through Skia's Ganesh on Metal (iOS and macOS).
//
// Windows draw through Surface.lockCanvas as before, but (unless ANDROPKG_NO_GPU) the Java Canvas is
// bound to a GPU SkSurface per window, so every Canvas call (text, bitmaps, paths, RenderNode
// layers, blurs) rasterizes on the GPU. Windows are then composed on the GPU into the host's
// CAMetalLayer, or into memory when there is none (tests, the Mac runner) or a frame must be
// kept (screenshots).
//
// One GrDirectContext serves the process; it is not thread-safe, so every use holds Mutex().
#pragma once

#include <stdint.h>

#include <mutex>

#include "include/core/SkRefCnt.h"

class GrDirectContext;
class SkCanvas;
class SkImage;
class SkSurface;

namespace andropkg_gpu {

/// A Metal device exists and ANDROPKG_NO_GPU is not set.
bool Enabled();

/// Held for every use of the context and of GPU surfaces (recursive: lockCanvas holds it until
/// unlockCanvasAndPost, which composes).
std::recursive_mutex& Mutex();

GrDirectContext* Context();

/// An RGBA8888 premultiplied render target, top-left origin.
sk_sp<SkSurface> MakeSurface(int width, int height);

/// Starts composing a display frame of width x height: the canvas draws into the host's
/// CAMetalLayer drawable, or an offscreen target (no layer, or `readback`: EndFrame will be
/// given a buffer).
SkCanvas* BeginFrame(int width, int height, bool readback);

/// Submits the frame and presents it. With `readback` (width * height RGBA8888 pixels) the
/// frame is also copied there.
void EndFrame(uint32_t* readback);

/// Whether frames reach the screen without a readback (the host gave us a layer).
bool PresentsToLayer();

/// A GPU image of a decoded video frame (a 32BGRA, Metal-compatible CVPixelBufferRef), without
/// copying it; nullptr if it can't be wrapped.
sk_sp<SkImage> ImageFromPixelBuffer(void* pixel_buffer);

}  // namespace andropkg_gpu

/// Host: the CAMetalLayer (CAMetalLayer*) frames are presented to.
extern "C" void andropkg_set_metal_layer(void* layer);
