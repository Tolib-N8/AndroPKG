// andropkg: libsoundpool — android.media.SoundPool on a Core Audio mixer.
//
// Samples are decoded whole into float PCM (runtime/media/decoder: Ogg Vorbis/Opus included,
// the usual format of game sounds) on a background queue, then SAMPLE_LOADED is posted. Each
// SoundPool mixes its playing streams into one 48 kHz stereo AudioQueue: per-stream left/right
// volume, rate (linear-interpolation resampling, which also converts the sample rate), loops,
// pause/resume, and maxStreams with priority-based stealing, as the real SoundPool does.
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <AVFoundation/AVFoundation.h>
#endif
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "decoder.h"
#include "jni_defaults.h"

namespace {
using andropkg_media::PcmDecoder;

JavaVM* gVm;
jfieldID gNativeContext;  // SoundPool.mNativeContext
jmethodID gPostEvent;     // void postEventFromNative(int msg, int arg1, int arg2, Object obj)
constexpr int kSampleLoaded = 1;
constexpr int kOutRate = 48000, kBufferFrames = 960, kBuffers = 3;  // 20 ms buffers

class ScopedEnv {
 public:
  ScopedEnv() {
    if (gVm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) != JNI_OK) {
      gVm->AttachCurrentThread(&env_, nullptr);
      attached_ = true;
    }
  }
  ~ScopedEnv() {
    if (attached_) gVm->DetachCurrentThread();
  }
  JNIEnv* operator->() const { return env_; }

 private:
  JNIEnv* env_ = nullptr;
  bool attached_ = false;
};

void ActivateAudioSession() {
#if TARGET_OS_IPHONE
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    AVAudioSession* s = [AVAudioSession sharedInstance];
    [s setCategory:AVAudioSessionCategoryPlayback error:nil];
    [s setActive:YES error:nil];
  });
#endif
}

struct Sample {
  std::vector<float> pcm;  // interleaved, `channels` per frame
  int rate = 0, channels = 0;
  int64_t frames = 0;
  std::atomic<bool> ready{false};
};

struct Stream {
  int id;
  std::shared_ptr<Sample> sample;
  double pos = 0, step = 1;
  float left = 1, right = 1, rate = 1;
  int loop = 0, priority = 0;
  bool paused = false, autoPaused = false;
};

class Pool {
 public:
  Pool(JNIEnv* env, jobject thiz, int max_streams) : max_(std::max(1, max_streams)) {
    weak_ = env->NewWeakGlobalRef(thiz);
    loader_ = dispatch_queue_create("andropkg.soundpool.load", DISPATCH_QUEUE_SERIAL);
    AudioStreamBasicDescription f = {};
    f.mSampleRate = kOutRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = f.mBytesPerPacket = 8;
    f.mFramesPerPacket = 1;
    if (AudioQueueNewOutput(&f, &Pool::Callback, this, nullptr, nullptr, 0, &queue_) == noErr) {
      for (auto& b : buffers_) AudioQueueAllocateBuffer(queue_, kBufferFrames * 8, &b);
    }
  }

  void Release(JNIEnv* env) {
    {
      std::lock_guard<std::mutex> l(lock_);
      streams_.clear();
      released_ = true;
    }
    if (queue_) AudioQueueDispose(queue_, true);
    queue_ = nullptr;
    if (weak_) env->DeleteWeakGlobalRef(weak_);
    weak_ = nullptr;
  }

