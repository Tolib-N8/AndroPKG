// andropkg: libmedia_jni — android.media.MediaPlayer on AVPlayer.
//
// Video: an AVPlayerItemVideoOutput (BGRA, Metal-compatible) is polled at 60 Hz and new frames
// are shown on the Surface given to setDisplay/setSurface (window_jni.cc, as MediaCodec does).
//
// Data sources: a FileDescriptor (MediaPlayer opens paths, assets and content: URIs itself
// and hands us the fd) is resolved to its host file with F_GETPATH; a sub-range of a file
// (e.g. a raw resource inside the APK) is copied out first. http(s) URLs go to AVPlayer as is.
// The content type is sniffed from the first bytes, since Android files rarely carry a usable
// extension. Events go back through MediaPlayer.postEventFromNative, like the real player's.
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "decoder.h"
#include "jni_defaults.h"

#include "andropkg/paths.h"

// file:// URLs name Android paths (/data/user/0/<pkg>/..., /sdcard/...): map them to the sandbox.
static NSURL* AndropkgHostURL(NSURL* url) {
  if (!url.isFileURL || !url.path) return url;
  char buf[1024];
  const char* t = andropkg_translate_path(url.path.fileSystemRepresentation, buf, sizeof buf);
  return [NSURL fileURLWithPath:@(t)];
}


namespace {
JavaVM* gVm;
jclass gMediaPlayerClass;
jfieldID gNativeContext;  // MediaPlayer.mNativeContext
jmethodID gPostEvent;     // static postEventFromNative(Object, int, int, int, Object)

enum {
  MEDIA_PREPARED = 1, MEDIA_PLAYBACK_COMPLETE = 2, MEDIA_SEEK_COMPLETE = 4, MEDIA_STARTED = 6,
  MEDIA_PAUSED = 7, MEDIA_STOPPED = 8, MEDIA_ERROR = 100, MEDIA_SET_VIDEO_SIZE = 5,
};
constexpr int kErrorUnknown = 1, kErrorIo = -1004, kErrorUnsupported = -1010;

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

/// MIME type from the first bytes, or nil if AVFoundation cannot play it.
NSString* SniffMime(const uint8_t* b, size_t n, bool* unsupported) {
  *unsupported = false;
  if (n >= 4 && !memcmp(b, "OggS", 4)) return @"audio/ogg";  // Vorbis/Opus: PcmBackend, not AVPlayer
  if (n >= 4 && !memcmp(b, "fLaC", 4)) return @"audio/flac";
  if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) return @"audio/wav";
  if (n >= 3 && !memcmp(b, "ID3", 3)) return @"audio/mpeg";
  if (n >= 12 && !memcmp(b + 4, "ftyp", 4)) {
    bool video = !memcmp(b + 8, "isom", 4) || !memcmp(b + 8, "mp41", 4) || !memcmp(b + 8, "mp42", 4) ||
                 !memcmp(b + 8, "avc1", 4) || !memcmp(b + 8, "qt  ", 4);
    return video ? @"video/mp4" : @"audio/mp4";
  }
  if (n >= 6 && !memcmp(b, "#!AMR\n", 6)) return @"audio/amr";
  if (n >= 4 && !memcmp(b, "FORM", 4)) return @"audio/aiff";
  if (n >= 4 && !memcmp(b, "caff", 4)) return @"audio/x-caf";
  if (n >= 4 && !memcmp(b, "\x1a\x45\xdf\xa3", 4)) { *unsupported = true; return nil; }  // Matroska/WebM
  if (n >= 2 && b[0] == 0xFF && (b[1] & 0xF6) == 0xF0) return @"audio/aac";  // ADTS
  if (n >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) return @"audio/mpeg";  // MPEG audio frame
  return nil;
}
}  // namespace


/// Playback of what AVPlayer can't play (Ogg Vorbis/Opus): a decoder feeding an AudioQueue.
class PcmBackend {
 public:
  using Ended = std::function<void()>;
  ~PcmBackend() {
    if (queue_) AudioQueueDispose(queue_, true);
  }

  bool Open(const char* path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    dec_ = andropkg_media::OpenDecoder(fd, 0, -1);
    close(fd);
    if (!dec_) return false;
    channels_ = std::min(dec_->channels(), 2);
    AudioStreamBasicDescription f = {};
    f.mSampleRate = dec_->rate();
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = UInt32(channels_);
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = f.mBytesPerPacket = UInt32(4 * channels_);
    f.mFramesPerPacket = 1;
    if (AudioQueueNewOutput(&f, &PcmBackend::Callback, this, nullptr, nullptr, 0, &queue_) != noErr) return false;
    buffer_frames_ = dec_->rate() / 10;  // 100 ms
    scratch_.resize(size_t(buffer_frames_) * dec_->channels());
    for (int i = 0; i < 3; i++) {
      AudioQueueBufferRef b;
      if (AudioQueueAllocateBuffer(queue_, UInt32(buffer_frames_ * 4 * channels_), &b) != noErr) return false;
      all_.push_back(b);
      free_.insert(b);
    }
    return true;
  }

  int64_t DurationMs() const { return dec_->frames() > 0 ? dec_->frames() * 1000 / dec_->rate() : -1; }
  int64_t PositionMs() {
    std::lock_guard<std::recursive_mutex> l(lock_);
    return (base_ + played_) * 1000 / dec_->rate();
  }
  void SetVolume(float v) { AudioQueueSetParameter(queue_, kAudioQueueParam_Volume, v); }
  void SetLooping(bool on) { looping_ = on; }
  void OnEnded(Ended e) { ended_ = std::move(e); }

  void Start() {
    {
      std::lock_guard<std::recursive_mutex> l(lock_);
      if (playing_) return;
      if (finished_) RewindLocked();
      playing_ = true;
      PrimeLocked();
    }
    AudioQueueStart(queue_, nullptr);
  }

  void Pause() {
    {
      std::lock_guard<std::recursive_mutex> l(lock_);
      if (!playing_) return;
      playing_ = false;
    }
    AudioQueuePause(queue_);
  }

  void Stop() {
    {
      std::lock_guard<std::recursive_mutex> l(lock_);
      playing_ = false;
    }
    Flush();
    std::lock_guard<std::recursive_mutex> l(lock_);
    RewindLocked();
  }

