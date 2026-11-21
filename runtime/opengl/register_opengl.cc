// andropkg: libandropkg_gl's JNI registration (android.opengl.*, com.google.android.gles_jni.*),
// called by libandroid_runtime's startup registration (android_runtime_onload.cc) through dlsym:
// the GL JNI lives in its own library, next to EGL (egl.m), so that libandroid_runtime doesn't
// link the platform's OpenGL (ES).
#include <jni.h>
#include <log/log.h>

int register_android_opengl_jni_EGL14(JNIEnv* env);
int register_android_opengl_jni_EGLExt(JNIEnv* env);
int register_android_opengl_jni_GLES20(JNIEnv* env);
int register_android_opengl_jni_GLES30(JNIEnv* env);
int register_com_google_android_gles_jni_EGLImpl(JNIEnv* env);
int register_com_google_android_gles_jni_GLImpl(JNIEnv* env);

extern "C" __attribute__((visibility("default"))) int andropkg_register_opengl(JNIEnv* env) {
  struct {
    const char* name;
    int (*fn)(JNIEnv*);
  } regs[] = {
      {"EGL14", register_android_opengl_jni_EGL14},   {"EGLExt", register_android_opengl_jni_EGLExt},
      {"GLES20", register_android_opengl_jni_GLES20}, {"GLES30", register_android_opengl_jni_GLES30},
      {"EGLImpl", register_com_google_android_gles_jni_EGLImpl},
      {"GLImpl", register_com_google_android_gles_jni_GLImpl},
  };
  for (auto& r : regs) {
    if (r.fn(env) < 0) {
      ALOGE("andropkg: registering %s natives failed", r.name);
      return -1;
    }
  }
  return 0;
}
