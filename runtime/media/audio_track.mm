// andropkg: android.media.AudioTrack (and AudioSystem's neutral answers) without audioflinger.
//
// Each AudioTrack is an AudioQueue output in the track's own sample rate and channel count
// (Core Audio resamples and mixes). PCM written by the app is converted to float into a ring
// buffer (MODE_STREAM) or kept whole (MODE_STATIC); the queue's callback pulls from it. The
// playback head advances by the app frames in each buffer the queue has finished playing,
// which also gives getTimestamp() its (position, time) pairs.
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#import <MediaPlayer/MediaPlayer.h>
#if TARGET_OS_IPHONE
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif
#include <jni.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "jni_defaults.h"

namespace {

using andropkg_media::RegisterDeclared;

JavaVM* gVm;
jfieldID gNativeTrack;   // AudioTrack.mNativeTrackInJavaObj
jmethodID gPostEvent;    // static AudioTrack.postEventFromNative(Object, int, int, int, Object)
jclass gAudioTrackClass;

constexpr int kSuccess = 0, kError = -1, kBadValue = -2, kInvalidOperation = -3;
constexpr int kEventMarker = 3, kEventNewPos = 4;  // AudioSystem.NATIVE_EVENT_*
constexpr int kModeStatic = 0;
constexpr int kBuffers = 3;

int64_t NowNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);  // System.nanoTime()
  return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

int BytesPerSample(int format) {
  switch (format) {
    case 3: return 1;   // ENCODING_PCM_8BIT
    case 2: return 2;   // ENCODING_PCM_16BIT
    case 21: return 3;  // ENCODING_PCM_24BIT_PACKED
    case 4:             // ENCODING_PCM_FLOAT
    case 22: return 4;  // ENCODING_PCM_32BIT
    default: return 0;  // compressed (AC3, ...) is not supported
  }
}

float SampleAt(const uint8_t* p, int format) {
  switch (format) {
    case 3: return (int(*p) - 128) / 128.f;
    case 2: { int16_t v; memcpy(&v, p, 2); return v / 32768.f; }
    case 21: { int32_t v = int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24); return v / 2147483648.f; }
    case 22: { int32_t v; memcpy(&v, p, 4); return v / 2147483648.f; }
    case 4: { float v; memcpy(&v, p, 4); return v; }
  }
  return 0;
}

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

/// A JNIEnv for the current thread; threads the VM does not know (GCD workers, Core Audio)
/// are attached for the scope only, since they may exit without telling ART.
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
  JNIEnv* get() const { return env_; }

 private:
  JNIEnv* env_ = nullptr;
  bool attached_ = false;
};

class Track {
 public:
  Track(JNIEnv* env, jobject weak, int rate, int src_channels, int format, int capacity_frames, int mode)
      : rate_(rate), src_channels_(src_channels), out_channels_(std::min(src_channels, 2)), format_(format),
        mode_(mode), capacity_(std::max(capacity_frames, 256)) {
    weak_ = env->NewGlobalRef(weak);
    if (mode_ == kModeStatic) {
      static_.reserve(size_t(capacity_) * out_channels_);
    } else {
      ring_.resize(size_t(capacity_) * out_channels_);
    }
    AudioStreamBasicDescription f = {};
    f.mSampleRate = rate_;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = out_channels_;
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = f.mBytesPerPacket = 4 * out_channels_;
    f.mFramesPerPacket = 1;
    ok_ = AudioQueueNewOutput(&f, &Track::Callback, this, nullptr, nullptr, 0, &queue_) == noErr;
    buffer_frames_ = std::max(256, rate_ / 100 * 2);  // ~20 ms
    for (int i = 0; ok_ && i < kBuffers; i++) {
      ok_ = AudioQueueAllocateBuffer(queue_, buffer_frames_ * f.mBytesPerFrame, &buffers_[i]) == noErr;
    }
  }

  ~Track() {
    if (queue_) AudioQueueDispose(queue_, true);
  }

  bool ok() const { return ok_; }
  int capacity() const { return capacity_; }
  int latency_ms() const { return (kBuffers * buffer_frames_ * 1000) / rate_ + 10; }

  void ReleaseRef(JNIEnv* env) {
    if (weak_) env->DeleteGlobalRef(weak_);
    weak_ = nullptr;
  }