  void SeekMs(int64_t ms) {
    bool was_playing;
    {
      std::lock_guard<std::recursive_mutex> l(lock_);
      was_playing = playing_;
      playing_ = false;
    }
    Flush();
    {
      std::lock_guard<std::recursive_mutex> l(lock_);
      int64_t frame = std::max<int64_t>(0, ms * dec_->rate() / 1000);
      if (dec_->frames() > 0) frame = std::min(frame, dec_->frames());
      dec_->Seek(frame);
      base_ = frame;
      played_ = 0;
      eos_ = finished_ = false;
    }
    if (was_playing) Start();
  }

  bool playing() const { return playing_; }

 private:
  /// Stops the queue at once; every buffer is ours again and stale callbacks are ignored.
  void Flush() {
    AudioQueueStop(queue_, true);
    std::lock_guard<std::recursive_mutex> l(lock_);
    gen_++;
    free_.insert(all_.begin(), all_.end());
    meta_.clear();
  }

  void RewindLocked() {
    dec_->Seek(0);
    base_ = played_ = 0;
    eos_ = finished_ = false;
  }

  void PrimeLocked() {
    std::vector<AudioQueueBufferRef> bufs(free_.begin(), free_.end());
    for (auto b : bufs) FillLocked(b);
  }

  /// Decodes into `b` and enqueues it; false (buffer stays free) at the end of the stream.
  bool FillLocked(AudioQueueBufferRef b) {
    if (eos_) return false;
    int n = dec_->Read(scratch_.data(), buffer_frames_);
    if (n <= 0) {
      eos_ = true;
      return false;
    }
    float* out = static_cast<float*>(b->mAudioData);
    int src = dec_->channels();
    for (int i = 0; i < n; i++) {
      for (int c = 0; c < channels_; c++) out[i * channels_ + c] = scratch_[size_t(i) * src + c];
    }
    b->mAudioDataByteSize = UInt32(n * 4 * channels_);
    meta_[b] = {gen_, n};
    free_.erase(b);
    AudioQueueEnqueueBuffer(queue_, b, 0, nullptr);
    return true;
  }

  static void Callback(void* self, AudioQueueRef, AudioQueueBufferRef b) {
    static_cast<PcmBackend*>(self)->Played(b);
  }

  void Played(AudioQueueBufferRef b) {
    Ended notify;
    {
      std::lock_guard<std::recursive_mutex> l(lock_);
      auto it = meta_.find(b);
      if (it == meta_.end() || it->second.first != gen_) return;  // flushed
      played_ += it->second.second;
      meta_.erase(it);
      free_.insert(b);
      if (!playing_) return;
      FillLocked(b);
      if (eos_ && meta_.empty()) {  // everything decoded has been heard
        if (looping_) {
          RewindLocked();
          PrimeLocked();
          return;
        }
        playing_ = false;
        finished_ = true;
        notify = ended_;
      }
    }
    if (notify) {
      AudioQueueStop(queue_, false);
      notify();
    }
  }

  std::unique_ptr<andropkg_media::PcmDecoder> dec_;
  AudioQueueRef queue_ = nullptr;
  std::vector<AudioQueueBufferRef> all_;
  std::set<AudioQueueBufferRef> free_;
  std::map<AudioQueueBufferRef, std::pair<int, int>> meta_;  // generation, frames
  std::vector<float> scratch_;
  std::recursive_mutex lock_;
  int channels_ = 0, buffer_frames_ = 0, gen_ = 0;
  int64_t base_ = 0, played_ = 0;
  std::atomic<bool> playing_{false}, looping_{false};
  bool eos_ = false, finished_ = false;
  Ended ended_;
};

// window_jni.cc (libandroid_runtime): shows a decoded frame on a Surface (its mNativeObject).
static void PostToSurface(int64_t surface, CVPixelBufferRef frame) {
  using Post = void (*)(int64_t, void*);
  static Post post = reinterpret_cast<Post>(dlsym(RTLD_DEFAULT, "andropkg_surface_post_pixel_buffer"));
  if (post) post(surface, frame);
}

@interface AndropkgPlayer : NSObject {
 @public
  std::shared_ptr<PcmBackend> pcm_;  // set instead of `player` for Ogg
  jobject weak_;  // global ref to the WeakReference<MediaPlayer>
  std::atomic<int64_t> surface_;  // Surface.mNativeObject frames go to (0: none)
  dispatch_source_t videoTimer_;
}
@property(nonatomic, strong) AVPlayerItemVideoOutput* videoOutput;
@property(nonatomic) int videoWidth;
@property(nonatomic) int videoHeight;
@property(nonatomic, strong) NSURL* url;
@property(nonatomic, strong) NSString* mime;
@property(nonatomic, strong) NSString* tempPath;
@property(nonatomic, strong) AVPlayer* player;
@property(nonatomic, strong) AVPlayerItem* item;
@property(nonatomic, strong) id endObserver;
@property(nonatomic, strong) id timeObserver;
@property(nonatomic) BOOL ended;
@property(nonatomic) BOOL looping;
@property(nonatomic) BOOL playing;
@property(nonatomic) BOOL unsupported;
@property(nonatomic) float volume;
@property(nonatomic) float speed;
@property(nonatomic) int64_t durationMs;
@end

@implementation AndropkgPlayer
- (instancetype)init {
  if ((self = [super init])) {
    _volume = 1.f;
    _speed = 1.f;
    _durationMs = -1;
  }
  return self;
}

- (void)post:(int)what arg1:(int)a1 arg2:(int)a2 {
  if (!weak_) return;
  ScopedEnv env;
  env->CallStaticVoidMethod(gMediaPlayerClass, gPostEvent, weak_, what, a1, a2, nullptr);
  if (env->ExceptionCheck()) env->ExceptionClear();
}

+ (dispatch_queue_t)queue {
  static dispatch_queue_t q;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ q = dispatch_queue_create("andropkg.mediaplayer", DISPATCH_QUEUE_SERIAL); });
  return q;
}

