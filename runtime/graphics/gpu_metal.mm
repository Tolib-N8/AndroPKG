// andropkg: Ganesh on Metal (see gpu_metal.h).
#include "gpu_metal.h"

#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/CAMetalLayer.h>

#include <stdlib.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/gpu/mtl/GrMtlTypes.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include "include/gpu/GrBackendSurface.h"
#include "include/gpu/GrDirectContext.h"
#include "include/gpu/mtl/GrMtlBackendContext.h"

namespace andropkg_gpu {
namespace {

struct State {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  sk_sp<GrDirectContext> context;
  CAMetalLayer* layer = nil;  // the host's, if any
  // The frame being composed.
  sk_sp<SkSurface> frame;
  // Skia acquires the layer's next drawable lazily, when the frame is first flushed, and
  // stores it (retained) here: the slot must outlive the surface's instantiation.
  GrMTLHandle drawable = nullptr;
  sk_sp<SkSurface> offscreen;  // reused when there is no layer
  CVMetalTextureCacheRef textures = nullptr;  // video frames as Metal textures
};

State& S() {
  static State* s = [] {
    auto* st = new State;
    if (getenv("ANDROPKG_NO_GPU") != nullptr) return st;  // CPU rendering (raster Skia)
    st->device = MTLCreateSystemDefaultDevice();
    if (st->device == nil) return st;
    st->queue = [st->device newCommandQueue];
    GrMtlBackendContext backend = {};
    backend.fDevice.retain((__bridge GrMTLHandle)st->device);
    backend.fQueue.retain((__bridge GrMTLHandle)st->queue);
    GrContextOptions options;
    st->context = GrDirectContext::MakeMetal(backend, options);
    return st;
  }();
  return *s;
}

}  // namespace

bool Enabled() { return S().context != nullptr; }

std::recursive_mutex& Mutex() {
  static std::recursive_mutex m;
  return m;
}

GrDirectContext* Context() { return S().context.get(); }

sk_sp<SkSurface> MakeSurface(int width, int height) {
  if (!Enabled() || width <= 0 || height <= 0) return nullptr;
  SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType,
                                       SkColorSpace::MakeSRGB());
  return SkSurface::MakeRenderTarget(S().context.get(), skgpu::Budgeted::kNo, info, 0,
                                     kTopLeft_GrSurfaceOrigin, nullptr);
}

bool PresentsToLayer() { return S().layer != nil; }

SkCanvas* BeginFrame(int width, int height, bool readback) {
  State& s = S();
  if (!s.context) return nullptr;
  s.frame.reset();
  // A frame to keep is composed offscreen: drawables can't be read back (iOS); EndFrame then
  // copies it to the layer.
  if (s.layer != nil && !readback) {
    CGSize size = CGSizeMake(width, height);
    if (!CGSizeEqualToSize(s.layer.drawableSize, size)) s.layer.drawableSize = size;
    s.frame = SkSurface::MakeFromCAMetalLayer(s.context.get(), (__bridge GrMTLHandle)s.layer,
                                              kTopLeft_GrSurfaceOrigin, 1, kBGRA_8888_SkColorType,
                                              SkColorSpace::MakeSRGB(), nullptr, &s.drawable);
  } else {
    if (!s.offscreen || s.offscreen->width() != width || s.offscreen->height() != height) {
      s.offscreen = MakeSurface(width, height);
    }
    s.frame = s.offscreen;
  }
  return s.frame ? s.frame->getCanvas() : nullptr;
}

