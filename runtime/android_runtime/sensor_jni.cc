// andropkg: android.hardware.SystemSensorManager natives without sensorservice.
//
// The host (iOS: CoreMotion) declares which sensor types it can feed and is told when apps
// start or stop listening; it pushes samples with andropkg_sensor_push() from any thread.
// Each SensorEventQueue gets them on its own Looper, like the real Receiver does.
// Without a host, the device has an accelerometer that only test scripts move.
#include <android_runtime/AndroidRuntime.h>
#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <utils/Looper.h>
#include <utils/SystemClock.h>

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "android_os_MessageQueue.h"

extern "C" {
typedef void (*andropkg_sensor_toggle_fn)(int type, int enabled, int period_us);
// Sensor types the host can feed (Android Sensor.TYPE_* values), and its on/off callback.
void andropkg_sensors_set_host(const int* types, int count, andropkg_sensor_toggle_fn toggle);
// One sample for every queue listening to `type` (values in Android units and axes).
void andropkg_sensor_push(int type, const float* values, int count);
}

namespace {

struct SensorDesc {
  int type;
  const char* name;
  float range, resolution, power;
  int min_delay_us;
};

// Ranges/resolutions of a typical phone IMU.
const SensorDesc kKnown[] = {
    {1, "Accelerometer", 78.4532f, 0.0023942f, 0.17f, 10000},          // TYPE_ACCELEROMETER
    {2, "Magnetic field", 4912.0f, 0.15f, 0.6f, 20000},                 // TYPE_MAGNETIC_FIELD
    {4, "Gyroscope", 34.906586f, 0.0010652f, 0.5f, 10000},              // TYPE_GYROSCOPE
    {9, "Gravity", 19.6133f, 0.0023942f, 0.6f, 10000},                  // TYPE_GRAVITY
    {10, "Linear acceleration", 19.6133f, 0.0023942f, 0.6f, 10000},     // TYPE_LINEAR_ACCELERATION
    {11, "Rotation vector", 1.0f, 5.9604645e-8f, 0.6f, 10000},          // TYPE_ROTATION_VECTOR
};

std::mutex gLock;
std::vector<int> gTypes = {1};  // index + 1 == sensor handle
andropkg_sensor_toggle_fn gToggle = nullptr;
std::map<int, int> gListeners;  // type -> number of enabled queue registrations

const SensorDesc* Desc(int type) {
  for (const auto& d : kKnown) if (d.type == type) return &d;
  return nullptr;
}

int TypeOfHandle(int handle) {
  return handle >= 1 && handle <= static_cast<int>(gTypes.size()) ? gTypes[handle - 1] : -1;
}

jmethodID gWeakRefGet;
jmethodID gDispatchSensorEvent;
jmethodID gDispatchFlushComplete;

struct Event {
  int handle;
  float values[16];
  int64_t timestamp;
};

class EventQueue : public android::MessageHandler {
 public:
  EventQueue(JNIEnv* env, jobject weak, android::sp<android::MessageQueue> mq)
      : weak_(env->NewGlobalRef(weak)), queue_(std::move(mq)) {
    scratch_ = static_cast<jfloatArray>(env->NewGlobalRef(env->NewFloatArray(16)));
  }

  // Under gLock.
  std::set<int> enabled;

  void Post(const Event& e) {
    {
      std::lock_guard<std::mutex> l(pending_lock_);
      if (pending_.size() > 64) pending_.pop_front();  // a stalled Looper keeps the latest
      pending_.push_back(e);
    }
    queue_->getLooper()->sendMessage(this, android::Message(0));
  }

  void PostFlush(int handle) {
    Event e{};
    e.handle = -handle;  // negative: flush-complete marker
    Post(e);
  }

  void Destroy(JNIEnv* env) {
    queue_->getLooper()->removeMessages(this);
    env->DeleteGlobalRef(weak_);
    env->DeleteGlobalRef(scratch_);
    weak_ = nullptr;
  }

  void handleMessage(const android::Message&) override {
    std::deque<Event> batch;
    {
      std::lock_guard<std::mutex> l(pending_lock_);
      batch.swap(pending_);
    }
    if (batch.empty() || weak_ == nullptr) return;
    JNIEnv* env = android::AndroidRuntime::getJNIEnv();
    // weak_ is a java.lang.ref.WeakReference to the BaseEventQueue.
    jobject receiver = env->CallObjectMethod(weak_, gWeakRefGet);
    if (receiver == nullptr) return;
    for (const Event& e : batch) {
      if (e.handle < 0) {
        env->CallVoidMethod(receiver, gDispatchFlushComplete, -e.handle);
      } else {
        env->SetFloatArrayRegion(scratch_, 0, 16, e.values);
        env->CallVoidMethod(receiver, gDispatchSensorEvent, e.handle, scratch_,
                            3 /* SENSOR_STATUS_ACCURACY_HIGH */, e.timestamp);
      }
      if (env->ExceptionCheck()) {
        ALOGE("andropkg: exception dispatching a sensor event");
        env->ExceptionDescribe();
        env->ExceptionClear();
      }
    }
    env->DeleteLocalRef(receiver);
  }

