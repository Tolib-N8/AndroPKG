// andropkg (M2d interim): framework graphics JNI tables that need the GPU renderer
// (ThreadedRenderer, TextureLayer, HardwareBufferRenderer), pdfium (PdfRenderer/PdfEditor),
// or aren't built yet. Registering nothing makes their natives fail lazily with
// UnsatisfiedLinkError instead of blocking startup.
#include <jni.h>

int register_android_graphics_Movie(JNIEnv*) { return 0; }
int register_android_graphics_drawable_AnimatedImageDrawable(JNIEnv*) { return 0; }
int register_android_graphics_BitmapRegionDecoder(JNIEnv*) { return 0; }

namespace android {
// HardwareRenderer: process-wide configuration natives succeed without effect, so app startup
// proceeds; the window draws through the software path (Surface.lockCanvas) until the GPU
// renderer is ported. Per-renderer natives stay unregistered.
static void hr_V(JNIEnv*, jclass) {}
static void hr_Z(JNIEnv*, jclass, jboolean) {}
static void hr_I(JNIEnv*, jclass, jint) {}
static void hr_SS(JNIEnv*, jclass, jstring, jstring) {}
static void hr_FdI(JNIEnv*, jclass, jobject, jint) {}
static void hr_initDisplayInfo(JNIEnv*, jclass, jint, jint, jfloat, jint, jlong, jlong, jboolean,
                               jboolean) {}
static jboolean hr_false(JNIEnv*, jclass) { return JNI_FALSE; }
static jboolean hr_isDrawingEnabled(JNIEnv*, jclass) { return JNI_TRUE; }

static const JNINativeMethod kHardwareRendererMethods[] = {
    {"nRotateProcessStatsBuffer", "()V", reinterpret_cast<void*>(hr_V)},
    {"nSetProcessStatsBuffer", "(I)V", reinterpret_cast<void*>(hr_I)},
    {"nSetIsHighEndGfx", "(Z)V", reinterpret_cast<void*>(hr_Z)},
    {"nSetIsLowRam", "(Z)V", reinterpret_cast<void*>(hr_Z)},
    {"nSetIsSystemOrPersistent", "(Z)V", reinterpret_cast<void*>(hr_Z)},
    {"nTrimMemory", "(I)V", reinterpret_cast<void*>(hr_I)},
    {"nTrimCaches", "(I)V", reinterpret_cast<void*>(hr_I)},
    {"nOverrideProperty", "(Ljava/lang/String;Ljava/lang/String;)V", reinterpret_cast<void*>(hr_SS)},
    {"nDumpGlobalProfileInfo", "(Ljava/io/FileDescriptor;I)V", reinterpret_cast<void*>(hr_FdI)},
    {"nSetHighContrastText", "(Z)V", reinterpret_cast<void*>(hr_Z)},
    {"nSetDebuggingEnabled", "(Z)V", reinterpret_cast<void*>(hr_Z)},
    {"nSetIsolatedProcess", "(Z)V", reinterpret_cast<void*>(hr_Z)},
    {"nSetContextPriority", "(I)V", reinterpret_cast<void*>(hr_I)},
    {"nSetDisplayDensityDpi", "(I)V", reinterpret_cast<void*>(hr_I)},
    {"nInitDisplayInfo", "(IIFIJJZZ)V", reinterpret_cast<void*>(hr_initDisplayInfo)},
    {"nSetDrawingEnabled", "(Z)V", reinterpret_cast<void*>(hr_Z)},
    {"nIsDrawingEnabled", "()Z", reinterpret_cast<void*>(hr_isDrawingEnabled)},
    {"nSetRtAnimationsEnabled", "(Z)V", reinterpret_cast<void*>(hr_Z)},
    {"setupShadersDiskCache", "(Ljava/lang/String;Ljava/lang/String;)V", reinterpret_cast<void*>(hr_SS)},
    {"disableVsync", "()V", reinterpret_cast<void*>(hr_V)},
    {"preload", "()V", reinterpret_cast<void*>(hr_V)},
    {"isWebViewOverlaysEnabled", "()Z", reinterpret_cast<void*>(hr_false)},
};

int register_android_view_ThreadedRenderer(JNIEnv* env) {
  jclass c = env->FindClass("android/graphics/HardwareRenderer");
  if (c == nullptr) {
    env->ExceptionClear();
    return 0;
  }
  for (const auto& m : kHardwareRendererMethods) {
    if (env->GetStaticMethodID(c, m.name, m.signature) == nullptr) {
      env->ExceptionClear();
      continue;
    }
    if (env->RegisterNatives(c, &m, 1) != JNI_OK) env->ExceptionClear();
  }
  env->DeleteLocalRef(c);
  return 0;
}
int register_android_graphics_TextureLayer(JNIEnv*) { return 0; }
int register_android_graphics_pdf_PdfRenderer(JNIEnv*) { return 0; }
int register_android_graphics_pdf_PdfEditor(JNIEnv*) { return 0; }
int register_android_graphics_pdf_PdfDocument(JNIEnv*) { return 0; }
int register_android_graphics_HardwareBufferRenderer(JNIEnv*) { return 0; }
}  // namespace android