  int Load(int fd, int64_t offset, int64_t length) {
    std::shared_ptr<PcmDecoder> dec(andropkg_media::OpenDecoder(fd, offset, length).release());
    int id;
    auto sample = std::make_shared<Sample>();
    {
      std::lock_guard<std::mutex> l(lock_);
      id = next_sample_++;
      samples_[id] = sample;
    }
    jobject weak = weak_;
    dispatch_async(loader_, ^{
      int status = 1;  // nonzero: load error
      if (dec) {
        sample->rate = dec->rate();
        sample->channels = dec->channels();
        std::vector<float> chunk(size_t(4096) * sample->channels);
        if (dec->frames() > 0) sample->pcm.reserve(size_t(dec->frames()) * sample->channels);
        for (int n; (n = dec->Read(chunk.data(), 4096)) > 0;) {
          sample->pcm.insert(sample->pcm.end(), chunk.begin(), chunk.begin() + long(n) * sample->channels);
        }
        sample->frames = int64_t(sample->pcm.size()) / sample->channels;
        sample->ready = sample->frames > 0;
        status = sample->ready ? 0 : 1;
      }
      Post(weak, kSampleLoaded, id, status);
    });
    return id;
  }

  bool Unload(int id) {
    std::lock_guard<std::mutex> l(lock_);
    return samples_.erase(id) > 0;
  }

  int Play(int sample_id, float left, float right, int priority, int loop, float rate) {
    bool start = false;
    int id;
    {
      std::lock_guard<std::mutex> l(lock_);
      auto it = samples_.find(sample_id);
      if (it == samples_.end() || !it->second->ready) return 0;
      if (int(streams_.size()) >= max_) {
        // Steal the lowest-priority stream (oldest first) if it is not above the new one.
        auto victim = std::min_element(streams_.begin(), streams_.end(),
                                       [](const Stream& a, const Stream& b) { return a.priority < b.priority; });
        if (victim->priority > priority) return 0;
        streams_.erase(victim);
      }
      Stream s;
      s.id = id = next_stream_++;
      s.sample = it->second;
      s.left = left;
      s.right = right;
      s.priority = priority;
      s.loop = loop;
      s.rate = std::clamp(rate, 0.5f, 2.0f);
      s.step = double(s.sample->rate) / kOutRate * s.rate;
      streams_.push_back(s);
      if (!running_ && queue_) {
        running_ = start = true;
        idle_buffers_ = 0;
        for (auto& b : buffers_) FillLocked(b);
      }
    }
    if (start) {
      ActivateAudioSession();
      AudioQueueStart(queue_, nullptr);
    }
    return id;
  }

  template <typename F>
  void WithStream(int id, F f) {
    std::lock_guard<std::mutex> l(lock_);
    for (auto& s : streams_) {
      if (s.id == id) {
        f(s);
        return;
      }
    }
  }

  void Stop(int id) {
    std::lock_guard<std::mutex> l(lock_);
    streams_.erase(std::remove_if(streams_.begin(), streams_.end(), [id](const Stream& s) { return s.id == id; }),
                   streams_.end());
  }

  void AutoPause(bool pause) {
    std::lock_guard<std::mutex> l(lock_);
    for (auto& s : streams_) {
      if (pause && !s.paused) {
        s.paused = s.autoPaused = true;
      } else if (!pause && s.autoPaused) {
        s.paused = s.autoPaused = false;
      }
    }
  }

  void Mute(bool m) { muted_ = m; }

 private:
  void Post(jobject weak, int msg, int a1, int a2) {
    ScopedEnv env;
    jobject self = env->NewLocalRef(weak);
    if (!self) return;
    env->CallVoidMethod(self, gPostEvent, msg, a1, a2, nullptr);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(self);
  }

  static void Callback(void* self, AudioQueueRef, AudioQueueBufferRef b) {
    auto* p = static_cast<Pool*>(self);
    bool stop = false;
    {
      std::lock_guard<std::mutex> l(p->lock_);
      if (p->released_ || !p->running_) return;
      p->FillLocked(b);
      // About a second of silence with nothing playing: let the queue sleep.
      p->idle_buffers_ = p->streams_.empty() ? p->idle_buffers_ + 1 : 0;
      if (p->idle_buffers_ > 50) {
        p->running_ = false;
        stop = true;
      }
    }
    if (stop) AudioQueueStop(p->queue_, false);
  }