- (void)teardown {
  if (videoTimer_) dispatch_source_cancel(videoTimer_);
  videoTimer_ = nil;
  _videoOutput = nil;
  if (pcm_) pcm_->Stop();
  pcm_.reset();
  if (_endObserver) [[NSNotificationCenter defaultCenter] removeObserver:_endObserver];
  _endObserver = nil;
  if (_timeObserver) [_player removeTimeObserver:_timeObserver];
  _timeObserver = nil;
  [_player pause];
  _player = nil;
  _item = nil;
  _playing = NO;
  if (_tempPath) unlink(_tempPath.fileSystemRepresentation);
  _tempPath = nil;
}

/// Loads the asset; on success creates the player. Returns 0 or a MEDIA_ERROR extra.
- (int)prepare {
  if (_unsupported) return kErrorUnsupported;
  if (!_url) return kErrorIo;
  if ([_mime isEqualToString:@"audio/ogg"]) {
    auto pcm = std::make_shared<PcmBackend>();
    if (!pcm->Open(_url.fileSystemRepresentation)) return kErrorUnsupported;
    pcm->SetVolume(_volume);
    pcm->SetLooping(_looping);
    __weak AndropkgPlayer* weakSelf = self;
    pcm->OnEnded([weakSelf] {
      AndropkgPlayer* me = weakSelf;
      if (!me) return;
      me.playing = NO;
      me.ended = YES;
      [me post:MEDIA_PLAYBACK_COMPLETE arg1:0 arg2:0];
    });
    _durationMs = pcm->DurationMs();
    pcm_ = pcm;
    return 0;
  }
  NSMutableDictionary* opts = [NSMutableDictionary dictionary];
  if (_mime) {
    if (@available(iOS 17.0, macOS 14.0, *)) opts[AVURLAssetOverrideMIMETypeKey] = _mime;
  }
  AVURLAsset* asset = [AVURLAsset URLAssetWithURL:_url options:opts];
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  [asset loadValuesAsynchronouslyForKeys:@[ @"playable", @"duration" ] completionHandler:^{
    dispatch_semaphore_signal(done);
  }];
  dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
  NSError* err = nil;
  if ([asset statusOfValueForKey:@"playable" error:&err] != AVKeyValueStatusLoaded || !asset.playable) {
    NSLog(@"andropkg: MediaPlayer cannot play %@: %@", _url, err);
    return err ? kErrorIo : kErrorUnsupported;
  }
  CMTime d = asset.duration;
  _durationMs = CMTIME_IS_NUMERIC(d) ? int64_t(CMTimeGetSeconds(d) * 1000) : -1;
  _item = [AVPlayerItem playerItemWithAsset:asset];
  [self setUpVideo:asset];
  _player = [AVPlayer playerWithPlayerItem:_item];
  _player.actionAtItemEnd = AVPlayerActionAtItemEndPause;
  _player.volume = _volume;
  __weak AndropkgPlayer* weakSelf = self;
  _endObserver = [[NSNotificationCenter defaultCenter]
      addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                  object:_item
                   queue:nil
              usingBlock:^(NSNotification*) { dispatch_async([AndropkgPlayer queue], ^{ [weakSelf itemEnded]; }); }];
  // The notification above comes through the main queue, which only runs where a main run
  // loop does (not under dalvikvm on macOS): also watch the clock from our own queue.
  _timeObserver = [_player addPeriodicTimeObserverForInterval:CMTimeMake(1, 20)
                                                        queue:[AndropkgPlayer queue]
                                                   usingBlock:^(CMTime t) {
    AndropkgPlayer* me = weakSelf;
    if (!me || !me.playing || me.durationMs <= 0) return;
    if (CMTimeGetSeconds(t) * 1000 >= me.durationMs - 1) [me itemEnded];
  }];
  return 0;
}

// Video tracks: size (as displayed, after the track's transform) and a pixel buffer output
// polled for new frames.
- (void)setUpVideo:(AVAsset*)asset {
  AVAssetTrack* track = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
  if (!track) return;
  CGSize size = CGSizeApplyAffineTransform(track.naturalSize, track.preferredTransform);
  _videoWidth = (int)fabs(size.width);
  _videoHeight = (int)fabs(size.height);
  NSDictionary* attrs = @{
    (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
    (id)kCVPixelBufferMetalCompatibilityKey : @YES,
    (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
  };
  _videoOutput = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:attrs];
  [_item addOutput:_videoOutput];
  videoTimer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, [AndropkgPlayer queue]);
  dispatch_source_set_timer(videoTimer_, DISPATCH_TIME_NOW, NSEC_PER_SEC / 60, NSEC_PER_SEC / 600);
  __weak AndropkgPlayer* weakSelf = self;
  dispatch_source_set_event_handler(videoTimer_, ^{ [weakSelf pollVideo]; });
  dispatch_resume(videoTimer_);
}

// On the player queue, 60 times a second.
- (void)pollVideo {
  int64_t surface = surface_.load();
  AVPlayerItemVideoOutput* out = _videoOutput;
  if (!surface || !out) return;
  CMTime t = [out itemTimeForHostTime:CACurrentMediaTime()];
  if (![out hasNewPixelBufferForItemTime:t]) return;
  CVPixelBufferRef pb = [out copyPixelBufferForItemTime:t itemTimeForDisplay:nullptr];
  if (!pb) return;
  PostToSurface(surface, pb);
  CVPixelBufferRelease(pb);
}

// On the player queue.
- (void)itemEnded {
  if (_ended || !_playing) return;
  if (_looping) {
    [_player seekToTime:kCMTimeZero];
    [_player playImmediatelyAtRate:_speed];
    return;
  }
  _ended = YES;
  _playing = NO;
  [self post:MEDIA_PLAYBACK_COMPLETE arg1:0 arg2:0];
}

- (void)start {
  ActivateAudioSession();
  if (pcm_) {
    _ended = NO;
    _playing = YES;
    pcm_->Start();
    [self post:MEDIA_STARTED arg1:0 arg2:0];
    return;
  }
  if (_ended) [_player seekToTime:kCMTimeZero];  // start() after completion replays
  _ended = NO;
  _playing = YES;
  [_player playImmediatelyAtRate:_speed];
  [self post:MEDIA_STARTED arg1:0 arg2:0];
}
@end