  // ---- control -----------------------------------------------------------------------------
  // Core Audio control calls are made without lock_ held: they may run buffer callbacks.
  void Start() {
    ActivateAudioSession();
    {
    std::lock_guard<std::recursive_mutex> l(lock_);
    if (state_ == kPlaying) return;
    if (state_ == kStopped || state_ == kFlushed) {
      position_ = 0;
      next_period_ = period_;
      marker_reached_ = false;
    }
    bool resume = state_ == kPaused;
    state_ = kPlaying;
    stopping_ = false;
    if (!resume) {
      for (int i = 0; i < kBuffers; i++) FillLocked(buffers_[i]);
    }
    }
    AudioQueueStart(queue_, nullptr);
  }

  void Pause() {
    {
      std::lock_guard<std::recursive_mutex> l(lock_);
      if (state_ != kPlaying) return;
      state_ = kPaused;
      space_.notify_all();
    }
    AudioQueuePause(queue_);
  }

  void Stop() {
    {
      std::lock_guard<std::recursive_mutex> l(lock_);
      space_.notify_all();
      if (state_ == kPlaying && mode_ != kModeStatic && Available() > 0) {
        stopping_ = true;  // drain what was written, then stop (the callback finishes it)
        return;
      }
      MarkStoppedLocked();
    }
    AudioQueueStop(queue_, true);
  }

  void Flush() {
    {
      std::lock_guard<std::recursive_mutex> l(lock_);
      if (state_ == kPlaying) return;  // flush is ignored while playing
      read_ = write_ = 0;
      if (state_ == kStopped || state_ == kPaused) state_ = kFlushed;
      space_.notify_all();
    }
    AudioQueueReset(queue_);  // drops buffers still queued from before a pause
  }

  void SetVolume(float l, float r) { AudioQueueSetParameter(queue_, kAudioQueueParam_Volume, std::max(l, r)); }

  int Position() {
    std::lock_guard<std::recursive_mutex> l(lock_);
    return int(position_);
  }

  bool Timestamp(int64_t* frames, int64_t* ns) {
    std::lock_guard<std::recursive_mutex> l(lock_);
    if (stamp_ns_ == 0) return false;
    *frames = stamp_frames_;
    *ns = stamp_ns_;
    return true;
  }

  int SetPosition(int frames) {
    std::lock_guard<std::recursive_mutex> l(lock_);
    if (mode_ != kModeStatic || state_ == kPlaying) return kInvalidOperation;
    static_pos_ = std::min<int64_t>(frames, StaticFrames());
    position_ = static_pos_;
    return kSuccess;
  }

  int SetLoop(int start, int end, int count) {
    std::lock_guard<std::recursive_mutex> l(lock_);
    if (mode_ != kModeStatic) return kInvalidOperation;
    loop_start_ = start;
    loop_end_ = end;
    loop_count_ = count;
    return kSuccess;
  }

  int ReloadStatic() {
    std::lock_guard<std::recursive_mutex> l(lock_);
    if (mode_ != kModeStatic || state_ == kPlaying) return kInvalidOperation;
    static_pos_ = 0;
    position_ = 0;
    return kSuccess;
  }

  void SetMarker(int frames) {
    std::lock_guard<std::recursive_mutex> l(lock_);
    marker_ = frames;
    marker_reached_ = position_ >= frames && frames > 0;
  }
  int marker() const { return marker_; }

  void SetPeriod(int frames) {
    std::lock_guard<std::recursive_mutex> l(lock_);
    period_ = frames;
    next_period_ = position_ + frames;
  }
  int period() const { return period_; }

  int underruns() const { return underruns_; }

  // ---- data --------------------------------------------------------------------------------
  /// Writes up to `samples` interleaved source samples; returns the samples taken.
  int Write(const uint8_t* data, int samples, int format, bool blocking) {
    int bps = BytesPerSample(format);
    if (bps == 0) return kBadValue;
    int frames = samples / src_channels_;
    std::unique_lock<std::recursive_mutex> l(lock_);
    if (mode_ == kModeStatic) {
      int room = capacity_ - StaticFrames();
      int n = std::min(frames, room);
      for (int i = 0; i < n; i++) {
        const uint8_t* f = data + size_t(i) * src_channels_ * bps;
        for (int c = 0; c < out_channels_; c++) static_.push_back(SampleAt(f + c * bps, format));
      }
      return n * src_channels_;
    }
    int done = 0;
    while (done < frames) {
      int room = capacity_ - Available();
      if (room == 0) {
        if (!blocking || state_ != kPlaying) break;
        space_.wait(l);
        continue;
      }
      int n = std::min(frames - done, room);
      for (int i = 0; i < n; i++) {
        const uint8_t* f = data + size_t(done + i) * src_channels_ * bps;
        float* dst = &ring_[size_t(write_ % capacity_) * out_channels_];
        for (int c = 0; c < out_channels_; c++) dst[c] = SampleAt(f + c * bps, format);
        write_++;
      }
      done += n;
    }
    return done * src_channels_;
  }

