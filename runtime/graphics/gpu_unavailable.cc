// andropkg (M2d interim): GPU entry points hwui references. Rendering is CPU (Skia raster)
// until the Metal path lands, so report "no GPU" exactly as a GPU-less device would.
#include <stddef.h>
#include <stdint.h>

extern "C" {
// EGL: no display.
void* eglGetDisplay(void*) { return nullptr; }
// Vulkan: initialization fails (VK_ERROR_INITIALIZATION_FAILED == -3).
int32_t vkEnumerateInstanceVersion(uint32_t* version) {
  if (version) *version = 0;
  return -3;
}
}