 private:
  jobject weak_;
  jfloatArray scratch_;
  android::sp<android::MessageQueue> queue_;
  std::mutex pending_lock_;
  std::deque<Event> pending_;
};

std::vector<EventQueue*> gQueues;  // under gLock

void SetListening(int type, bool on, int period_us) {
  int& n = gListeners[type];
  int before = n;
  n = std::max(0, n + (on ? 1 : -1));
  if (gToggle != nullptr && (before == 0) != (n == 0)) gToggle(type, n > 0, period_us);
}

// ---- SystemSensorManager -------------------------------------------------------------------

void ssm_classInit(JNIEnv*, jclass) {}
jlong ssm_create(JNIEnv*, jclass, jstring) { return 1; }

jboolean ssm_getSensorAtIndex(JNIEnv* env, jclass, jlong, jobject sensor, jint index) {
  int type;
  {
    std::lock_guard<std::mutex> l(gLock);
    if (index < 0 || index >= static_cast<int>(gTypes.size())) return JNI_FALSE;
    type = gTypes[index];
  }
  const SensorDesc* d = Desc(type);
  if (d == nullptr) return JNI_FALSE;
  jclass c = env->GetObjectClass(sensor);
  auto setStr = [&](const char* f, const char* v) {
    jstring s = env->NewStringUTF(v);
    env->SetObjectField(sensor, env->GetFieldID(c, f, "Ljava/lang/String;"), s);
    env->DeleteLocalRef(s);
  };
  setStr("mName", d->name);
  setStr("mVendor", "andropkg");
  env->SetIntField(sensor, env->GetFieldID(c, "mVersion", "I"), 1);
  env->SetIntField(sensor, env->GetFieldID(c, "mHandle", "I"), index + 1);
  env->SetFloatField(sensor, env->GetFieldID(c, "mMaxRange", "F"), d->range);
  env->SetFloatField(sensor, env->GetFieldID(c, "mResolution", "F"), d->resolution);
  env->SetFloatField(sensor, env->GetFieldID(c, "mPower", "F"), d->power);
  env->SetIntField(sensor, env->GetFieldID(c, "mMinDelay", "I"), d->min_delay_us);
  env->SetIntField(sensor, env->GetFieldID(c, "mMaxDelay", "I"), 1000000);
  env->SetIntField(sensor, env->GetFieldID(c, "mFlags", "I"), 0);  // continuous reporting
  // setType() also fills mStringType ("android.sensor.accelerometer", ...).
  env->CallBooleanMethod(sensor, env->GetMethodID(c, "setType", "(I)Z"), type);
  env->DeleteLocalRef(c);
  return JNI_TRUE;
}

void ssm_getDynamicSensors(JNIEnv*, jclass, jlong, jobject) {}
void ssm_getRuntimeSensors(JNIEnv*, jclass, jlong, jint, jobject) {}
jboolean ssm_isDataInjectionEnabled(JNIEnv*, jclass, jlong) { return JNI_FALSE; }
jint ssm_createDirectChannel(JNIEnv*, jclass, jlong, jint, jlong, jint, jint, jobject) { return -1; }
void ssm_destroyDirectChannel(JNIEnv*, jclass, jlong, jint) {}
jint ssm_configDirectChannel(JNIEnv*, jclass, jlong, jint, jint, jint) { return -1; }
jint ssm_setOperationParameter(JNIEnv*, jclass, jlong, jint, jint, jfloatArray, jintArray) { return -1; }

// ---- SystemSensorManager.BaseEventQueue ----------------------------------------------------

jlong q_init(JNIEnv* env, jclass, jlong, jobject weak, jobject msgQ, jstring, jint, jstring, jstring) {
  android::sp<android::MessageQueue> mq = android::android_os_MessageQueue_getMessageQueue(env, msgQ);
  if (mq == nullptr) {
    jniThrowRuntimeException(env, "MessageQueue is not initialized.");
    return 0;
  }
  auto* q = new EventQueue(env, weak, mq);
  q->incStrong(nullptr);
  std::lock_guard<std::mutex> l(gLock);
  gQueues.push_back(q);
  return reinterpret_cast<jlong>(q);
}

jint q_enable(JNIEnv*, jclass, jlong ptr, jint handle, jint rate_us, jint) {
  auto* q = reinterpret_cast<EventQueue*>(ptr);
  std::lock_guard<std::mutex> l(gLock);
  int type = TypeOfHandle(handle);
  if (type < 0) return -22;  // BAD_VALUE
  if (q->enabled.insert(handle).second) SetListening(type, true, rate_us);
  return 0;
}

jint q_disable(JNIEnv*, jclass, jlong ptr, jint handle) {
  auto* q = reinterpret_cast<EventQueue*>(ptr);
  std::lock_guard<std::mutex> l(gLock);
  if (q->enabled.erase(handle) > 0) SetListening(TypeOfHandle(handle), false, 0);
  return 0;
}

void q_destroy(JNIEnv* env, jclass, jlong ptr) {
  auto* q = reinterpret_cast<EventQueue*>(ptr);
  {
    std::lock_guard<std::mutex> l(gLock);
    for (int h : q->enabled) SetListening(TypeOfHandle(h), false, 0);
    q->enabled.clear();
    gQueues.erase(std::remove(gQueues.begin(), gQueues.end(), q), gQueues.end());
  }
  q->Destroy(env);
  q->decStrong(nullptr);
}

jint q_flush(JNIEnv*, jclass, jlong ptr) {
  auto* q = reinterpret_cast<EventQueue*>(ptr);
  std::lock_guard<std::mutex> l(gLock);
  for (int h : q->enabled) q->PostFlush(h);
  return 0;
}

jint q_inject(JNIEnv*, jclass, jlong, jint, jfloatArray, jint, jlong) { return -38; /* INVALID_OPERATION */ }

#define M(name, sig, fn) {name, sig, reinterpret_cast<void*>(fn)}
const JNINativeMethod kManager[] = {
    M("nativeClassInit", "()V", ssm_classInit),
    M("nativeCreate", "(Ljava/lang/String;)J", ssm_create),
    M("nativeGetSensorAtIndex", "(JLandroid/hardware/Sensor;I)Z", ssm_getSensorAtIndex),
    M("nativeGetDynamicSensors", "(JLjava/util/List;)V", ssm_getDynamicSensors),
    M("nativeGetRuntimeSensors", "(JILjava/util/List;)V", ssm_getRuntimeSensors),
    M("nativeIsDataInjectionEnabled", "(J)Z", ssm_isDataInjectionEnabled),
    M("nativeCreateDirectChannel", "(JIJIILandroid/hardware/HardwareBuffer;)I", ssm_createDirectChannel),
    M("nativeDestroyDirectChannel", "(JI)V", ssm_destroyDirectChannel),
    M("nativeConfigDirectChannel", "(JIII)I", ssm_configDirectChannel),
    M("nativeSetOperationParameter", "(JII[F[I)I", ssm_setOperationParameter),
};
const JNINativeMethod kQueue[] = {
    M("nativeInitBaseEventQueue",
      "(JLjava/lang/ref/WeakReference;Landroid/os/MessageQueue;Ljava/lang/String;ILjava/lang/String;"
      "Ljava/lang/String;)J",
      q_init),
    M("nativeEnableSensor", "(JIII)I", q_enable),
    M("nativeDisableSensor", "(JI)I", q_disable),
    M("nativeDestroySensorEventQueue", "(J)V", q_destroy),
    M("nativeFlushSensor", "(J)I", q_flush),
    M("nativeInjectSensorData", "(JI[FIJ)I", q_inject),
};
#undef M

}  // namespace