 private:
  enum State { kStopped, kPlaying, kPaused, kFlushed };

  int Available() const { return int(write_ - read_); }
  int StaticFrames() const { return int(static_.size() / out_channels_); }

  void MarkStoppedLocked() {
    state_ = kStopped;
    stopping_ = false;
    read_ = write_ = 0;
  }

  /// Fills `b` from the app's data (silence for the rest), remembers how many app frames it
  /// carries (counted once it has been played) and enqueues it.
  void FillLocked(AudioQueueBufferRef b) {
    float* out = static_cast<float*>(b->mAudioData);
    int got = 0;
    if (state_ == kPlaying) {
      if (mode_ == kModeStatic) {
        int end = StaticFrames();
        while (got < buffer_frames_) {
          int stop = (loop_count_ != 0 && loop_end_ > loop_start_) ? std::min(loop_end_, end) : end;
          if (static_pos_ >= stop) {
            if (loop_count_ != 0 && loop_end_ > loop_start_) {
              static_pos_ = loop_start_;
              if (loop_count_ > 0) loop_count_--;
              continue;
            }
            break;
          }
          int n = std::min<int64_t>(buffer_frames_ - got, stop - static_pos_);
          memcpy(out + size_t(got) * out_channels_, &static_[size_t(static_pos_) * out_channels_],
                 size_t(n) * out_channels_ * sizeof(float));
          static_pos_ += n;
          got += n;
        }
      } else {
        int n = std::min(buffer_frames_, Available());
        for (int i = 0; i < n; i++) {
          const float* src = &ring_[size_t((read_ + i) % capacity_) * out_channels_];
          for (int c = 0; c < out_channels_; c++) out[size_t(i) * out_channels_ + c] = src[c];
        }
        read_ += n;
        got = n;
        if (n < buffer_frames_ && !stopping_ && n == 0) underruns_++;
        space_.notify_all();
      }
    }
    std::fill(out + size_t(got) * out_channels_, out + size_t(buffer_frames_) * out_channels_, 0.f);
    b->mAudioDataByteSize = buffer_frames_ * out_channels_ * sizeof(float);
    b->mUserData = reinterpret_cast<void*>(intptr_t(got));
    AudioQueueEnqueueBuffer(queue_, b, 0, nullptr);
  }

  static void Callback(void* self, AudioQueueRef, AudioQueueBufferRef b) {
    static_cast<Track*>(self)->OnBufferPlayed(b);
  }

  void OnBufferPlayed(AudioQueueBufferRef b) {
    int events[4];
    int nevents = 0, marker = 0, newpos = 0;
    {
      std::lock_guard<std::recursive_mutex> l(lock_);
      if (state_ != kPlaying) return;
      position_ += intptr_t(b->mUserData);
      stamp_frames_ = position_;
      stamp_ns_ = NowNs();
      if (marker_ > 0 && !marker_reached_ && position_ >= marker_) {
        marker_reached_ = true;
        events[nevents++] = kEventMarker;
        marker = marker_;
      }
      if (period_ > 0 && position_ >= next_period_) {
        while (next_period_ <= position_) next_period_ += period_;
        events[nevents++] = kEventNewPos;
        newpos = int(position_);
      }
      if (stopping_ && Available() == 0 && intptr_t(b->mUserData) == 0) {
        MarkStoppedLocked();
        AudioQueueStop(queue_, false);  // asynchronous: allowed inside the callback
      } else {
        FillLocked(b);
      }
    }
    if (nevents && weak_) {
      ScopedEnv env;
      for (int i = 0; i < nevents; i++) {
        env->CallStaticVoidMethod(gAudioTrackClass, gPostEvent, weak_, events[i],
                                  events[i] == kEventMarker ? marker : newpos, 0, nullptr);
        if (env->ExceptionCheck()) env->ExceptionClear();
      }
    }
  }

  jobject weak_ = nullptr;
  const int rate_, src_channels_, out_channels_, format_, mode_, capacity_;
  bool ok_ = false;
  AudioQueueRef queue_ = nullptr;
  AudioQueueBufferRef buffers_[kBuffers] = {};
  int buffer_frames_ = 0;