  void FillLocked(AudioQueueBufferRef b) {
    float* out = static_cast<float*>(b->mAudioData);
    std::fill(out, out + kBufferFrames * 2, 0.f);
    for (auto it = streams_.begin(); it != streams_.end();) {
      Stream& s = *it;
      bool done = false;
      if (!s.paused) {
        const Sample& sm = *s.sample;
        const float* pcm = sm.pcm.data();
        int ch = sm.channels;
        for (int i = 0; i < kBufferFrames; i++) {
          if (s.pos >= double(sm.frames - 1)) {
            if (s.loop != 0) {
              s.pos = 0;
              if (s.loop > 0) s.loop--;
            } else {
              done = true;
              break;
            }
          }
          int64_t i0 = int64_t(s.pos);
          float frac = float(s.pos - double(i0));
          const float* a = pcm + i0 * ch;
          const float* c = a + ch;
          float l = a[0] + (c[0] - a[0]) * frac;
          float r = ch > 1 ? a[1] + (c[1] - a[1]) * frac : l;
          out[2 * i] += l * s.left;
          out[2 * i + 1] += r * s.right;
          s.pos += s.step;
        }
      }
      it = done ? streams_.erase(it) : it + 1;
    }
    for (int i = 0; i < kBufferFrames * 2; i++) out[i] = muted_ ? 0.f : std::clamp(out[i], -1.f, 1.f);
    b->mAudioDataByteSize = kBufferFrames * 8;
    AudioQueueEnqueueBuffer(queue_, b, 0, nullptr);
  }

  const int max_;
  jobject weak_ = nullptr;
  dispatch_queue_t loader_;
  AudioQueueRef queue_ = nullptr;
  AudioQueueBufferRef buffers_[kBuffers] = {};
  std::mutex lock_;
  std::map<int, std::shared_ptr<Sample>> samples_;
  std::vector<Stream> streams_;
  int next_sample_ = 1, next_stream_ = 1, idle_buffers_ = 0;
  bool running_ = false, released_ = false;
  std::atomic<bool> muted_{false};
};

Pool* Get(JNIEnv* env, jobject thiz) { return reinterpret_cast<Pool*>(env->GetLongField(thiz, gNativeContext)); }

jint sp_setup(JNIEnv* env, jobject thiz, jint max_streams, jobject /*attributes*/, jstring) {
  env->SetLongField(thiz, gNativeContext, reinterpret_cast<jlong>(new Pool(env, thiz, max_streams)));
  return 0;
}

void sp_release(JNIEnv* env, jobject thiz) {
  if (Pool* p = Get(env, thiz)) {
    env->SetLongField(thiz, gNativeContext, 0);
    p->Release(env);
    // The mixer callback may still be finishing on Core Audio's thread: keep the object.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_global_queue(0, 0), ^{ delete p; });
  }
}

jint sp_load(JNIEnv* env, jobject thiz, jobject fdObj, jlong offset, jlong length, jint) {
  Pool* p = Get(env, thiz);
  if (!p || !fdObj) return 0;
  jclass c = env->FindClass("java/io/FileDescriptor");
  int fd = env->GetIntField(fdObj, env->GetFieldID(c, "descriptor", "I"));
  env->DeleteLocalRef(c);
  return p->Load(fd, offset, length);
}

jboolean sp_unload(JNIEnv* env, jobject thiz, jint id) { Pool* p = Get(env, thiz); return p && p->Unload(id); }

jint sp_play(JNIEnv* env, jobject thiz, jint sample, jfloat l, jfloat r, jint priority, jint loop, jfloat rate, jint) {
  Pool* p = Get(env, thiz);
  return p ? p->Play(sample, l, r, priority, loop, rate) : 0;
}