namespace {
void EnsurePlayerIds(JNIEnv* env);
AndropkgPlayer* Get(JNIEnv* env, jobject thiz) {
  EnsurePlayerIds(env);
  return (__bridge AndropkgPlayer*)reinterpret_cast<void*>(env->GetLongField(thiz, gNativeContext));
}

void Throw(JNIEnv* env, const char* cls, const char* msg) {
  if (jclass c = env->FindClass(cls)) env->ThrowNew(c, msg);
}

AndropkgPlayer* GetOrThrow(JNIEnv* env, jobject thiz) {
  AndropkgPlayer* p = Get(env, thiz);
  if (!p) Throw(env, "java/lang/IllegalStateException", nullptr);
  return p;
}

void mp_init(JNIEnv*, jclass) {}

// Field/method IDs are looked up on first use, not in JNI_OnLoad: GetFieldID/GetMethodID
// initialize the class, and MediaPlayer's static initializer loads this very library (another
// media class may be the one loading it).
void EnsurePlayerIds(JNIEnv* env) {
  if (gNativeContext) return;
  gNativeContext = env->GetFieldID(gMediaPlayerClass, "mNativeContext", "J");
  gPostEvent = env->GetStaticMethodID(gMediaPlayerClass, "postEventFromNative", "(Ljava/lang/Object;IIILjava/lang/Object;)V");
}

void mp_setup(JNIEnv* env, jobject thiz, jobject weak, jobject /*attribution*/, jint /*session*/) {
  EnsurePlayerIds(env);
  AndropkgPlayer* p = [[AndropkgPlayer alloc] init];
  p->weak_ = env->NewGlobalRef(weak);
  env->SetLongField(thiz, gNativeContext, reinterpret_cast<jlong>((__bridge_retained void*)p));
}

void mp_release(JNIEnv* env, jobject thiz) {
  jlong ctx = env->GetLongField(thiz, gNativeContext);
  if (!ctx) return;
  env->SetLongField(thiz, gNativeContext, 0);
  AndropkgPlayer* p = (__bridge_transfer AndropkgPlayer*)reinterpret_cast<void*>(ctx);
  [p teardown];
  if (p->weak_) env->DeleteGlobalRef(p->weak_);
  p->weak_ = nullptr;
}

void mp_reset(JNIEnv* env, jobject thiz) {
  if (AndropkgPlayer* p = Get(env, thiz)) {
    [p teardown];
    p.url = nil;
    p.mime = nil;
    p.unsupported = NO;
    p.durationMs = -1;
  }
}

struct Source {
  NSURL* url;
  NSString* mime;
  NSString* tempPath;  // a copied-out slice, deleted with the source
  bool unsupported = false;
};

/// The playable file behind (fd, offset, length); throws IOException and returns false on error.
bool ResolveFd(JNIEnv* env, jobject fdObj, jlong offset, jlong length, Source* out) {
  jclass fdc = env->FindClass("java/io/FileDescriptor");
  int fd = env->GetIntField(fdObj, env->GetFieldID(fdc, "descriptor", "I"));
  env->DeleteLocalRef(fdc);
  struct stat st;
  char path[MAXPATHLEN];
  if (fd < 0 || fstat(fd, &st) != 0) {
    Throw(env, "java/io/IOException", "bad file descriptor");
    return false;
  }
  if (offset >= st.st_size) {
    Throw(env, "java/io/IOException", "offset past the end of the file");
    return false;
  }
  length = std::min<jlong>(length, st.st_size - offset);
  uint8_t head[64];
  ssize_t n = pread(fd, head, sizeof head, offset);
  out->mime = SniffMime(head, n > 0 ? size_t(n) : 0, &out->unsupported);
  if (offset == 0 && length == st.st_size && fcntl(fd, F_GETPATH, path) == 0) {
    out->url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    return true;
  }
  // A slice of a larger file (a raw resource in the APK): copy it out.
  NSString* tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:
                                             [NSString stringWithFormat:@"andropkg-media-%@", [NSUUID UUID].UUIDString]];
  int dst = open(tmp.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  std::vector<uint8_t> buf(1 << 16);
  for (jlong done = 0; dst >= 0 && done < length;) {
    ssize_t r = pread(fd, buf.data(), size_t(std::min<jlong>(buf.size(), length - done)), offset + done);
    if (r <= 0) break;
    write(dst, buf.data(), size_t(r));
    done += r;
  }
  if (dst >= 0) close(dst);
  out->tempPath = tmp;
  out->url = [NSURL fileURLWithPath:tmp];
  return true;
}

AVURLAsset* AssetFor(NSURL* url, NSString* mime) {
  NSMutableDictionary* opts = [NSMutableDictionary dictionary];
  if (mime) {
    if (@available(iOS 17.0, macOS 14.0, *)) opts[AVURLAssetOverrideMIMETypeKey] = mime;
  }
  return [AVURLAsset URLAssetWithURL:url options:opts];
}

void mp_setDataSourceFd(JNIEnv* env, jobject thiz, jobject fdObj, jlong offset, jlong length) {
  AndropkgPlayer* p = GetOrThrow(env, thiz);
  Source src;
  if (!p || !ResolveFd(env, fdObj, offset, length, &src)) return;
  p.url = src.url;
  p.mime = src.mime;
  p.tempPath = src.tempPath;
  p.unsupported = src.unsupported;
}

void mp_setDataSourceUrl(JNIEnv* env, jobject thiz, jobject /*httpService*/, jstring jpath, jobjectArray, jobjectArray) {
  AndropkgPlayer* p = GetOrThrow(env, thiz);
  if (!p) return;
  const char* s = env->GetStringUTFChars(jpath, nullptr);
  NSString* str = [NSString stringWithUTF8String:s];
  env->ReleaseStringUTFChars(jpath, s);
  NSURL* url = AndropkgHostURL([NSURL URLWithString:str]);
  if (!url || !url.scheme) {
    Throw(env, "java/io/IOException", "unsupported data source");
    return;
  }
  p.url = url;
}

jint mp_prepare(JNIEnv* env, jobject thiz, jobject) {
  AndropkgPlayer* p = GetOrThrow(env, thiz);
  if (!p) return 0;
  if (int err = [p prepare]) {
    char msg[64];
    snprintf(msg, sizeof msg, "Prepare failed.: status=0x%x", err == kErrorUnsupported ? 0x80000000 : 1);
    Throw(env, "java/io/IOException", msg);
  } else if (p.videoOutput) {
    [p post:MEDIA_SET_VIDEO_SIZE arg1:p.videoWidth arg2:p.videoHeight];
  }
  return 0;
}

jint mp_prepareAsync(JNIEnv* env, jobject thiz, jobject) {
  AndropkgPlayer* p = GetOrThrow(env, thiz);
  if (!p) return 0;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    if (int err = [p prepare]) {
      [p post:MEDIA_ERROR arg1:kErrorUnknown arg2:err];
    } else {
      if (p.videoOutput) [p post:MEDIA_SET_VIDEO_SIZE arg1:p.videoWidth arg2:p.videoHeight];
      [p post:MEDIA_PREPARED arg1:0 arg2:0];
    }
  });
  return 0;
}