  std::recursive_mutex lock_;
  std::condition_variable_any space_;
  State state_ = kStopped;
  bool stopping_ = false;
  std::vector<float> ring_;
  int64_t read_ = 0, write_ = 0;
  std::vector<float> static_;
  int64_t static_pos_ = 0;
  int loop_start_ = 0, loop_end_ = 0, loop_count_ = 0;
  int64_t position_ = 0;
  int64_t stamp_frames_ = 0, stamp_ns_ = 0;
  int marker_ = 0;
  bool marker_reached_ = false;
  int period_ = 0;
  int64_t next_period_ = 0;
  std::atomic<int> underruns_{0};
};


// ---- "Now Playing" (MPNowPlayingInfoCenter) and remote commands -----------------------------
// dev.andropkg.system.MediaSessionImpl mirrors the app's active MediaSession here; the lock
// screen / Control Center / headphone / media-key commands go back to it as transport controls.
jclass gSessionClass;
jmethodID gHostCommand;  // static void hostCommand(String, long)

void HostCommand(const char* cmd, jlong arg = 0) {
  if (!gSessionClass) return;
  ScopedEnv env;
  jstring s = env->NewStringUTF(cmd);
  env->CallStaticVoidMethod(gSessionClass, gHostCommand, s, arg);
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(s);
}

NSString* NSStr(JNIEnv* env, jstring s) {
  if (!s) return nil;
  const char* c = env->GetStringUTFChars(s, nullptr);
  NSString* r = [NSString stringWithUTF8String:c];
  env->ReleaseStringUTFChars(s, c);
  return r;
}

void InstallRemoteCommands() {
  dispatch_async(dispatch_get_main_queue(), ^{
    MPRemoteCommandCenter* c = [MPRemoteCommandCenter sharedCommandCenter];
    [c.togglePlayPauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
      HostCommand("toggle");
      return MPRemoteCommandHandlerStatusSuccess;
    }];
    [c.playCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
      HostCommand("play");
      return MPRemoteCommandHandlerStatusSuccess;
    }];
    [c.pauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
      HostCommand("pause");
      return MPRemoteCommandHandlerStatusSuccess;
    }];
    [c.stopCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
      HostCommand("stop");
      return MPRemoteCommandHandlerStatusSuccess;
    }];
    [c.nextTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
      HostCommand("next");
      return MPRemoteCommandHandlerStatusSuccess;
    }];
    [c.previousTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
      HostCommand("previous");
      return MPRemoteCommandHandlerStatusSuccess;
    }];
    [c.changePlaybackPositionCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* e) {
      HostCommand("seek", jlong(static_cast<MPChangePlaybackPositionCommandEvent*>(e).positionTime * 1000));
      return MPRemoteCommandHandlerStatusSuccess;
    }];
  });
}

void np_update(JNIEnv* env, jclass, jstring title, jstring artist, jstring album, jlong duration, jlong position,
               jint state, jfloat speed, jlong actions, jbyteArray art) {
  NSMutableDictionary* info = [NSMutableDictionary dictionary];
  if (NSString* t = NSStr(env, title)) info[MPMediaItemPropertyTitle] = t;
  if (NSString* a = NSStr(env, artist)) info[MPMediaItemPropertyArtist] = a;
  if (NSString* a = NSStr(env, album)) info[MPMediaItemPropertyAlbumTitle] = a;
  if (duration > 0) info[MPMediaItemPropertyPlaybackDuration] = @(duration / 1000.0);
  info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(position / 1000.0);
  bool playing = state == 3;  // STATE_PLAYING
  info[MPNowPlayingInfoPropertyPlaybackRate] = @(playing ? (speed > 0 ? speed : 1.0) : 0.0);
  if (art) {
    jsize n = env->GetArrayLength(art);
    NSMutableData* d = [NSMutableData dataWithLength:size_t(n)];
    env->GetByteArrayRegion(art, 0, n, static_cast<jbyte*>(d.mutableBytes));
#if TARGET_OS_IPHONE
    UIImage* img = [UIImage imageWithData:d];
#else
    NSImage* img = [[NSImage alloc] initWithData:d];
#endif
    if (img) {
      info[MPMediaItemPropertyArtwork] = [[MPMediaItemArtwork alloc] initWithBoundsSize:img.size
                                                                         requestHandler:^(CGSize) { return img; }];
    }
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    MPNowPlayingInfoCenter* center = [MPNowPlayingInfoCenter defaultCenter];
    center.nowPlayingInfo = info;
    center.playbackState = playing ? MPNowPlayingPlaybackStatePlaying
                         : state == 2 ? MPNowPlayingPlaybackStatePaused : MPNowPlayingPlaybackStateStopped;
    MPRemoteCommandCenter* c = [MPRemoteCommandCenter sharedCommandCenter];
    c.nextTrackCommand.enabled = (actions & 32) != 0;      // ACTION_SKIP_TO_NEXT
    c.previousTrackCommand.enabled = (actions & 16) != 0;  // ACTION_SKIP_TO_PREVIOUS
    c.changePlaybackPositionCommand.enabled = (actions & 256) != 0;  // ACTION_SEEK_TO
  });
}