void EndFrame(uint32_t* readback) {
  State& s = S();
  if (!s.frame) return;
  if (readback != nullptr) {
    SkImageInfo info = SkImageInfo::Make(s.frame->width(), s.frame->height(), kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    s.frame->readPixels(info, readback, static_cast<size_t>(s.frame->width()) * 4, 0, 0);
    if (s.layer != nil && s.frame == s.offscreen) {  // now show it
      sk_sp<SkImage> image = s.offscreen->makeImageSnapshot();
      s.frame = SkSurface::MakeFromCAMetalLayer(s.context.get(), (__bridge GrMTLHandle)s.layer,
                                                kTopLeft_GrSurfaceOrigin, 1, kBGRA_8888_SkColorType,
                                                SkColorSpace::MakeSRGB(), nullptr, &s.drawable);
      if (s.frame) s.frame->getCanvas()->drawImage(image, 0, 0);
    }
  }
  if (!s.frame) return;
  s.frame->flushAndSubmit();
  if (s.drawable != nullptr) {
    id<CAMetalDrawable> drawable = (__bridge_transfer id<CAMetalDrawable>)s.drawable;  // balances Skia's retain
    s.drawable = nullptr;
    id<MTLCommandBuffer> cb = [s.queue commandBuffer];
    [cb presentDrawable:drawable];
    [cb commit];
  }
  if (s.frame != s.offscreen) s.frame.reset();
  // Keep the GPU's memory in check: textures of bitmaps no longer drawn are purged, and the
  // video frame texture cache lets go of frames no longer referenced (it never does unless
  // flushed).
  s.context->performDeferredCleanup(std::chrono::seconds(5));
  if (s.textures) CVMetalTextureCacheFlush(s.textures, 0);
  static const bool logMemory = getenv("ANDROPKG_LOG_MEMORY") != nullptr;
  static int frames = 0;
  if (logMemory && ++frames % 300 == 0) {
    int count = 0;
    size_t bytes = 0;
    s.context->getResourceCacheUsage(&count, &bytes);
    fprintf(stderr, "andropkg-gpu: skia cache %d resources, %zu MB (%zu MB purgeable); device %llu MB allocated\n",
            count, bytes >> 20, s.context->getResourceCachePurgeableBytes() >> 20,
            (unsigned long long)s.device.currentAllocatedSize >> 20);
  }
}

sk_sp<SkImage> ImageFromPixelBuffer(void* pixel_buffer) {
  std::lock_guard<std::recursive_mutex> g(Mutex());
  State& s = S();
  auto pb = static_cast<CVPixelBufferRef>(pixel_buffer);
  if (!s.context || pb == nullptr || CVPixelBufferGetPixelFormatType(pb) != kCVPixelFormatType_32BGRA) return nullptr;
  if (s.textures == nullptr && CVMetalTextureCacheCreate(nullptr, nullptr, s.device, nullptr, &s.textures) != kCVReturnSuccess) {
    return nullptr;
  }
  int w = (int)CVPixelBufferGetWidth(pb), h = (int)CVPixelBufferGetHeight(pb);
  CVMetalTextureRef tex = nullptr;
  if (CVMetalTextureCacheCreateTextureFromImage(nullptr, s.textures, pb, nullptr, MTLPixelFormatBGRA8Unorm, w, h, 0,
                                                &tex) != kCVReturnSuccess) {
    return nullptr;
  }
  GrMtlTextureInfo info;
  info.fTexture.retain((__bridge GrMTLHandle)CVMetalTextureGetTexture(tex));
  GrBackendTexture backend(w, h, GrMipmapped::kNo, info);
  // The CVMetalTexture (and the pixel buffer it keeps) lives until Skia is done with the image.
  return SkImage::MakeFromTexture(s.context.get(), backend, kTopLeft_GrSurfaceOrigin, kBGRA_8888_SkColorType,
                                  kOpaque_SkAlphaType, nullptr,
                                  [](void* t) { CFRelease(static_cast<CVMetalTextureRef>(t)); }, tex);
}

}  // namespace andropkg_gpu

extern "C" __attribute__((visibility("default"))) void andropkg_set_metal_layer(void* layer) {
  std::lock_guard<std::recursive_mutex> g(andropkg_gpu::Mutex());
  andropkg_gpu::State& s = andropkg_gpu::S();
  if (!s.context) return;
  s.layer = (__bridge CAMetalLayer*)layer;
  if (s.layer != nil) {
    s.layer.device = s.device;
    s.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    s.layer.framebufferOnly = NO;  // Skia may read the drawable (blend modes, readback)
  }
}