void mp_start(JNIEnv* env, jobject thiz) {
  AndropkgPlayer* p = GetOrThrow(env, thiz);
  if (p && (p.player || p->pcm_)) [p start];
}

void mp_pause(JNIEnv* env, jobject thiz) {
  AndropkgPlayer* p = GetOrThrow(env, thiz);
  if (p && p->pcm_) {
    p->pcm_->Pause();
    p.playing = NO;
    [p post:MEDIA_PAUSED arg1:0 arg2:0];
    return;
  }
  if (!p || !p.player) return;
  [p.player pause];
  p.playing = NO;
  [p post:MEDIA_PAUSED arg1:0 arg2:0];
}

void mp_stop(JNIEnv* env, jobject thiz) {
  AndropkgPlayer* p = GetOrThrow(env, thiz);
  if (p && p->pcm_) {
    p->pcm_->Stop();
    p.playing = NO;
    [p post:MEDIA_STOPPED arg1:0 arg2:0];
    return;
  }
  if (!p || !p.player) return;
  [p.player pause];
  [p.player seekToTime:kCMTimeZero];
  p.playing = NO;
  [p post:MEDIA_STOPPED arg1:0 arg2:0];
}

void mp_seekTo(JNIEnv* env, jobject thiz, jlong ms, jint mode) {
  AndropkgPlayer* p = GetOrThrow(env, thiz);
  if (p && p->pcm_) {
    std::shared_ptr<PcmBackend> pcm = p->pcm_;
    p.ended = NO;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      pcm->SeekMs(ms);
      [p post:MEDIA_SEEK_COMPLETE arg1:0 arg2:0];
    });
    return;
  }
  if (!p || !p.player) return;
  // SEEK_CLOSEST (3) is exact; the sync-frame modes may land anywhere nearby.
  CMTime tol = mode == 3 ? kCMTimeZero : CMTimeMakeWithSeconds(0.5, 1000);
  [p.player seekToTime:CMTimeMakeWithSeconds(ms / 1000.0, 1000)
       toleranceBefore:tol
        toleranceAfter:tol
     completionHandler:^(BOOL) { [p post:MEDIA_SEEK_COMPLETE arg1:0 arg2:0]; }];
  p.ended = NO;
}

jboolean mp_isPlaying(JNIEnv* env, jobject thiz) {
  AndropkgPlayer* p = GetOrThrow(env, thiz);
  return p && p.playing;
}

jint mp_getCurrentPosition(JNIEnv* env, jobject thiz) {
  AndropkgPlayer* p = GetOrThrow(env, thiz);
  if (p && p->pcm_) return jint(p->pcm_->PositionMs());
  if (!p || !p.player) return 0;
  CMTime t = p.player.currentTime;
  return CMTIME_IS_NUMERIC(t) ? jint(CMTimeGetSeconds(t) * 1000) : 0;
}

jint mp_getDuration(JNIEnv* env, jobject thiz) {
  AndropkgPlayer* p = GetOrThrow(env, thiz);
  return p ? jint(p.durationMs) : -1;
}

void mp_setVolume(JNIEnv* env, jobject thiz, jfloat l, jfloat r) {
  if (AndropkgPlayer* p = GetOrThrow(env, thiz)) {
    p.volume = std::max(l, r);
    p.player.volume = p.volume;
    if (p->pcm_) p->pcm_->SetVolume(p.volume);
  }
}

void mp_setLooping(JNIEnv* env, jobject thiz, jboolean on) {
  if (AndropkgPlayer* p = GetOrThrow(env, thiz)) {
    p.looping = on;
    if (p->pcm_) p->pcm_->SetLooping(on);
  }
}

jboolean mp_isLooping(JNIEnv* env, jobject thiz) {
  AndropkgPlayer* p = GetOrThrow(env, thiz);
  return p && p.looping;
}

void mp_setPlaybackParams(JNIEnv* env, jobject thiz, jobject params) {
  AndropkgPlayer* p = GetOrThrow(env, thiz);
  if (!p || !params) return;
  jclass c = env->GetObjectClass(params);
  jint set = env->GetIntField(params, env->GetFieldID(c, "mSet", "I"));
  if (set & 1 /* SET_SPEED */) p.speed = env->GetFloatField(params, env->GetFieldID(c, "mSpeed", "F"));
  env->DeleteLocalRef(c);
  if (p.playing) p.player.rate = p.speed;
}

jobject mp_getPlaybackParams(JNIEnv* env, jobject thiz) {
  AndropkgPlayer* p = GetOrThrow(env, thiz);
  jclass c = env->FindClass("android/media/PlaybackParams");
  jobject o = env->NewObject(c, env->GetMethodID(c, "<init>", "()V"));
  jmethodID setSpeed = env->GetMethodID(c, "setSpeed", "(F)Landroid/media/PlaybackParams;");
  jmethodID setPitch = env->GetMethodID(c, "setPitch", "(F)Landroid/media/PlaybackParams;");
  env->DeleteLocalRef(env->CallObjectMethod(o, setSpeed, p ? p.speed : 1.f));
  env->DeleteLocalRef(env->CallObjectMethod(o, setPitch, 1.f));
  env->DeleteLocalRef(c);
  return o;
}