void np_clear(JNIEnv*, jclass) {
  dispatch_async(dispatch_get_main_queue(), ^{
    [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = nil;
    [MPNowPlayingInfoCenter defaultCenter].playbackState = MPNowPlayingPlaybackStateStopped;
  });
}

jclass LoadServicesClass(JNIEnv* env, const char* name) {
  jclass loaderClass = env->FindClass("java/lang/ClassLoader");
  jobject loader = env->CallStaticObjectMethod(
      loaderClass, env->GetStaticMethodID(loaderClass, "getSystemClassLoader", "()Ljava/lang/ClassLoader;"));
  jstring jname = env->NewStringUTF(name);
  auto c = static_cast<jclass>(env->CallObjectMethod(
      loader, env->GetMethodID(loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), jname));
  env->DeleteLocalRef(jname);
  env->DeleteLocalRef(loader);
  env->DeleteLocalRef(loaderClass);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return nullptr;
  }
  return c;
}

Track* Get(JNIEnv* env, jobject thiz) {
  return reinterpret_cast<Track*>(env->GetLongField(thiz, gNativeTrack));
}

int ChannelCount(int position_mask, int index_mask) {
  int n = __builtin_popcount(unsigned(position_mask));
  return n > 0 ? n : __builtin_popcount(unsigned(index_mask));
}

// ---- AudioTrack natives --------------------------------------------------------------------

extern "C" void andropkg_media_bind_services(JNIEnv* env);

jint at_setup(JNIEnv* env, jobject thiz, jobject weak, jobject /*attributes*/, jintArray jrate, jint position_mask,
              jint index_mask, jint format, jint buffer_bytes, jint mode, jintArray jsession, jobject /*attribution*/,
              jlong /*native_track*/, jboolean /*offload*/, jint, jobject, jstring) {
  andropkg_media_bind_services(env);
  int channels = ChannelCount(position_mask, index_mask);
  int bps = BytesPerSample(format);
  if (channels == 0) return -17;  // ERROR_SETUP_INVALIDCHANNELMASK
  if (bps == 0) return -18;       // ERROR_SETUP_INVALIDFORMAT
  jint rate = 0;
  env->GetIntArrayRegion(jrate, 0, 1, &rate);
  if (rate == 0) {
    rate = 48000;
    env->SetIntArrayRegion(jrate, 0, 1, &rate);
  }
  jint session = 0;
  env->GetIntArrayRegion(jsession, 0, 1, &session);
  if (session == 0) {
    static std::atomic<int> next{1};
    session = next++;
    env->SetIntArrayRegion(jsession, 0, 1, &session);
  }
  auto* t = new Track(env, weak, rate, channels, format, buffer_bytes / (channels * bps), mode);
  if (!t->ok()) {
    t->ReleaseRef(env);
    delete t;
    return -20;  // ERROR_NATIVESETUP_AUDIOSYSTEM
  }
  env->SetLongField(thiz, gNativeTrack, reinterpret_cast<jlong>(t));
  return kSuccess;
}

void at_release(JNIEnv* env, jobject thiz) {
  if (Track* t = Get(env, thiz)) {
    env->SetLongField(thiz, gNativeTrack, 0);
    t->Stop();
    t->ReleaseRef(env);
    delete t;
  }
}

void at_start(JNIEnv* env, jobject thiz) { if (Track* t = Get(env, thiz)) t->Start(); }
void at_stop(JNIEnv* env, jobject thiz) { if (Track* t = Get(env, thiz)) t->Stop(); }
void at_pause(JNIEnv* env, jobject thiz) { if (Track* t = Get(env, thiz)) t->Pause(); }
void at_flush(JNIEnv* env, jobject thiz) { if (Track* t = Get(env, thiz)) t->Flush(); }
void at_setVolume(JNIEnv* env, jobject thiz, jfloat l, jfloat r) { if (Track* t = Get(env, thiz)) t->SetVolume(l, r); }

