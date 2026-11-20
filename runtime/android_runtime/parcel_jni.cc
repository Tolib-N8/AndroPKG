// andropkg: android.os.Parcel without libbinder. There is no other process to send a parcel
// to, but Java code parcels all the time (Parcelable round trips, Bundle copies, metadata for
// SurfaceControl...), so Parcel is a byte buffer with libbinder's layout: values padded to
// 4 bytes, String16/String8/byte arrays length-prefixed (-1 = null). Binders and file
// descriptors can't be flattened to bytes; like libbinder's object list they're kept beside
// the data, keyed by their offset, and a parcel holding them refuses to marshall().
//
// Methods declared @CriticalNative in Parcel.java take no JNIEnv/jclass (see android_os_Parcel.cpp).
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <string>
#include <vector>

#include <nativehelper/JNIHelp.h>
#include <nativehelper/JNIPlatformHelp.h>

#include "jni_util.h"

namespace {

constexpr int32_t kTypeNull = 0;
constexpr int32_t kTypeBinder = 0x73622a85;  // BINDER_TYPE_BINDER ('sb*')
constexpr int32_t kTypeFd = 0x66642a85;      // BINDER_TYPE_FD ('fd*')
constexpr int32_t kInterfaceHeader = 0x53595354;  // 'SYST'
constexpr int32_t kStrictModePenaltyGather = 1 << 31;
constexpr int32_t kUnsetWorkSource = -1;

std::atomic<int64_t> gAllocSize{0};
std::atomic<int64_t> gAllocCount{0};
JavaVM* gVm = nullptr;

JNIEnv* Env() {
  JNIEnv* env = nullptr;
  gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  return env;
}

struct Object {
  int32_t type;
  jobject binder;  // global ref (kTypeBinder)
  int fd;          // owned (kTypeFd)
};

struct Parcel {
  std::vector<uint8_t> data;
  size_t size = 0;
  size_t pos = 0;
  std::map<size_t, Object> objects;  // offset -> object
  bool allowFds = true;

  ~Parcel() { Clear(); }

  void Clear() {
    for (auto& [off, o] : objects) Release(o);
    objects.clear();
    Account(-static_cast<int64_t>(data.capacity()));
    data.clear();
    data.shrink_to_fit();
    size = pos = 0;
  }

  static void Release(Object& o) {
    if (o.type == kTypeBinder && o.binder) Env()->DeleteGlobalRef(o.binder);
    if (o.type == kTypeFd && o.fd >= 0) close(o.fd);
  }

  void Account(int64_t delta) { gAllocSize += delta; }

  void Reserve(size_t n) {
    size_t before = data.capacity();
    if (n > data.size()) data.resize(n);
    Account(static_cast<int64_t>(data.capacity()) - static_cast<int64_t>(before));
  }

  static size_t Pad(size_t n) { return (n + 3) & ~size_t{3}; }

  // Space for n bytes at pos (zero padding), returning where to write.
  uint8_t* WriteInplace(size_t n) {
    size_t padded = Pad(n);
    Reserve(pos + padded);
    uint8_t* p = data.data() + pos;
    if (padded > n) memset(p + n, 0, padded - n);
    DropObjectsIn(pos, padded);
    pos += padded;
    if (pos > size) size = pos;
    return p;
  }

  const uint8_t* ReadInplace(size_t n) {
    size_t padded = Pad(n);
    if (pos > size || padded > size - pos || padded < n) return nullptr;
    const uint8_t* p = data.data() + pos;
    pos += padded;
    return p;
  }

  template <typename T>
  void Write(T v) {
    memcpy(WriteInplace(sizeof v), &v, sizeof v);
  }

  template <typename T>
  T Read() {
    T v{};
    if (const uint8_t* p = ReadInplace(sizeof v)) memcpy(&v, p, sizeof v);
    return v;
  }

  void DropObjectsIn(size_t off, size_t len) {
    for (auto it = objects.lower_bound(off); it != objects.end() && it->first < off + len;) {
      Release(it->second);
      it = objects.erase(it);
    }
  }