// invoke(): track queries and selection. No in-band tracks: an empty list for every request.
jint mp_invoke(JNIEnv* env, jobject, jobject /*request*/, jobject reply) {
  jclass c = env->GetObjectClass(reply);
  env->CallVoidMethod(reply, env->GetMethodID(c, "writeInt", "(I)V"), 0);
  env->DeleteLocalRef(c);
  return 0;
}

jint mp_getAudioStreamType(JNIEnv*, jobject) { return 3; /* STREAM_MUSIC */ }

jfieldID gSurfaceNative;  // android.view.Surface.mNativeObject
void mp_setVideoSurface(JNIEnv* env, jobject thiz, jobject surface) {
  AndropkgPlayer* p = Get(env, thiz);
  if (!p) return;
  if (surface && !gSurfaceNative) {
    jclass c = env->FindClass("android/view/Surface");
    gSurfaceNative = env->GetFieldID(c, "mNativeObject", "J");
    env->DeleteLocalRef(c);
  }
  p->surface_ = surface ? env->GetLongField(surface, gSurfaceNative) : 0;
}
jint mp_getVideoWidth(JNIEnv* env, jobject thiz) {
  AndropkgPlayer* p = Get(env, thiz);
  return p ? p.videoWidth : 0;
}
jint mp_getVideoHeight(JNIEnv* env, jobject thiz) {
  AndropkgPlayer* p = Get(env, thiz);
  return p ? p.videoHeight : 0;
}
jint mp_getAudioSessionId(JNIEnv*, jobject) { return 1; }

#define M(name, sig, fn) {const_cast<char*>(name), const_cast<char*>(sig), reinterpret_cast<void*>(fn)}
const JNINativeMethod kMediaPlayer[] = {
    M("native_init", "()V", mp_init),
    M("native_setup", "(Ljava/lang/Object;Landroid/os/Parcel;I)V", mp_setup),
    M("_release", "()V", mp_release),
    M("native_finalize", "()V", mp_release),
    M("_reset", "()V", mp_reset),
    M("_setDataSource", "(Ljava/io/FileDescriptor;JJ)V", mp_setDataSourceFd),
    M("nativeSetDataSource", "(Landroid/os/IBinder;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V",
      mp_setDataSourceUrl),
    M("_prepare", "(Landroid/os/Parcel;)I", mp_prepare),
    M("_prepareAsync", "(Landroid/os/Parcel;)I", mp_prepareAsync),
    M("_start", "()V", mp_start),
    M("_pause", "()V", mp_pause),
    M("_stop", "()V", mp_stop),
    M("_seekTo", "(JI)V", mp_seekTo),
    M("isPlaying", "()Z", mp_isPlaying),
    M("getCurrentPosition", "()I", mp_getCurrentPosition),
    M("getDuration", "()I", mp_getDuration),
    M("_setVolume", "(FF)V", mp_setVolume),
    M("setLooping", "(Z)V", mp_setLooping),
    M("isLooping", "()Z", mp_isLooping),
    M("setPlaybackParams", "(Landroid/media/PlaybackParams;)V", mp_setPlaybackParams),
    M("getPlaybackParams", "()Landroid/media/PlaybackParams;", mp_getPlaybackParams),
    M("native_invoke", "(Landroid/os/Parcel;Landroid/os/Parcel;)I", mp_invoke),
    M("_getAudioStreamType", "()I", mp_getAudioStreamType),
    M("getAudioSessionId", "()I", mp_getAudioSessionId),
    M("_setVideoSurface", "(Landroid/view/Surface;)V", mp_setVideoSurface),
    M("getVideoWidth", "()I", mp_getVideoWidth),
    M("getVideoHeight", "()I", mp_getVideoHeight),
};
#undef M
}  // namespace


// ---- MediaMetadataRetriever on AVAsset metadata ------------------------------------------------
@interface AndropkgRetriever : NSObject
@property(nonatomic, strong) AVURLAsset* asset;
@property(nonatomic, strong) NSString* mime;
@property(nonatomic, strong) NSString* tempPath;
@property(nonatomic) int64_t oggDurationMs;
@property(nonatomic) int oggRate;
@end
@implementation AndropkgRetriever
- (void)clear {
  if (_tempPath) unlink(_tempPath.fileSystemRepresentation);
  _tempPath = nil;
  _asset = nil;
}
- (void)dealloc {
  [self clear];
}
@end