template <typename Array, typename Elem>
jint WriteArray(JNIEnv* env, jobject thiz, Array data, jint offset, jint count, jint format, jboolean blocking, int elem_bytes) {
  Track* t = Get(env, thiz);
  if (t == nullptr) return kInvalidOperation;
  if (data == nullptr || offset < 0 || count < 0) return kBadValue;
  Elem* p = static_cast<Elem*>(env->GetPrimitiveArrayCritical(data, nullptr));
  if (p == nullptr) return kBadValue;
  // Copy out first: Write() may block, which must not happen inside a critical region.
  std::vector<uint8_t> copy(reinterpret_cast<uint8_t*>(p + offset), reinterpret_cast<uint8_t*>(p + offset) + size_t(count) * sizeof(Elem));
  env->ReleasePrimitiveArrayCritical(data, p, JNI_ABORT);
  int bps = BytesPerSample(format);
  if (bps == 0) return kBadValue;
  int samples = int(copy.size()) / bps;
  int taken = t->Write(copy.data(), samples, format, blocking);
  if (taken < 0) return taken;
  return int((int64_t(taken) * bps) / elem_bytes);
}

jint at_writeByte(JNIEnv* env, jobject thiz, jbyteArray data, jint off, jint size, jint format, jboolean blocking) {
  return WriteArray<jbyteArray, jbyte>(env, thiz, data, off, size, format, blocking, 1);
}
jint at_writeShort(JNIEnv* env, jobject thiz, jshortArray data, jint off, jint size, jint format, jboolean blocking) {
  return WriteArray<jshortArray, jshort>(env, thiz, data, off, size, format, blocking, 2);
}
jint at_writeFloat(JNIEnv* env, jobject thiz, jfloatArray data, jint off, jint size, jint format, jboolean blocking) {
  return WriteArray<jfloatArray, jfloat>(env, thiz, data, off, size, format, blocking, 4);
}
jint at_writeNative(JNIEnv* env, jobject thiz, jobject buffer, jint pos, jint size, jint format, jboolean blocking) {
  Track* t = Get(env, thiz);
  if (t == nullptr) return kInvalidOperation;
  auto* base = static_cast<uint8_t*>(env->GetDirectBufferAddress(buffer));
  int bps = BytesPerSample(format);
  if (base == nullptr || bps == 0) return kBadValue;
  int taken = t->Write(base + pos, size / bps, format, blocking);
  return taken < 0 ? taken : taken * bps;
}

jint at_getPosition(JNIEnv* env, jobject thiz) { Track* t = Get(env, thiz); return t ? t->Position() : 0; }
jint at_setPosition(JNIEnv* env, jobject thiz, jint f) { Track* t = Get(env, thiz); return t ? t->SetPosition(f) : kInvalidOperation; }
jint at_setLoop(JNIEnv* env, jobject thiz, jint s, jint e, jint c) { Track* t = Get(env, thiz); return t ? t->SetLoop(s, e, c) : kInvalidOperation; }
jint at_reloadStatic(JNIEnv* env, jobject thiz) { Track* t = Get(env, thiz); return t ? t->ReloadStatic() : kInvalidOperation; }
jint at_getTimestamp(JNIEnv* env, jobject thiz, jlongArray out) {
  Track* t = Get(env, thiz);
  int64_t v[2];
  if (t == nullptr || !t->Timestamp(&v[0], &v[1])) return kError;
  jlong j[2] = {v[0], v[1]};
  env->SetLongArrayRegion(out, 0, 2, j);
  return kSuccess;
}
jint at_getLatency(JNIEnv* env, jobject thiz) { Track* t = Get(env, thiz); return t ? t->latency_ms() : 0; }
jint at_capacity(JNIEnv* env, jobject thiz) { Track* t = Get(env, thiz); return t ? t->capacity() : 0; }
jint at_setBufferSize(JNIEnv* env, jobject thiz, jint frames) {
  Track* t = Get(env, thiz);
  return t ? std::min(frames, t->capacity()) : kInvalidOperation;
}
jint at_setMarker(JNIEnv* env, jobject thiz, jint f) { if (Track* t = Get(env, thiz)) t->SetMarker(f); return kSuccess; }
jint at_getMarker(JNIEnv* env, jobject thiz) { Track* t = Get(env, thiz); return t ? t->marker() : 0; }
jint at_setPeriod(JNIEnv* env, jobject thiz, jint f) { if (Track* t = Get(env, thiz)) t->SetPeriod(f); return kSuccess; }
jint at_getPeriod(JNIEnv* env, jobject thiz) { Track* t = Get(env, thiz); return t ? t->period() : 0; }
jint at_underruns(JNIEnv* env, jobject thiz) { Track* t = Get(env, thiz); return t ? t->underruns() : 0; }
jint at_outputRate(JNIEnv*, jclass, jint) { return 48000; }
jint at_minBufferSize(JNIEnv*, jclass, jint rate, jint channels, jint format) {
  int bps = BytesPerSample(format);
  if (rate <= 0 || channels <= 0 || bps == 0) return kBadValue;
  return std::max(rate / 25, 256) * channels * bps;  // 40 ms
}
jboolean at_directSupported(JNIEnv*, jclass, jint, jint, jint, jint, jint, jint, jint) { return JNI_FALSE; }