void sp_pause(JNIEnv* env, jobject thiz, jint id) {
  if (Pool* p = Get(env, thiz)) p->WithStream(id, [](Stream& s) { s.paused = true; s.autoPaused = false; });
}
void sp_resume(JNIEnv* env, jobject thiz, jint id) {
  if (Pool* p = Get(env, thiz)) p->WithStream(id, [](Stream& s) { s.paused = false; });
}
void sp_stop(JNIEnv* env, jobject thiz, jint id) { if (Pool* p = Get(env, thiz)) p->Stop(id); }
void sp_autoPause(JNIEnv* env, jobject thiz) { if (Pool* p = Get(env, thiz)) p->AutoPause(true); }
void sp_autoResume(JNIEnv* env, jobject thiz) { if (Pool* p = Get(env, thiz)) p->AutoPause(false); }
void sp_setVolume(JNIEnv* env, jobject thiz, jint id, jfloat l, jfloat r) {
  if (Pool* p = Get(env, thiz)) p->WithStream(id, [=](Stream& s) { s.left = l; s.right = r; });
}
void sp_setPriority(JNIEnv* env, jobject thiz, jint id, jint pr) {
  if (Pool* p = Get(env, thiz)) p->WithStream(id, [=](Stream& s) { s.priority = pr; });
}
void sp_setLoop(JNIEnv* env, jobject thiz, jint id, jint loop) {
  if (Pool* p = Get(env, thiz)) p->WithStream(id, [=](Stream& s) { s.loop = loop; });
}
void sp_setRate(JNIEnv* env, jobject thiz, jint id, jfloat rate) {
  if (Pool* p = Get(env, thiz)) {
    p->WithStream(id, [=](Stream& s) {
      s.rate = std::clamp(rate, 0.5f, 2.0f);
      s.step = double(s.sample->rate) / kOutRate * s.rate;
    });
  }
}
void sp_mute(JNIEnv* env, jobject thiz, jboolean m) { if (Pool* p = Get(env, thiz)) p->Mute(m); }

#define M(name, sig, fn) {const_cast<char*>(name), const_cast<char*>(sig), reinterpret_cast<void*>(fn)}
const JNINativeMethod kSoundPool[] = {
    M("native_setup", "(ILjava/lang/Object;Ljava/lang/String;)I", sp_setup),
    M("native_release", "()V", sp_release),
    M("_load", "(Ljava/io/FileDescriptor;JJI)I", sp_load),
    M("unload", "(I)Z", sp_unload),
    M("_play", "(IFFIIFI)I", sp_play),
    M("pause", "(I)V", sp_pause),
    M("resume", "(I)V", sp_resume),
    M("stop", "(I)V", sp_stop),
    M("autoPause", "()V", sp_autoPause),
    M("autoResume", "()V", sp_autoResume),
    M("_setVolume", "(IFF)V", sp_setVolume),
    M("setPriority", "(II)V", sp_setPriority),
    M("setLoop", "(II)V", sp_setLoop),
    M("setRate", "(IF)V", sp_setRate),
    M("_mute", "(Z)V", sp_mute),
};
#undef M
}  // namespace

extern "C" __attribute__((visibility("default"))) jint JNI_OnLoad(JavaVM* vm, void*) {
  gVm = vm;
  JNIEnv* env = nullptr;
  vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  jclass c = env->FindClass("android/media/SoundPool");
  if (c == nullptr) {
    env->ExceptionClear();
    return JNI_VERSION_1_6;
  }
  gNativeContext = env->GetFieldID(c, "mNativeContext", "J");
  gPostEvent = env->GetMethodID(c, "postEventFromNative", "(IIILjava/lang/Object;)V");
  env->DeleteLocalRef(c);
  andropkg_media::RegisterNativeDefaults(env, "android/media/SoundPool");
  andropkg_media::RegisterDeclared(env, "android/media/SoundPool", kSoundPool, sizeof kSoundPool / sizeof kSoundPool[0]);
  return JNI_VERSION_1_6;
}