namespace {
jfieldID gRetrieverContext;  // MediaMetadataRetriever.mNativeContext (looked up in native_setup)
jclass gRetrieverClass;

AndropkgRetriever* GetRetriever(JNIEnv* env, jobject thiz) {
  if (!gRetrieverContext) gRetrieverContext = env->GetFieldID(gRetrieverClass, "mNativeContext", "J");
  return (__bridge AndropkgRetriever*)reinterpret_cast<void*>(env->GetLongField(thiz, gRetrieverContext));
}

void mr_setup(JNIEnv* env, jobject thiz) {
  if (!gRetrieverContext) gRetrieverContext = env->GetFieldID(gRetrieverClass, "mNativeContext", "J");
  AndropkgRetriever* r = [[AndropkgRetriever alloc] init];
  env->SetLongField(thiz, gRetrieverContext, reinterpret_cast<jlong>((__bridge_retained void*)r));
}

void mr_release(JNIEnv* env, jobject thiz) {
  jlong ctx = env->GetLongField(thiz, gRetrieverContext);
  if (!ctx) return;
  env->SetLongField(thiz, gRetrieverContext, 0);
  AndropkgRetriever* r = (__bridge_transfer AndropkgRetriever*)reinterpret_cast<void*>(ctx);
  [r clear];
}

void LoadAsset(AndropkgRetriever* r, NSURL* url, NSString* mime) {
  AVURLAsset* a = AssetFor(url, mime);
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  [a loadValuesAsynchronouslyForKeys:@[ @"metadata", @"commonMetadata", @"duration", @"tracks" ]
                   completionHandler:^{ dispatch_semaphore_signal(done); }];
  dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
  r.asset = a;
  r.mime = mime;
}

void mr_setDataSourceFd(JNIEnv* env, jobject thiz, jobject fdObj, jlong offset, jlong length) {
  AndropkgRetriever* r = GetRetriever(env, thiz);
  Source src;
  if (!r || !ResolveFd(env, fdObj, offset, length, &src)) return;
  [r clear];
  r.tempPath = src.tempPath;
  if ([src.mime isEqualToString:@"audio/ogg"]) {  // AVFoundation can't read it: the decoder can
    int fd = open(src.url.fileSystemRepresentation, O_RDONLY | O_CLOEXEC);
    auto dec = fd >= 0 ? andropkg_media::OpenDecoder(fd, 0, -1) : nullptr;
    if (fd >= 0) close(fd);
    if (!dec) {
      Throw(env, "java/lang/IllegalArgumentException", "unsupported format");
      return;
    }
    r.mime = src.mime;
    r.oggDurationMs = dec->frames() > 0 ? dec->frames() * 1000 / dec->rate() : -1;
    r.oggRate = dec->rate();
    return;
  }
  if (src.unsupported) {
    Throw(env, "java/lang/IllegalArgumentException", "unsupported format");
    return;
  }
  LoadAsset(r, src.url, src.mime);
}

void mr_setDataSourceUrl(JNIEnv* env, jobject thiz, jobject, jstring jpath, jobjectArray, jobjectArray) {
  AndropkgRetriever* r = GetRetriever(env, thiz);
  const char* s = env->GetStringUTFChars(jpath, nullptr);
  NSURL* url = AndropkgHostURL([NSURL URLWithString:[NSString stringWithUTF8String:s]]);
  env->ReleaseStringUTFChars(jpath, s);
  if (!r || !url.scheme) {
    Throw(env, "java/lang/IllegalArgumentException", "unsupported data source");
    return;
  }
  [r clear];
  LoadAsset(r, url, nil);
}

AVMetadataItem* Find(NSArray<AVMetadataItem*>* items, NSArray<NSString*>* identifiers) {
  for (NSString* ident in identifiers) {
    NSArray* hits = [AVMetadataItem metadataItemsFromArray:items filteredByIdentifier:ident];
    if (hits.count) return hits.firstObject;
  }
  return nil;
}

NSString* StringOf(AVMetadataItem* item) {
  if (!item) return nil;
  if (item.stringValue.length) return item.stringValue;
  if (item.numberValue) return item.numberValue.stringValue;
  if (item.dateValue) {
    NSDateComponents* c = [[NSCalendar currentCalendar] components:NSCalendarUnitYear fromDate:item.dateValue];
    return [NSString stringWithFormat:@"%ld", (long)c.year];
  }
  return nil;
}

/// iTunes trkn/disk atoms: [0 0 | n (BE16) | total (BE16) | ...] -> "n/total".
NSString* PairOf(AVMetadataItem* item) {
  if (!item) return nil;
  NSData* d = item.dataValue;
  if (d.length >= 6 && !item.stringValue.length) {
    const uint8_t* b = static_cast<const uint8_t*>(d.bytes);
    int n = b[2] << 8 | b[3], total = b[4] << 8 | b[5];
    return total ? [NSString stringWithFormat:@"%d/%d", n, total] : [NSString stringWithFormat:@"%d", n];
  }
  return StringOf(item);
}

jstring mr_extract(JNIEnv* env, jobject thiz, jint key) {
  AndropkgRetriever* r = GetRetriever(env, thiz);
  AVURLAsset* a = r.asset;
  if (!a && r.oggRate > 0) {
    NSString* v = nil;
    switch (key) {
      case 9: v = r.oggDurationMs >= 0 ? [NSString stringWithFormat:@"%lld", (long long)r.oggDurationMs] : nil; break;
      case 12: v = r.mime; break;
      case 16: v = @"yes"; break;
      case 38: v = [NSString stringWithFormat:@"%d", r.oggRate]; break;
    }
    return v ? env->NewStringUTF(v.UTF8String) : nullptr;
  }
  if (!a) return nullptr;
  NSArray<AVMetadataItem*>* all = [a.commonMetadata arrayByAddingObjectsFromArray:a.metadata];
  NSArray<AVAssetTrack*>* audio = [a tracksWithMediaType:AVMediaTypeAudio];
  NSArray<AVAssetTrack*>* video = [a tracksWithMediaType:AVMediaTypeVideo];
  NSString* v = nil;
  switch (key) {
    case 7:  // TITLE
      v = StringOf(Find(all, @[ AVMetadataCommonIdentifierTitle, AVMetadataIdentifierID3MetadataTitleDescription ]));
      break;
    case 2:  // ARTIST
      v = StringOf(Find(all, @[ AVMetadataCommonIdentifierArtist, AVMetadataIdentifierID3MetadataLeadPerformer ]));
      break;
    case 1:  // ALBUM
      v = StringOf(Find(all, @[ AVMetadataCommonIdentifierAlbumName, AVMetadataIdentifierID3MetadataAlbumTitle ]));
      break;
    case 13:  // ALBUMARTIST
      v = StringOf(Find(all, @[ AVMetadataIdentifieriTunesMetadataAlbumArtist, AVMetadataIdentifierID3MetadataBand ]));
      break;
    case 3:  // AUTHOR
      v = StringOf(Find(all, @[ AVMetadataCommonIdentifierAuthor ]));
      break;
    case 4:  // COMPOSER
      v = StringOf(Find(all, @[ AVMetadataIdentifieriTunesMetadataComposer, AVMetadataIdentifierID3MetadataComposer ]));
      break;
    case 6:  // GENRE
      v = StringOf(Find(all, @[ AVMetadataIdentifieriTunesMetadataUserGenre, AVMetadataIdentifierID3MetadataContentType,
                                AVMetadataIdentifierQuickTimeMetadataGenre ]));
      break;
    case 8:  // YEAR
    case 5:  // DATE
      v = StringOf(Find(all, @[ AVMetadataIdentifieriTunesMetadataReleaseDate, AVMetadataIdentifierID3MetadataYear,
                                AVMetadataIdentifierID3MetadataRecordingTime, AVMetadataCommonIdentifierCreationDate ]));
      if (key == 8 && v.length > 4) v = [v substringToIndex:4];
      break;
    case 0:  // CD_TRACK_NUMBER
      v = PairOf(Find(all, @[ AVMetadataIdentifieriTunesMetadataTrackNumber, AVMetadataIdentifierID3MetadataTrackNumber ]));
      break;
    case 14:  // DISC_NUMBER
      v = PairOf(Find(all, @[ AVMetadataIdentifieriTunesMetadataDiscNumber, AVMetadataIdentifierID3MetadataPartOfASet ]));
      break;
    case 15:  // COMPILATION
      v = StringOf(Find(all, @[ AVMetadataIdentifieriTunesMetadataDiscCompilation ]));
      break;
    case 11:  // WRITER
      v = StringOf(Find(all, @[ AVMetadataIdentifierID3MetadataLyricist ]));
      break;
    case 9: {  // DURATION (ms)
      CMTime d = a.duration;
      if (CMTIME_IS_NUMERIC(d)) v = [NSString stringWithFormat:@"%lld", (long long)(CMTimeGetSeconds(d) * 1000)];
      break;
    }
    case 10: v = [NSString stringWithFormat:@"%lu", (unsigned long)a.tracks.count]; break;  // NUM_TRACKS
    case 12: v = r.mime; break;                                                            // MIMETYPE
    case 16: v = audio.count ? @"yes" : nil; break;                                       // HAS_AUDIO
    case 17: v = video.count ? @"yes" : nil; break;                                       // HAS_VIDEO
    case 18:  // VIDEO_WIDTH
    case 19:  // VIDEO_HEIGHT
      if (video.count) {
        CGSize sz = video.firstObject.naturalSize;
        v = [NSString stringWithFormat:@"%d", int(key == 18 ? sz.width : sz.height)];
      }
      break;
    case 20:  // BITRATE (bit/s)
      if (audio.count || video.count) {
        float bps = 0;
        for (AVAssetTrack* t in a.tracks) bps += t.estimatedDataRate;
        v = [NSString stringWithFormat:@"%d", int(bps)];
      }
      break;
    case 38:  // SAMPLERATE
      if (audio.count) {
        CMFormatDescriptionRef f = (__bridge CMFormatDescriptionRef)audio.firstObject.formatDescriptions.firstObject;
        const AudioStreamBasicDescription* asbd = f ? CMAudioFormatDescriptionGetStreamBasicDescription(f) : nullptr;
        if (asbd) v = [NSString stringWithFormat:@"%d", int(asbd->mSampleRate)];
      }
      break;
  }
  return v ? env->NewStringUTF(v.UTF8String) : nullptr;
}

jbyteArray mr_picture(JNIEnv* env, jobject thiz, jint) {
  AndropkgRetriever* r = GetRetriever(env, thiz);
  if (!r.asset) return nullptr;
  NSArray<AVMetadataItem*>* all = [r.asset.commonMetadata arrayByAddingObjectsFromArray:r.asset.metadata];
  AVMetadataItem* art = Find(all, @[ AVMetadataCommonIdentifierArtwork, AVMetadataIdentifierID3MetadataAttachedPicture,
                                     AVMetadataIdentifieriTunesMetadataCoverArt ]);
  NSData* d = art.dataValue;
  if (!d.length) return nullptr;
  jbyteArray out = env->NewByteArray(jsize(d.length));
  env->SetByteArrayRegion(out, 0, jsize(d.length), static_cast<const jbyte*>(d.bytes));
  return out;
}

#define M(name, sig, fn) {const_cast<char*>(name), const_cast<char*>(sig), reinterpret_cast<void*>(fn)}
const JNINativeMethod kRetriever[] = {
    M("native_setup", "()V", mr_setup),
    M("release", "()V", mr_release),
    M("native_finalize", "()V", mr_release),
    M("_setDataSource", "(Ljava/io/FileDescriptor;JJ)V", mr_setDataSourceFd),
    M("_setDataSource", "(Landroid/os/IBinder;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V",
      mr_setDataSourceUrl),
    M("nativeExtractMetadata", "(I)Ljava/lang/String;", mr_extract),
    M("getEmbeddedPicture", "(I)[B", mr_picture),
};
#undef M
}  // namespace