extern "C" __attribute__((visibility("default"))) void andropkg_sensors_set_host(const int* types, int count, andropkg_sensor_toggle_fn toggle) {
  std::lock_guard<std::mutex> l(gLock);
  gTypes.clear();
  for (int i = 0; i < count; i++) {
    if (Desc(types[i]) != nullptr) gTypes.push_back(types[i]);
  }
  gToggle = toggle;
}

extern "C" __attribute__((visibility("default"))) void andropkg_sensor_push(int type, const float* values, int count) {
  Event e{};
  e.timestamp = android::elapsedRealtimeNano();
  std::copy(values, values + std::min(count, 16), e.values);
  std::lock_guard<std::mutex> l(gLock);
  auto it = std::find(gTypes.begin(), gTypes.end(), type);
  if (it == gTypes.end()) return;
  e.handle = static_cast<int>(it - gTypes.begin()) + 1;
  for (EventQueue* q : gQueues) {
    if (q->enabled.count(e.handle)) q->Post(e);
  }
}

namespace android {
int register_andropkg_sensors(JNIEnv* env) {
  jclass m = env->FindClass("android/hardware/SystemSensorManager");
  jclass q = env->FindClass("android/hardware/SystemSensorManager$BaseEventQueue");
  if (m == nullptr || q == nullptr) return -1;
  if (env->RegisterNatives(m, kManager, sizeof kManager / sizeof kManager[0]) < 0) return -1;
  if (env->RegisterNatives(q, kQueue, sizeof kQueue / sizeof kQueue[0]) < 0) return -1;
  jclass wr = env->FindClass("java/lang/ref/Reference");
  gWeakRefGet = env->GetMethodID(wr, "get", "()Ljava/lang/Object;");
  gDispatchSensorEvent = env->GetMethodID(q, "dispatchSensorEvent", "(I[FIJ)V");
  gDispatchFlushComplete = env->GetMethodID(q, "dispatchFlushCompleteEvent", "(I)V");
  return gDispatchSensorEvent != nullptr && gDispatchFlushComplete != nullptr ? 0 : -1;
}
}  // namespace android