  void WriteObject(Object o) {
    size_t at = pos;
    Write<int32_t>(o.type);
    Write<int32_t>(0);
    if (o.type != kTypeNull) objects[at] = o;
  }

  const Object* ReadObject() {
    size_t at = pos;
    int32_t type = Read<int32_t>();
    Read<int32_t>();
    if (type == kTypeNull) return nullptr;
    auto it = objects.find(at);
    return it == objects.end() ? nullptr : &it->second;
  }

  bool HasFds(size_t off, size_t len) const {
    for (auto it = objects.lower_bound(off); it != objects.end() && it->first < off + len; ++it) {
      if (it->second.type == kTypeFd) return true;
    }
    return false;
  }
};

Parcel* P(jlong ptr) { return reinterpret_cast<Parcel*>(ptr); }

// ---- strings ---------------------------------------------------------------------------

void WriteString16(Parcel* p, JNIEnv* env, jstring s) {
  if (s == nullptr) {
    p->Write<int32_t>(-1);
    return;
  }
  jsize len = env->GetStringLength(s);
  p->Write<int32_t>(len);
  uint8_t* dst = p->WriteInplace((static_cast<size_t>(len) + 1) * sizeof(jchar));
  env->GetStringRegion(s, 0, len, reinterpret_cast<jchar*>(dst));
  reinterpret_cast<jchar*>(dst)[len] = 0;
}

jstring ReadString16(Parcel* p, JNIEnv* env) {
  int32_t len = p->Read<int32_t>();
  if (len < 0) return nullptr;
  const uint8_t* src = p->ReadInplace((static_cast<size_t>(len) + 1) * sizeof(jchar));
  if (src == nullptr) return nullptr;
  return env->NewString(reinterpret_cast<const jchar*>(src), len);
}

std::string ToUtf8(JNIEnv* env, jstring s) {
  jsize len = env->GetStringLength(s);
  std::vector<jchar> u(static_cast<size_t>(len));
  env->GetStringRegion(s, 0, len, u.data());
  std::string out;
  for (jsize i = 0; i < len; i++) {
    uint32_t c = u[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < len && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (u[++i] - 0xDC00);
    }
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

jstring FromUtf8(JNIEnv* env, const char* s, size_t n) {
  std::vector<jchar> u;
  for (size_t i = 0; i < n;) {
    uint8_t b = static_cast<uint8_t>(s[i]);
    uint32_t c;
    int extra;
    if (b < 0x80) { c = b; extra = 0; }
    else if ((b & 0xE0) == 0xC0) { c = b & 0x1F; extra = 1; }
    else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; extra = 2; }
    else { c = b & 0x07; extra = 3; }
    i++;
    for (int k = 0; k < extra && i < n; k++, i++) c = (c << 6) | (static_cast<uint8_t>(s[i]) & 0x3F);
    if (c >= 0x10000) {
      c -= 0x10000;
      u.push_back(static_cast<jchar>(0xD800 + (c >> 10)));
      u.push_back(static_cast<jchar>(0xDC00 + (c & 0x3FF)));
    } else {
      u.push_back(static_cast<jchar>(c));
    }
  }
  return env->NewString(u.data(), static_cast<jsize>(u.size()));
}

// ---- JNI -------------------------------------------------------------------------------

void markSensitive(jlong) {}
void markForBinder(JNIEnv*, jclass, jlong, jobject) {}
jboolean isForRpc(jlong) { return JNI_FALSE; }
jint dataSize(jlong ptr) { return static_cast<jint>(P(ptr)->size); }
jint dataAvail(jlong ptr) {
  Parcel* p = P(ptr);
  return p->pos < p->size ? static_cast<jint>(p->size - p->pos) : 0;
}
jint dataPosition(jlong ptr) { return static_cast<jint>(P(ptr)->pos); }
jint dataCapacity(jlong ptr) { return static_cast<jint>(P(ptr)->data.size()); }

void setDataSize(JNIEnv*, jclass, jlong ptr, jint size) {
  Parcel* p = P(ptr);
  size_t n = size < 0 ? 0 : static_cast<size_t>(size);
  p->Reserve(n);
  if (n < p->size) p->DropObjectsIn(n, p->size - n);
  p->size = n;
  if (p->pos > n) p->pos = n;
}
void setDataPosition(jlong ptr, jint pos) {
  Parcel* p = P(ptr);
  if (pos >= 0 && static_cast<size_t>(pos) <= p->size) p->pos = static_cast<size_t>(pos);
}
void setDataCapacity(JNIEnv*, jclass, jlong ptr, jint size) {
  if (size > 0) P(ptr)->Reserve(static_cast<size_t>(size));
}
jboolean pushAllowFds(jlong ptr, jboolean allow) {
  Parcel* p = P(ptr);
  bool last = p->allowFds;
  if (!allow) p->allowFds = false;
  return last;
}
void restoreAllowFds(jlong ptr, jboolean last) { P(ptr)->allowFds = last; }

void writeByteArray(JNIEnv* env, jclass, jlong ptr, jbyteArray data, jint offset, jint length) {
  Parcel* p = P(ptr);
  if (data == nullptr) {
    p->Write<int32_t>(-1);
    return;
  }
  p->Write<int32_t>(length);
  uint8_t* dst = p->WriteInplace(static_cast<size_t>(length));
  env->GetByteArrayRegion(data, offset, length, reinterpret_cast<jbyte*>(dst));
}
jint writeInt(jlong ptr, jint v) { P(ptr)->Write<int32_t>(v); return 0; }
jint writeLong(jlong ptr, jlong v) { P(ptr)->Write<int64_t>(v); return 0; }
jint writeFloat(jlong ptr, jfloat v) { P(ptr)->Write<float>(v); return 0; }
jint writeDouble(jlong ptr, jdouble v) { P(ptr)->Write<double>(v); return 0; }

void signalExceptionForError(JNIEnv* env, jclass, jint err) {
  const char* cls = err == -ENOMEM ? "java/lang/OutOfMemoryError"
                    : err == -EINVAL || err == -EBADMSG ? "java/lang/IllegalArgumentException"
                                                        : "java/lang/RuntimeException";
  jniThrowExceptionFmt(env, cls, "Parcel error %d", err);
}

void writeString8(JNIEnv* env, jclass, jlong ptr, jstring s) {
  Parcel* p = P(ptr);
  if (s == nullptr) {
    p->Write<int32_t>(-1);
    return;
  }
  std::string u = ToUtf8(env, s);
  p->Write<int32_t>(static_cast<int32_t>(u.size()));
  uint8_t* dst = p->WriteInplace(u.size() + 1);
  memcpy(dst, u.c_str(), u.size() + 1);
}
void writeString16(JNIEnv* env, jclass, jlong ptr, jstring s) { WriteString16(P(ptr), env, s); }

void writeStrongBinder(JNIEnv* env, jclass, jlong ptr, jobject binder) {
  if (binder == nullptr) {
    P(ptr)->WriteObject({kTypeNull, nullptr, -1});
  } else {
    P(ptr)->WriteObject({kTypeBinder, env->NewGlobalRef(binder), -1});
  }
}
void writeFileDescriptor(JNIEnv* env, jclass, jlong ptr, jobject fdObj) {
  Parcel* p = P(ptr);
  int fd = fdObj ? jniGetFDFromFileDescriptor(env, fdObj) : -1;
  if (!p->allowFds) {
    jniThrowException(env, "java/lang/RuntimeException", "Not allowed to write file descriptors here");
    return;
  }
  int copy = fd >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1;
  if (fd >= 0 && copy < 0) {
    jniThrowIOException(env, errno);
    return;
  }
  p->WriteObject({copy < 0 ? kTypeNull : kTypeFd, nullptr, copy});
}

jbyteArray createByteArray(JNIEnv* env, jclass, jlong ptr) {
  Parcel* p = P(ptr);
  int32_t len = p->Read<int32_t>();
  if (len < 0 || static_cast<size_t>(len) > static_cast<size_t>(dataAvail(ptr))) return nullptr;
  const uint8_t* src = p->ReadInplace(static_cast<size_t>(len));
  jbyteArray a = env->NewByteArray(len);
  if (a && src) env->SetByteArrayRegion(a, 0, len, reinterpret_cast<const jbyte*>(src));
  return a;
}
jboolean readByteArray(JNIEnv* env, jclass, jlong ptr, jbyteArray dest, jint destLen) {
  Parcel* p = P(ptr);
  int32_t len = p->Read<int32_t>();
  if (len < 0 || len != destLen || static_cast<size_t>(len) > static_cast<size_t>(dataAvail(ptr))) {
    return JNI_FALSE;
  }
  const uint8_t* src = p->ReadInplace(static_cast<size_t>(len));
  if (src == nullptr) return JNI_FALSE;
  env->SetByteArrayRegion(dest, 0, len, reinterpret_cast<const jbyte*>(src));
  return JNI_TRUE;
}
jint readInt(jlong ptr) { return P(ptr)->Read<int32_t>(); }
jlong readLong(jlong ptr) { return P(ptr)->Read<int64_t>(); }
jfloat readFloat(jlong ptr) { return P(ptr)->Read<float>(); }
jdouble readDouble(jlong ptr) { return P(ptr)->Read<double>(); }

jstring readString8(JNIEnv* env, jclass, jlong ptr) {
  Parcel* p = P(ptr);
  int32_t len = p->Read<int32_t>();
  if (len < 0) return nullptr;
  const uint8_t* src = p->ReadInplace(static_cast<size_t>(len) + 1);
  return src ? FromUtf8(env, reinterpret_cast<const char*>(src), static_cast<size_t>(len)) : nullptr;
}
jstring readString16(JNIEnv* env, jclass, jlong ptr) { return ReadString16(P(ptr), env); }

jobject readStrongBinder(JNIEnv* env, jclass, jlong ptr) {
  const Object* o = P(ptr)->ReadObject();
  return o && o->type == kTypeBinder ? env->NewLocalRef(o->binder) : nullptr;
}
jobject readFileDescriptor(JNIEnv* env, jclass, jlong ptr) {
  const Object* o = P(ptr)->ReadObject();
  if (o == nullptr || o->type != kTypeFd) return nullptr;
  int copy = fcntl(o->fd, F_DUPFD_CLOEXEC, 0);  // the parcel keeps its own descriptor
  return copy < 0 ? nullptr : jniCreateFileDescriptor(env, copy);
}

jlong create(JNIEnv*, jclass) {
  gAllocCount++;
  return reinterpret_cast<jlong>(new Parcel);
}
void freeBuffer(JNIEnv*, jclass, jlong ptr) { P(ptr)->Clear(); }
void destroy(JNIEnv*, jclass, jlong ptr) {
  gAllocCount--;
  delete P(ptr);
}

jbyteArray marshall(JNIEnv* env, jclass, jlong ptr) {
  Parcel* p = P(ptr);
  if (!p->objects.empty()) {
    jniThrowException(env, "java/lang/RuntimeException",
                      "Tried to marshall a Parcel that contains objects (binders or FDs).");
    return nullptr;
  }
  jbyteArray a = env->NewByteArray(static_cast<jsize>(p->size));
  if (a) env->SetByteArrayRegion(a, 0, static_cast<jsize>(p->size), reinterpret_cast<const jbyte*>(p->data.data()));
  return a;
}
void unmarshall(JNIEnv* env, jclass, jlong ptr, jbyteArray data, jint offset, jint length) {
  Parcel* p = P(ptr);
  if (data == nullptr || length < 0) return;
  p->Clear();
  p->Reserve(static_cast<size_t>(length));
  env->GetByteArrayRegion(data, offset, length, reinterpret_cast<jbyte*>(p->data.data()));
  p->size = static_cast<size_t>(length);
  p->pos = 0;
}

jint compareData(JNIEnv*, jclass, jlong a, jlong b) {
  Parcel* x = P(a);
  Parcel* y = P(b);
  size_t n = x->size < y->size ? x->size : y->size;
  int c = n ? memcmp(x->data.data(), y->data.data(), n) : 0;
  if (c != 0) return c;
  return x->size == y->size ? 0 : (x->size < y->size ? -1 : 1);
}
jboolean compareDataInRange(JNIEnv*, jclass, jlong a, jint aOff, jlong b, jint bOff, jint len) {
  Parcel* x = P(a);
  Parcel* y = P(b);
  if (aOff < 0 || bOff < 0 || len < 0 || static_cast<size_t>(aOff) + len > x->size ||
      static_cast<size_t>(bOff) + len > y->size) {
    return JNI_FALSE;
  }
  return memcmp(x->data.data() + aOff, y->data.data() + bOff, static_cast<size_t>(len)) == 0;
}

void appendFrom(JNIEnv* env, jclass, jlong thisPtr, jlong otherPtr, jint offset, jint length) {
  Parcel* p = P(thisPtr);
  Parcel* o = P(otherPtr);
  if (offset < 0 || length < 0 || static_cast<size_t>(offset) + length > o->size) {
    jniThrowException(env, "java/lang/IllegalArgumentException", "appendFrom: bad range");
    return;
  }
  size_t at = p->pos;
  uint8_t* dst = p->WriteInplace(static_cast<size_t>(length));
  memcpy(dst, o->data.data() + offset, static_cast<size_t>(length));
  for (auto it = o->objects.lower_bound(static_cast<size_t>(offset));
       it != o->objects.end() && it->first < static_cast<size_t>(offset) + length; ++it) {
    Object copy = it->second;
    if (copy.type == kTypeBinder) copy.binder = env->NewGlobalRef(copy.binder);
    if (copy.type == kTypeFd) copy.fd = fcntl(copy.fd, F_DUPFD_CLOEXEC, 0);
    p->objects[at + (it->first - offset)] = copy;
  }
}

jboolean hasFileDescriptors(jlong ptr) { return P(ptr)->HasFds(0, P(ptr)->size); }
jboolean hasFileDescriptorsInRange(JNIEnv* env, jclass, jlong ptr, jint offset, jint length) {
  if (offset < 0 || length < 0) {
    jniThrowException(env, "java/lang/IllegalArgumentException", "bad range");
    return JNI_FALSE;
  }
  return P(ptr)->HasFds(static_cast<size_t>(offset), static_cast<size_t>(length));
}

void writeInterfaceToken(JNIEnv* env, jclass, jlong ptr, jstring name) {
  Parcel* p = P(ptr);
  p->Write<int32_t>(kStrictModePenaltyGather);
  p->Write<int32_t>(kUnsetWorkSource);
  p->Write<int32_t>(kInterfaceHeader);
  WriteString16(p, env, name);
}
void enforceInterface(JNIEnv* env, jclass, jlong ptr, jstring name) {
  Parcel* p = P(ptr);
  p->Read<int32_t>();  // strict mode policy
  p->Read<int32_t>();  // work source
  int32_t header = p->Read<int32_t>();
  jstring got = ReadString16(p, env);
  bool ok = header == kInterfaceHeader && got != nullptr && name != nullptr &&
            ToUtf8(env, got) == ToUtf8(env, name);
  if (!ok) jniThrowException(env, "java/lang/SecurityException", "Binder invocation to an incorrect interface");
}

jlong getGlobalAllocSize(JNIEnv*, jclass) { return gAllocSize; }
jlong getGlobalAllocCount(JNIEnv*, jclass) { return gAllocCount; }
jlong getOpenAshmemSize(jlong) { return 0; }
jint readCallingWorkSourceUid(jlong) { return kUnsetWorkSource; }
jboolean replaceCallingWorkSourceUid(jlong, jint) { return JNI_FALSE; }

#define M(name, sig, fn) {name, sig, reinterpret_cast<void*>(fn)}
const JNINativeMethod kMethods[] = {
    M("nativeMarkSensitive", "(J)V", markSensitive),
    M("nativeMarkForBinder", "(JLandroid/os/IBinder;)V", markForBinder),
    M("nativeIsForRpc", "(J)Z", isForRpc),
    M("nativeDataSize", "(J)I", dataSize),
    M("nativeDataAvail", "(J)I", dataAvail),
    M("nativeDataPosition", "(J)I", dataPosition),
    M("nativeDataCapacity", "(J)I", dataCapacity),
    M("nativeSetDataSize", "(JI)V", setDataSize),
    M("nativeSetDataPosition", "(JI)V", setDataPosition),
    M("nativeSetDataCapacity", "(JI)V", setDataCapacity),
    M("nativePushAllowFds", "(JZ)Z", pushAllowFds),
    M("nativeRestoreAllowFds", "(JZ)V", restoreAllowFds),
    M("nativeWriteByteArray", "(J[BII)V", writeByteArray),
    M("nativeWriteBlob", "(J[BII)V", writeByteArray),
    M("nativeWriteInt", "(JI)I", writeInt),
    M("nativeWriteLong", "(JJ)I", writeLong),
    M("nativeWriteFloat", "(JF)I", writeFloat),
    M("nativeWriteDouble", "(JD)I", writeDouble),
    M("nativeSignalExceptionForError", "(I)V", signalExceptionForError),
    M("nativeWriteString8", "(JLjava/lang/String;)V", writeString8),
    M("nativeWriteString16", "(JLjava/lang/String;)V", writeString16),
    M("nativeWriteStrongBinder", "(JLandroid/os/IBinder;)V", writeStrongBinder),
    M("nativeWriteFileDescriptor", "(JLjava/io/FileDescriptor;)V", writeFileDescriptor),
    M("nativeCreateByteArray", "(J)[B", createByteArray),
    M("nativeReadByteArray", "(J[BI)Z", readByteArray),
    M("nativeReadBlob", "(J)[B", createByteArray),
    M("nativeReadInt", "(J)I", readInt),
    M("nativeReadLong", "(J)J", readLong),
    M("nativeReadFloat", "(J)F", readFloat),
    M("nativeReadDouble", "(J)D", readDouble),
    M("nativeReadString8", "(J)Ljava/lang/String;", readString8),
    M("nativeReadString16", "(J)Ljava/lang/String;", readString16),
    M("nativeReadStrongBinder", "(J)Landroid/os/IBinder;", readStrongBinder),
    M("nativeReadFileDescriptor", "(J)Ljava/io/FileDescriptor;", readFileDescriptor),
    M("nativeCreate", "()J", create),
    M("nativeFreeBuffer", "(J)V", freeBuffer),
    M("nativeDestroy", "(J)V", destroy),
    M("nativeMarshall", "(J)[B", marshall),
    M("nativeUnmarshall", "(J[BII)V", unmarshall),
    M("nativeCompareData", "(JJ)I", compareData),
    M("nativeCompareDataInRange", "(JIJII)Z", compareDataInRange),
    M("nativeAppendFrom", "(JJII)V", appendFrom),
    M("nativeHasFileDescriptors", "(J)Z", hasFileDescriptors),
    M("nativeHasFileDescriptorsInRange", "(JII)Z", hasFileDescriptorsInRange),
    M("nativeWriteInterfaceToken", "(JLjava/lang/String;)V", writeInterfaceToken),
    M("nativeEnforceInterface", "(JLjava/lang/String;)V", enforceInterface),
    M("getGlobalAllocSize", "()J", getGlobalAllocSize),
    M("getGlobalAllocCount", "()J", getGlobalAllocCount),
    M("nativeGetOpenAshmemSize", "(J)J", getOpenAshmemSize),
    M("nativeReadCallingWorkSourceUid", "(J)I", readCallingWorkSourceUid),
    M("nativeReplaceCallingWorkSourceUid", "(JI)Z", replaceCallingWorkSourceUid),
};
#undef M

}  // namespace

namespace android {
int register_andropkg_parcel(JNIEnv* env) {
  env->GetJavaVM(&gVm);
  return andropkg::RegisterIfDeclared(env, "android/os/Parcel", kMethods,
                                      static_cast<int>(sizeof kMethods / sizeof kMethods[0]));
}
}  // namespace android