namespace andropkg_media {
void RegisterCodecs(JNIEnv* env);  // media_codec.mm
}

extern "C" __attribute__((visibility("default"))) jint JNI_OnLoad(JavaVM* vm, void*) {
  gVm = vm;
  JNIEnv* env = nullptr;
  vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  jclass mp = env->FindClass("android/media/MediaPlayer");
  if (mp == nullptr) {
    env->ExceptionClear();
    return JNI_VERSION_1_6;
  }
  gMediaPlayerClass = static_cast<jclass>(env->NewGlobalRef(mp));
  env->DeleteLocalRef(mp);
  // Everything else libmedia_jni would register (recorder, codecs, extractor, ...) answers
  // like a device without that component until it is implemented.
  for (const char* c : {"android/media/MediaPlayer", "android/media/MediaRecorder", "android/media/MediaMetadataRetriever",
                        "android/media/MediaExtractor", "android/media/MediaCodec", "android/media/MediaCodecList",
                        "android/media/MediaMuxer", "android/media/MediaSync", "android/media/MediaScanner",
                        "android/media/MediaDrm", "android/media/MediaCrypto", "android/media/ImageReader",
                        "android/media/ImageWriter", "android/media/MediaDescrambler", "android/media/MediaHTTPConnection",
                        "android/media/ResampleInputStream", "android/media/MediaProfiles", "android/media/Utils",
                        "android/mtp/MtpDatabase", "android/mtp/MtpDevice", "android/mtp/MtpServer"}) {
    andropkg_media::RegisterNativeDefaults(env, c);
  }
  andropkg_media::RegisterDeclared(env, "android/media/MediaPlayer", kMediaPlayer, sizeof kMediaPlayer / sizeof kMediaPlayer[0]);
  andropkg_media::RegisterCodecs(env);  // MediaCodec, MediaCodecList, MediaExtractor (media_codec.mm)
  if (jclass mr = env->FindClass("android/media/MediaMetadataRetriever")) {
    gRetrieverClass = static_cast<jclass>(env->NewGlobalRef(mr));
    env->DeleteLocalRef(mr);
    andropkg_media::RegisterDeclared(env, "android/media/MediaMetadataRetriever", kRetriever,
                                     sizeof kRetriever / sizeof kRetriever[0]);
  }
  return JNI_VERSION_1_6;
}