jobject DefaultPlaybackParams(JNIEnv* env) {
  jclass c = env->FindClass("android/media/PlaybackParams");
  jobject p = env->NewObject(c, env->GetMethodID(c, "<init>", "()V"));
  jmethodID setSpeed = env->GetMethodID(c, "setSpeed", "(F)Landroid/media/PlaybackParams;");
  jmethodID setPitch = env->GetMethodID(c, "setPitch", "(F)Landroid/media/PlaybackParams;");
  env->DeleteLocalRef(env->CallObjectMethod(p, setSpeed, 1.f));
  env->DeleteLocalRef(env->CallObjectMethod(p, setPitch, 1.f));
  env->DeleteLocalRef(c);
  return p;
}
jobject at_getPlaybackParams(JNIEnv* env, jobject) { return DefaultPlaybackParams(env); }

#define M(name, sig, fn) {const_cast<char*>(name), const_cast<char*>(sig), reinterpret_cast<void*>(fn)}
const JNINativeMethod kAudioTrack[] = {
    M("native_setup",
      "(Ljava/lang/Object;Ljava/lang/Object;[IIIIII[ILandroid/os/Parcel;JZILjava/lang/Object;Ljava/lang/String;)I",
      at_setup),
    M("native_release", "()V", at_release),
    M("native_finalize", "()V", at_release),
    M("native_start", "()V", at_start),
    M("native_stop", "()V", at_stop),
    M("native_pause", "()V", at_pause),
    M("native_flush", "()V", at_flush),
    M("native_setVolume", "(FF)V", at_setVolume),
    M("native_write_byte", "([BIIIZ)I", at_writeByte),
    M("native_write_short", "([SIIIZ)I", at_writeShort),
    M("native_write_float", "([FIIIZ)I", at_writeFloat),
    M("native_write_native_bytes", "(Ljava/nio/ByteBuffer;IIIZ)I", at_writeNative),
    M("native_get_position", "()I", at_getPosition),
    M("native_set_position", "(I)I", at_setPosition),
    M("native_set_loop", "(III)I", at_setLoop),
    M("native_reload_static", "()I", at_reloadStatic),
    M("native_get_timestamp", "([J)I", at_getTimestamp),
    M("native_get_latency", "()I", at_getLatency),
    M("native_get_buffer_capacity_frames", "()I", at_capacity),
    M("native_get_buffer_size_frames", "()I", at_capacity),
    M("native_set_buffer_size_frames", "(I)I", at_setBufferSize),
    M("native_set_marker_pos", "(I)I", at_setMarker),
    M("native_get_marker_pos", "()I", at_getMarker),
    M("native_set_pos_update_period", "(I)I", at_setPeriod),
    M("native_get_pos_update_period", "()I", at_getPeriod),
    M("native_get_underrun_count", "()I", at_underruns),
    M("native_get_output_sample_rate", "(I)I", at_outputRate),
    M("native_get_min_buff_size", "(III)I", at_minBufferSize),
    M("native_is_direct_output_supported", "(IIIIIII)Z", at_directSupported),
    M("native_get_playback_params", "()Landroid/media/PlaybackParams;", at_getPlaybackParams),
};

// ---- AudioSystem: a device with one speaker output ------------------------------------------
jint as_sampleRate(JNIEnv*, jclass) { return 48000; }
jint as_frameCount(JNIEnv*, jclass) { return 960; }
jint as_outputLatency(JNIEnv*, jclass, jint) { return 40; }
jint as_maxSampleRate(JNIEnv*, jclass) { return 192000; }
jint as_minSampleRate(JNIEnv*, jclass) { return 4000; }
jint as_maxChannels(JNIEnv*, jclass) { return 8; }
jfloat as_masterVolume(JNIEnv*, jclass) { return 1.f; }
std::atomic<int> gNextId{100};
jint as_newId(JNIEnv*, jclass) { return gNextId++; }
jint as_maxIndex(JNIEnv*, jclass, jobject) { return 15; }
jint as_volumeIndex(JNIEnv*, jclass, jobject, jint) { return 11; }
const JNINativeMethod kAudioSystem[] = {
    M("getPrimaryOutputSamplingRate", "()I", as_sampleRate),
    M("getPrimaryOutputFrameCount", "()I", as_frameCount),
    M("getOutputLatency", "(I)I", as_outputLatency),
    // Read once into AudioFormat/AudioSystem static finals (rate and channel limits).
    M("native_getMaxSampleRate", "()I", as_maxSampleRate),
    M("native_getMinSampleRate", "()I", as_minSampleRate),
    M("native_getMaxChannelCount", "()I", as_maxChannels),
    M("getMasterVolume", "()F", as_masterVolume),
    M("newAudioSessionId", "()I", as_newId),
    M("newAudioPlayerId", "()I", as_newId),
    M("newAudioRecorderId", "()I", as_newId),
    M("getMaxVolumeIndexForAttributes", "(Landroid/media/AudioAttributes;)I", as_maxIndex),
    M("getVolumeIndexForAttributes", "(Landroid/media/AudioAttributes;I)I", as_volumeIndex),
};
#undef M

}  // namespace

/// A remote media command as from the lock screen (tests: "mediacmd" in ANDROPKG_TEST_SCRIPT).
extern "C" __attribute__((visibility("default"))) void andropkg_media_command(const char* cmd, long long arg) {
  HostCommand(cmd, arg);
}

/// Binds MediaSessionImpl's host natives once andropkg's services are loadable (first window
/// or AudioTrack); a no-op outside andropkg.
extern "C" __attribute__((visibility("default"))) void andropkg_media_bind_services(JNIEnv* env) {
  static std::atomic<bool> done{false};
  if (done.load()) return;
  jclass c = LoadServicesClass(env, "dev.andropkg.system.MediaSessionImpl");
  if (c == nullptr) return;
  if (done.exchange(true)) return;
  static const JNINativeMethod kMethods[] = {
      {const_cast<char*>("nativeNowPlaying"),
       const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJIFJ[B)V"),
       reinterpret_cast<void*>(np_update)},
      {const_cast<char*>("nativeNowPlayingClear"), const_cast<char*>("()V"), reinterpret_cast<void*>(np_clear)},
  };
  if (env->RegisterNatives(c, kMethods, 2) != JNI_OK) {
    env->ExceptionClear();
    return;
  }
  gSessionClass = static_cast<jclass>(env->NewGlobalRef(c));
  gHostCommand = env->GetStaticMethodID(c, "hostCommand", "(Ljava/lang/String;J)V");
  InstallRemoteCommands();
  env->CallStaticVoidMethod(c, env->GetStaticMethodID(c, "onHostReady", "()V"));
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(c);
}

namespace android {
int register_andropkg_audio(JNIEnv* env) {
  env->GetJavaVM(&gVm);
  jclass at = env->FindClass("android/media/AudioTrack");
  if (at == nullptr) {
    env->ExceptionClear();
    return 0;
  }
  gAudioTrackClass = static_cast<jclass>(env->NewGlobalRef(at));
  gNativeTrack = env->GetFieldID(at, "mNativeTrackInJavaObj", "J");
  gPostEvent = env->GetStaticMethodID(at, "postEventFromNative", "(Ljava/lang/Object;IIILjava/lang/Object;)V");
  env->DeleteLocalRef(at);
  andropkg_media::RegisterNativeDefaults(env, "android/media/AudioTrack");
  RegisterDeclared(env, "android/media/AudioTrack", kAudioTrack, sizeof kAudioTrack / sizeof kAudioTrack[0]);
  andropkg_media::RegisterNativeDefaults(env, "android/media/AudioSystem");
  RegisterDeclared(env, "android/media/AudioSystem", kAudioSystem, sizeof kAudioSystem / sizeof kAudioSystem[0]);
  // The rest of libandroid_runtime's media natives: a device whose audio policy has no
  // strategies, groups or ports to report, no recorder input, no tone/JET synthesis yet.
  for (const char* c : {"android/media/AudioRecord", "android/media/audiopolicy/AudioProductStrategy",
                        "android/media/audiopolicy/AudioVolumeGroup",
                        "android/media/audiopolicy/AudioVolumeGroupChangeHandler", "android/media/AudioPortEventHandler",
                        "android/media/audio/common/AidlConversion", "android/media/MediaMetrics",
                        "android/media/ToneGenerator", "android/media/JetPlayer"}) {
    andropkg_media::RegisterNativeDefaults(env, c);
  }
  return 0;
}
}  // namespace android
