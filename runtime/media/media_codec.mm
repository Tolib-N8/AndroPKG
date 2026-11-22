// andropkg: android.media.MediaCodecList / MediaExtractor / MediaCodec — audio.
//
// Decoders (software, named like the platform's so apps' heuristics hold):
//   c2.android.vorbis.decoder  Tremolo's DSP API (csd-0 identification, csd-1 setup headers;
//                              each packet ends with Android's 4-byte page-sample count)
//   c2.android.opus.decoder    libopus (csd-0 OpusHead; csd-1 codec delay in ns)
//   c2.android.aac.decoder     Core Audio's AudioConverter (csd-0 AudioSpecificConfig -> esds)
//   c2.android.mp3.decoder     AudioConverter
//   c2.android.raw.decoder     PCM passthrough
// They decode an access unit when it is queued, into 16-bit (or float, per pcm-encoding) PCM.
// Synchronous (dequeue*) and asynchronous (setCallback: events through postEventFromNative)
// modes. No encoders, no secure codecs.
//
// Video (VideoToolbox; VideoDecoder below):
//   c2.android.avc.decoder     H.264, Annex-B access units; SPS/PPS from csd-0/csd-1 or in-band
//   c2.android.hevc.decoder    H.265, VPS/SPS/PPS likewise
// Frames come out in presentation order as CVPixelBuffers (BGRA, Metal-compatible); with an
// output Surface, releaseOutputBuffer(render) shows one on it (window_jni.cc); ByteBuffer mode
// returns empty buffers.
//
// The extractor demuxes Ogg (Vorbis, Opus) itself, MP4/M4A/AAC/MP3 through Core Audio's
// AudioFile (compressed packets), and serves anything else ExtAudioFile reads (WAV, FLAC,
// AIFF, CAF, ALAC...) as an audio/raw track decoded on the fly.
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <VideoToolbox/VideoToolbox.h>

#include <dlfcn.h>
#include <fcntl.h>
#include "andropkg/paths.h"
#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "decoder.h"
#include "jni_defaults.h"
#include "opus.h"

extern "C" {
#include "ivorbiscodec.h"
#include "codec_internal.h"
}

namespace {

using Bytes = std::vector<uint8_t>;

// ---- JNI helpers -------------------------------------------------------------------------
void Throw(JNIEnv* env, const char* cls, const std::string& msg) {
  if (env->ExceptionCheck()) return;
  if (jclass c = env->FindClass(cls)) env->ThrowNew(c, msg.c_str());
}

// ANDROPKG_LOG_MEDIA=1: log MediaCodec / MediaExtractor calls.
bool MediaTrace() {
  static const bool on = getenv("ANDROPKG_LOG_MEDIA") != nullptr;
  return on;
}
void MediaLog(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  fprintf(stderr, "andropkg-media I %s\n", buf);
}
#define MLOG(...) do { if (MediaTrace()) MediaLog(__VA_ARGS__); } while (0)

std::string Str(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string r(c ? c : "");
  if (c) env->ReleaseStringUTFChars(s, c);
  return r;
}

jobject NewInteger(JNIEnv* env, int v) {
  jclass c = env->FindClass("java/lang/Integer");
  jobject o = env->CallStaticObjectMethod(c, env->GetStaticMethodID(c, "valueOf", "(I)Ljava/lang/Integer;"), v);
  env->DeleteLocalRef(c);
  return o;
}
jobject NewLong(JNIEnv* env, int64_t v) {
  jclass c = env->FindClass("java/lang/Long");
  jobject o = env->CallStaticObjectMethod(c, env->GetStaticMethodID(c, "valueOf", "(J)Ljava/lang/Long;"), (jlong)v);
  env->DeleteLocalRef(c);
  return o;
}
jobject NewDirect(JNIEnv* env, const Bytes& b) {  // a copy in a Java-heap direct buffer
  jclass c = env->FindClass("java/nio/ByteBuffer");
  jobject buf = env->CallStaticObjectMethod(c, env->GetStaticMethodID(c, "allocateDirect", "(I)Ljava/nio/ByteBuffer;"),
                                            (jint)b.size());
  if (buf && !b.empty()) memcpy(env->GetDirectBufferAddress(buf), b.data(), b.size());
  env->DeleteLocalRef(c);
  return buf;
}

/// A java.util.HashMap<String, Object> (MediaFormat's map).
class JMap {
 public:
  explicit JMap(JNIEnv* env) : env_(env) {
    jclass c = env->FindClass("java/util/HashMap");
    map_ = env->NewObject(c, env->GetMethodID(c, "<init>", "()V"));
    put_ = env->GetMethodID(c, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    env->DeleteLocalRef(c);
  }
  void Put(const char* k, jobject v) {
    jstring key = env_->NewStringUTF(k);
    jobject old = env_->CallObjectMethod(map_, put_, key, v);
    if (old) env_->DeleteLocalRef(old);
    env_->DeleteLocalRef(key);
    env_->DeleteLocalRef(v);
  }
  void PutInt(const char* k, int v) { Put(k, NewInteger(env_, v)); }
  void PutLong(const char* k, int64_t v) { Put(k, NewLong(env_, v)); }
  void PutString(const char* k, const std::string& v) { Put(k, env_->NewStringUTF(v.c_str())); }
  void PutBuffer(const char* k, const Bytes& v) { Put(k, NewDirect(env_, v)); }
  jobject get() const { return map_; }

 private:
  JNIEnv* env_;
  jobject map_;
  jmethodID put_;
};

/// MediaFormat keys/values (as native_configure / setParameters pass them).
struct Format {
  std::map<std::string, int64_t> ints;
  std::map<std::string, std::string> strings;
  std::map<std::string, Bytes> buffers;

  static Format From(JNIEnv* env, jobjectArray keys, jobjectArray values) {
    Format f;
    jclass integer = env->FindClass("java/lang/Integer"), lng = env->FindClass("java/lang/Long");
    jclass flt = env->FindClass("java/lang/Float"), str = env->FindClass("java/lang/String");
    jclass bb = env->FindClass("java/nio/ByteBuffer");
    jmethodID intValue = env->GetMethodID(integer, "intValue", "()I");
    jmethodID longValue = env->GetMethodID(lng, "longValue", "()J");
    jmethodID floatValue = env->GetMethodID(flt, "floatValue", "()F");
    jmethodID position = env->GetMethodID(bb, "position", "()I"), limit = env->GetMethodID(bb, "limit", "()I");
    for (jsize i = 0, n = keys ? env->GetArrayLength(keys) : 0; i < n; i++) {
      jstring k = static_cast<jstring>(env->GetObjectArrayElement(keys, i));
      jobject v = env->GetObjectArrayElement(values, i);
      std::string key = Str(env, k);
      if (!v) {
      } else if (env->IsInstanceOf(v, integer)) {
        f.ints[key] = env->CallIntMethod(v, intValue);
      } else if (env->IsInstanceOf(v, lng)) {
        f.ints[key] = env->CallLongMethod(v, longValue);
      } else if (env->IsInstanceOf(v, flt)) {
        f.ints[key] = (int64_t)env->CallFloatMethod(v, floatValue);
      } else if (env->IsInstanceOf(v, str)) {
        f.strings[key] = Str(env, static_cast<jstring>(v));
      } else if (env->IsInstanceOf(v, bb)) {
        int pos = env->CallIntMethod(v, position), lim = env->CallIntMethod(v, limit);
        Bytes data(std::max(0, lim - pos));
        if (auto* p = static_cast<uint8_t*>(env->GetDirectBufferAddress(v))) {
          memcpy(data.data(), p + pos, data.size());
        } else {  // a heap buffer: through its array
          jmethodID array = env->GetMethodID(bb, "array", "()[B");
          jmethodID arrayOffset = env->GetMethodID(bb, "arrayOffset", "()I");
          jbyteArray a = static_cast<jbyteArray>(env->CallObjectMethod(v, array));
          int off = env->CallIntMethod(v, arrayOffset);
          if (a) env->GetByteArrayRegion(a, off + pos, (jsize)data.size(), reinterpret_cast<jbyte*>(data.data()));
          env->ExceptionClear();
        }
        f.buffers[key] = std::move(data);
      }
      env->DeleteLocalRef(k);
      if (v) env->DeleteLocalRef(v);
    }
    return f;
  }
  int64_t Int(const char* k, int64_t def) const {
    auto it = ints.find(k);
    return it == ints.end() ? def : it->second;
  }
  std::string String(const char* k) const {
    auto it = strings.find(k);
    return it == strings.end() ? std::string() : it->second;
  }
  const Bytes* Buffer(const char* k) const {
    auto it = buffers.find(k);
    return it == buffers.end() ? nullptr : &it->second;
  }
};

// ---- the codecs ------------------------------------------------------------------------------
struct CodecDesc {
  const char* name;
  const char* mime;
};
const CodecDesc kCodecs[] = {
    {"c2.android.aac.decoder", "audio/mp4a-latm"}, {"c2.android.mp3.decoder", "audio/mpeg"},
    {"c2.android.vorbis.decoder", "audio/vorbis"}, {"c2.android.opus.decoder", "audio/opus"},
    {"c2.android.raw.decoder", "audio/raw"},
    {"c2.android.avc.decoder", "video/avc"}, {"c2.android.hevc.decoder", "video/hevc"},
};
constexpr int kFirstVideoCodec = 5;
bool IsVideo(int desc) { return desc >= kFirstVideoCodec; }
constexpr int kNumCodecs = sizeof kCodecs / sizeof *kCodecs;

int CodecByName(const std::string& name) {
  for (int i = 0; i < kNumCodecs; i++) {
    if (name == kCodecs[i].name) return i;
    if (name == std::string(kCodecs[i].name).replace(0, 10, "OMX.google.")) return i;  // old names
  }
  return -1;
}
int CodecByMime(const std::string& mime) {
  for (int i = 0; i < kNumCodecs; i++) if (strcasecmp(mime.c_str(), kCodecs[i].mime) == 0) return i;
  return -1;
}

/// One codec instance's decoding: an access unit in, interleaved float PCM out.
class Decoder {
 public:
  virtual ~Decoder() = default;
  virtual bool Init(const Format& f) = 0;
  /// Appends the PCM of one input access unit (may be empty, e.g. headers).
  virtual bool Decode(const uint8_t* data, size_t size, std::vector<float>* out) = 0;
  virtual void Flush() {}
  int rate = 0, channels = 0;
};

// Vorbis: Tremolo's DSP layer, fed packets as ogg_references.
class VorbisDecoder : public Decoder {
 public:
  ~VorbisDecoder() override {
    if (dsp_) vorbis_dsp_destroy(dsp_);
    vorbis_comment_clear(&vc_);
    vorbis_info_clear(&vi_);
  }
  bool Init(const Format& f) override {
    vorbis_info_init(&vi_);
    vorbis_comment_init(&vc_);
    const Bytes* id = f.Buffer("csd-0");
    const Bytes* setup = f.Buffer("csd-1");
    if (!id || !setup) return false;
    // identification (b_o_s), an empty comment header (Tremolo requires one), setup
    static const uint8_t kComment[] = {3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (Header(*id, true) != 0 || Header(Bytes(kComment, kComment + sizeof kComment), false) != 0 ||
        Header(*setup, false) != 0) {
      return false;
    }
    dsp_ = vorbis_dsp_create(&vi_);
    rate = (int)vi_.rate;
    channels = vi_.channels;
    return dsp_ != nullptr;
  }
  bool Decode(const uint8_t* data, size_t size, std::vector<float>* out) override {
    if (size >= 4) size -= 4;  // Android's trailing page-sample count
    if (size == 0) return true;
    ogg_buffer buf{};
    ogg_reference ref{};
    ogg_packet pack{};
    Wrap(data, size, &buf, &ref, &pack);
    pack.packetno = packetno_++;
    if (vorbis_dsp_synthesis(dsp_, &pack, 1) != 0) return true;  // a bad packet: skip it
    int16_t pcm[4096 * 8];
    int n;
    while ((n = vorbis_dsp_pcmout(dsp_, pcm, 4096)) > 0) {
      for (int i = 0; i < n * channels; i++) out->push_back(pcm[i] / 32768.f);
      vorbis_dsp_read(dsp_, n);
    }
    return true;
  }
  void Flush() override {
    if (dsp_) vorbis_dsp_restart(dsp_);
  }

 private:
  static void Wrap(const uint8_t* data, size_t size, ogg_buffer* buf, ogg_reference* ref, ogg_packet* pack) {
    buf->data = const_cast<uint8_t*>(data);
    buf->size = (long)size;
    buf->refcount = 1;
    ref->buffer = buf;
    ref->begin = 0;
    ref->length = (long)size;
    ref->next = nullptr;
    pack->packet = ref;
    pack->bytes = (long)size;
  }
  int Header(const Bytes& h, bool bos) {
    ogg_buffer buf{};
    ogg_reference ref{};
    ogg_packet pack{};
    Wrap(h.data(), h.size(), &buf, &ref, &pack);
    pack.b_o_s = bos;
    return vorbis_dsp_headerin(&vi_, &vc_, &pack);
  }
  vorbis_info vi_{};
  vorbis_comment vc_{};
  vorbis_dsp_state* dsp_ = nullptr;
  int64_t packetno_ = 3;
};

// Opus: libopus at 48 kHz; the codec delay (pre-skip) is dropped from the start.
class OpusDecoderImpl : public Decoder {
 public:
  ~OpusDecoderImpl() override {
    if (dec_) opus_decoder_destroy(dec_);
  }
  bool Init(const Format& f) override {
    const Bytes* head = f.Buffer("csd-0");
    channels = (int)f.Int("channel-count", 2);
    int preskip = 0;
    if (head && head->size() >= 19 && !memcmp(head->data(), "OpusHead", 8)) {
      channels = (*head)[9];
      preskip = (*head)[10] | ((*head)[11] << 8);
    }
    if (const Bytes* delay = f.Buffer("csd-1"); delay && delay->size() == 8) {
      int64_t ns;
      memcpy(&ns, delay->data(), 8);
      preskip = (int)(ns * 48000 / 1000000000LL);
    }
    if (channels < 1 || channels > 2) channels = 2;  // (multistream: not yet)
    rate = 48000;
    skip_ = initialSkip_ = preskip;
    int err = 0;
    dec_ = opus_decoder_create(48000, channels, &err);
    return dec_ && err == OPUS_OK;
  }
  bool Decode(const uint8_t* data, size_t size, std::vector<float>* out) override {
    if (size == 0) return true;
    if (size >= 8 && !memcmp(data, "OpusHead", 8)) return true;  // (headers in-band)
    if (size >= 8 && !memcmp(data, "OpusTags", 8)) return true;
    float pcm[5760 * 2];
    int n = opus_decode_float(dec_, data, (opus_int32)size, pcm, 5760, 0);
    if (n <= 0) return true;
    int start = std::min(skip_, n);
    skip_ -= start;
    out->insert(out->end(), pcm + start * channels, pcm + n * channels);
    return true;
  }
  void Flush() override {
    opus_decoder_ctl(dec_, OPUS_RESET_STATE);
    skip_ = 0;  // (a seek: the stream's preroll is the app's business)
  }

 private:
  OpusDecoder* dec_ = nullptr;
  int skip_ = 0, initialSkip_ = 0;
};

// AAC / MP3: an AudioConverter fed one packet at a time.
class ConverterDecoder : public Decoder {
 public:
  explicit ConverterDecoder(bool aac) : aac_(aac) {}
  ~ConverterDecoder() override {
    if (conv_) AudioConverterDispose(conv_);
  }
  bool Init(const Format& f) override {
    rate = (int)f.Int("sample-rate", 44100);
    channels = (int)f.Int("channel-count", 2);
    AudioStreamBasicDescription in{};
    in.mFormatID = aac_ ? kAudioFormatMPEG4AAC : kAudioFormatMPEGLayer3;
    in.mFramesPerPacket = aac_ ? 1024 : 1152;
    Bytes cookie;
    if (aac_) {
      const Bytes* asc = f.Buffer("csd-0");
      Bytes defaultAsc;
      if (!asc || asc->size() < 2) {  // AAC-LC at the format's rate/channels
        static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
        int idx = 4;
        for (int i = 0; i < 12; i++) if (kRates[i] == rate) idx = i;
        defaultAsc = {(uint8_t)((2 << 3) | (idx >> 1)), (uint8_t)(((idx & 1) << 7) | (channels << 3))};
        asc = &defaultAsc;
      }
      int aot = (*asc)[0] >> 3;
      int idx = (((*asc)[0] & 7) << 1) | ((*asc)[1] >> 7);
      static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
      if (idx < 13) rate = kRates[idx];
      int ch = ((*asc)[1] >> 3) & 15;
      if (ch > 0 && ch < 8) channels = ch == 7 ? 8 : ch;
      in.mFormatFlags = aot == 5 || aot == 29 ? kMPEG4Object_AAC_LC : (UInt32)aot;  // SBR/PS: the cookie says so
      cookie = Esds(*asc);
    }
    in.mSampleRate = rate;
    in.mChannelsPerFrame = (UInt32)channels;
    AudioStreamBasicDescription out{};
    out.mSampleRate = rate;
    out.mFormatID = kAudioFormatLinearPCM;
    out.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    out.mChannelsPerFrame = (UInt32)channels;
    out.mBitsPerChannel = 32;
    out.mBytesPerFrame = out.mBytesPerPacket = 4 * (UInt32)channels;
    out.mFramesPerPacket = 1;
    if (!cookie.empty()) {  // let Core Audio complete the input format (HE-AAC's real rate)
      UInt32 sz = sizeof in;
      AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, (UInt32)cookie.size(), cookie.data(), &sz, &in);
      if (in.mSampleRate > 0) rate = (int)in.mSampleRate, out.mSampleRate = in.mSampleRate;
    }
    if (AudioConverterNew(&in, &out, &conv_) != noErr) return false;
    if (!cookie.empty())
      AudioConverterSetProperty(conv_, kAudioConverterDecompressionMagicCookie, (UInt32)cookie.size(), cookie.data());
    return true;
  }
  bool Decode(const uint8_t* data, size_t size, std::vector<float>* out) override {
    if (size == 0) return true;
    Feed feed{data, size, false, {}};
    feed.desc.mDataByteSize = (UInt32)size;
    std::vector<float> pcm(4096 * (size_t)channels);
    for (;;) {
      AudioBufferList list{};
      list.mNumberBuffers = 1;
      list.mBuffers[0].mNumberChannels = (UInt32)channels;
      list.mBuffers[0].mDataByteSize = (UInt32)(pcm.size() * sizeof(float));
      list.mBuffers[0].mData = pcm.data();
      UInt32 frames = 4096;
      OSStatus st = AudioConverterFillComplexBuffer(conv_, Input, &feed, &frames, &list, nullptr);
      out->insert(out->end(), pcm.begin(), pcm.begin() + frames * channels);
      if (st != noErr || frames == 0) break;  // (kNoMoreInput: this packet is consumed)
    }
    return true;
  }
  void Flush() override { AudioConverterReset(conv_); }

 private:
  struct Feed {
    const uint8_t* data;
    size_t size;
    bool given;
    AudioStreamPacketDescription desc;
  };
  static constexpr OSStatus kNoMoreInput = 'nomo';
  static OSStatus Input(AudioConverterRef, UInt32* packets, AudioBufferList* list,
                        AudioStreamPacketDescription** descs, void* user) {
    auto* f = static_cast<Feed*>(user);
    if (f->given) {
      *packets = 0;
      return kNoMoreInput;
    }
    f->given = true;
    list->mBuffers[0].mData = const_cast<uint8_t*>(f->data);
    list->mBuffers[0].mDataByteSize = (UInt32)f->size;
    if (descs) *descs = &f->desc;
    *packets = 1;
    return noErr;
  }
  // An MPEG-4 ES descriptor around the AudioSpecificConfig: Core Audio's AAC cookie.
  static Bytes Esds(const Bytes& asc) {
    auto desc = [](uint8_t tag, const Bytes& body) {
      Bytes d{tag, 0x80, 0x80, 0x80, (uint8_t)body.size()};
      d.insert(d.end(), body.begin(), body.end());
      return d;
    };
    Bytes dsi = desc(5, asc);
    Bytes dcd{0x40, 0x15, 0, 0x18, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // AAC, audio stream, sizes 0
    dcd.insert(dcd.end(), dsi.begin(), dsi.end());
    Bytes es{0, 0, 0};  // ES_ID, flags
    Bytes d = desc(4, dcd);
    es.insert(es.end(), d.begin(), d.end());
    Bytes sl = desc(6, Bytes{2});
    es.insert(es.end(), sl.begin(), sl.end());
    return desc(3, es);
  }
  bool aac_;
  AudioConverterRef conv_ = nullptr;
};

// PCM in (16-bit, float or 8-bit per pcm-encoding).
class RawDecoder : public Decoder {
 public:
  bool Init(const Format& f) override {
    rate = (int)f.Int("sample-rate", 44100);
    channels = (int)f.Int("channel-count", 2);
    encoding_ = (int)f.Int("pcm-encoding", 2);
    return true;
  }
  bool Decode(const uint8_t* data, size_t size, std::vector<float>* out) override {
    if (encoding_ == 4) {
      const float* p = reinterpret_cast<const float*>(data);
      out->insert(out->end(), p, p + size / 4);
    } else if (encoding_ == 3) {
      for (size_t i = 0; i < size; i++) out->push_back((data[i] - 128) / 128.f);
    } else {
      const int16_t* p = reinterpret_cast<const int16_t*>(data);
      for (size_t i = 0; i < size / 2; i++) out->push_back(p[i] / 32768.f);
    }
    return true;
  }

 private:
  int encoding_ = 2;
};

std::unique_ptr<Decoder> MakeDecoder(int codec) {
  switch (codec) {
    case 0: return std::make_unique<ConverterDecoder>(true);
    case 1: return std::make_unique<ConverterDecoder>(false);
    case 2: return std::make_unique<VorbisDecoder>();
    case 3: return std::make_unique<OpusDecoderImpl>();
    case 4: return std::make_unique<RawDecoder>();
  }
  return nullptr;
}

// ---- video: VideoToolbox ------------------------------------------------------------------
// Annex-B access units in (start codes 00 00 01 / 00 00 00 01), CVPixelBuffers out in
// presentation order. VideoToolbox emits frames in decode order, so up to kReorder are held
// back and released lowest-pts first (all of them at end of stream).
class VideoDecoder {
 public:
  explicit VideoDecoder(bool hevc) : hevc_(hevc) {}
  ~VideoDecoder() {
    Teardown();
    for (auto& f : frames_) CVPixelBufferRelease(f.second);
  }

  bool Init(const Format& f) {
    width = (int)f.Int("width", 0);
    height = (int)f.Int("height", 0);
    for (const char* k : {"csd-0", "csd-1", "csd-2"}) {
      if (const Bytes* b = f.Buffer(k)) ScanParameterSets(b->data(), b->size());
    }
    return true;
  }

  // One access unit. Parameter sets in it (or config-only buffers) update the session.
  bool Decode(const uint8_t* data, size_t size, int64_t ptsUs) {
    @autoreleasepool {  // (called on app threads, which have no autorelease pool to drain)
      return DecodeUnit(data, size, ptsUs);
    }
  }

  bool DecodeUnit(const uint8_t* data, size_t size, int64_t ptsUs) {
    std::vector<std::pair<const uint8_t*, size_t>> nals = SplitNals(data, size);
    Bytes avcc;
    for (auto& [p, n] : nals) {
      if (n == 0) continue;
      int type = hevc_ ? (p[0] >> 1) & 0x3f : p[0] & 0x1f;
      if (IsParameterSet(type)) {
        Remember(type, p, n);
        continue;
      }
      if (hevc_ ? type == 35 : type == 9) continue;  // access unit delimiter
      uint32_t len = CFSwapInt32HostToBig((uint32_t)n);
      avcc.insert(avcc.end(), reinterpret_cast<uint8_t*>(&len), reinterpret_cast<uint8_t*>(&len) + 4);
      avcc.insert(avcc.end(), p, p + n);
    }
    if (avcc.empty()) return true;
    if (!session_ && !CreateSession()) return false;
    CMBlockBufferRef block = nullptr;
    void* mem = malloc(avcc.size());
    memcpy(mem, avcc.data(), avcc.size());
    if (CMBlockBufferCreateWithMemoryBlock(nullptr, mem, avcc.size(), kCFAllocatorMalloc, nullptr, 0, avcc.size(), 0,
                                           &block) != noErr) {
      free(mem);
      return false;
    }
    CMSampleTimingInfo timing = {kCMTimeInvalid, CMTimeMake(ptsUs, 1000000), kCMTimeInvalid};
    size_t sampleSize = avcc.size();
    CMSampleBufferRef sample = nullptr;
    OSStatus st = CMSampleBufferCreateReady(nullptr, block, format_, 1, 1, &timing, 1, &sampleSize, &sample);
    CFRelease(block);
    if (st != noErr) return false;
    VTDecodeInfoFlags info = 0;
    st = VTDecompressionSessionDecodeFrame(session_, sample, 0, nullptr, &info);  // synchronous
    CFRelease(sample);
    if (st == kVTInvalidSessionErr) {  // e.g. after the app went to the background: start over
      Teardown();
      return true;
    }
    return st == noErr;
  }

  // Next frame in presentation order (retained), if one may go out.
  bool Take(CVPixelBufferRef* frame, int64_t* ptsUs) {
    if (frames_.empty() || (!draining_ && frames_.size() <= kReorder)) return false;
    auto it = frames_.begin();
    *ptsUs = it->first;
    *frame = it->second;
    frames_.erase(it);
    return true;
  }
  void Drain() { draining_ = true; }
  bool Drained() const { return draining_ && frames_.empty(); }
  void Flush() {
    if (session_) VTDecompressionSessionWaitForAsynchronousFrames(session_);
    for (auto& f : frames_) CVPixelBufferRelease(f.second);
    frames_.clear();
    draining_ = false;
  }

  int width = 0, height = 0;
  bool sizeChanged = false;

 private:
  static constexpr size_t kReorder = 4;

  bool IsParameterSet(int type) const { return hevc_ ? (type >= 32 && type <= 34) : (type == 7 || type == 8); }

  static std::vector<std::pair<const uint8_t*, size_t>> SplitNals(const uint8_t* d, size_t n) {
    std::vector<std::pair<const uint8_t*, size_t>> out;
    size_t i = 0, start = SIZE_MAX;
    while (i + 3 <= n) {
      if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
        if (start != SIZE_MAX) {
          size_t end = i;
          while (end > start && d[end - 1] == 0) end--;  // trailing zeros of a 4-byte start code
          out.push_back({d + start, end - start});
        }
        i += 3;
        start = i;
      } else {
        i++;
      }
    }
    if (start != SIZE_MAX && start < n) out.push_back({d + start, n - start});
    if (out.empty() && n > 4) {  // not Annex-B: length-prefixed (AVCC/HVCC)
      size_t p = 0;
      while (p + 4 <= n) {
        size_t len = (size_t)d[p] << 24 | d[p + 1] << 16 | d[p + 2] << 8 | d[p + 3];
        if (len == 0 || p + 4 + len > n) break;
        out.push_back({d + p + 4, len});
        p += 4 + len;
      }
    }
    return out;
  }

  void ScanParameterSets(const uint8_t* d, size_t n) {
    for (auto& [p, len] : SplitNals(d, n)) {
      if (len == 0) continue;
      int type = hevc_ ? (p[0] >> 1) & 0x3f : p[0] & 0x1f;
      if (IsParameterSet(type)) Remember(type, p, len);
    }
  }

  void Remember(int type, const uint8_t* p, size_t n) {
    Bytes& slot = hevc_ ? (type == 32 ? vps_ : type == 33 ? sps_ : pps_) : (type == 7 ? sps_ : pps_);
    if (slot.size() == n && memcmp(slot.data(), p, n) == 0) return;
    slot.assign(p, p + n);
    Teardown();  // new parameters: a new format description and session
  }

  bool CreateSession() {
    if (sps_.empty() || pps_.empty() || (hevc_ && vps_.empty())) return false;
    OSStatus st;
    if (hevc_) {
      const uint8_t* sets[] = {vps_.data(), sps_.data(), pps_.data()};
      size_t sizes[] = {vps_.size(), sps_.size(), pps_.size()};
      st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(nullptr, 3, sets, sizes, 4, nullptr, &format_);
    } else {
      const uint8_t* sets[] = {sps_.data(), pps_.data()};
      size_t sizes[] = {sps_.size(), pps_.size()};
      st = CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, 2, sets, sizes, 4, &format_);
    }
    if (st != noErr) return false;
    CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(format_);
    if (dim.width != width || dim.height != height) {
      width = dim.width;
      height = dim.height;
      sizeChanged = true;
    }
    NSDictionary* attrs = @{
      (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
      (id)kCVPixelBufferMetalCompatibilityKey : @YES,
      (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };
    VTDecompressionOutputCallbackRecord cb = {&VideoDecoder::Output, this};
    st = VTDecompressionSessionCreate(nullptr, format_, nullptr, (__bridge CFDictionaryRef)attrs, &cb, &session_);
    return st == noErr;
  }

  void Teardown() {
    if (session_) {
      VTDecompressionSessionWaitForAsynchronousFrames(session_);
      VTDecompressionSessionInvalidate(session_);
      CFRelease(session_);
      session_ = nullptr;
    }
    if (format_) {
      CFRelease(format_);
      format_ = nullptr;
    }
  }

  static void Output(void* self, void*, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image, CMTime pts,
                     CMTime) {
    if (status != noErr || image == nullptr) return;
    auto* d = static_cast<VideoDecoder*>(self);
    int64_t us = CMTIME_IS_VALID(pts) ? CMTimeConvertScale(pts, 1000000, kCMTimeRoundingMethod_Default).value : 0;
    CVPixelBufferRetain(image);
    auto it = d->frames_.find(us);
    if (it != d->frames_.end()) {  // same pts twice: keep the newer
      CVPixelBufferRelease(it->second);
      it->second = image;
    } else {
      d->frames_[us] = image;
    }
  }

  bool hevc_;
  Bytes vps_, sps_, pps_;
  CMVideoFormatDescriptionRef format_ = nullptr;
  VTDecompressionSessionRef session_ = nullptr;
  std::map<int64_t, CVPixelBufferRef> frames_;  // decoded, by pts
  bool draining_ = false;
};

// window_jni.cc (libandroid_runtime, loaded first): shows a decoded frame on a Surface (its
// mNativeObject).
void PostToSurface(int64_t surface, CVPixelBufferRef frame) {
  using Post = void (*)(int64_t, void*);
  static Post post = reinterpret_cast<Post>(dlsym(RTLD_DEFAULT, "andropkg_surface_post_pixel_buffer"));
  if (post) post(surface, frame);
}
jfieldID gSurfaceNative;  // android.view.Surface.mNativeObject

// ---- MediaCodec ----------------------------------------------------------------------------
constexpr int kNumInput = 4, kNumOutput = 8;
constexpr size_t kInputSize = 128 * 1024, kOutputSize = 128 * 1024;
enum { INFO_TRY_AGAIN_LATER = -1, INFO_OUTPUT_FORMAT_CHANGED = -2, INFO_OUTPUT_BUFFERS_CHANGED = -3 };
enum { FLAG_END_OF_STREAM = 4, FLAG_CODEC_CONFIG = 2 };
enum { EVENT_CALLBACK = 1, CB_INPUT_AVAILABLE = 1, CB_OUTPUT_AVAILABLE = 2, CB_ERROR = 3, CB_OUTPUT_FORMAT_CHANGE = 4 };

struct Codec {
  int desc;
  jobject self = nullptr;  // (global) the Java MediaCodec, for async events
  std::mutex lock;
  Format format;
  std::unique_ptr<Decoder> dec;
  bool started = false, async = false, formatPending = true, eosQueued = false, float_out = false;
  Bytes in[kNumInput], out[kNumOutput];
  bool inFree[kNumInput], outFree[kNumOutput];
  size_t outSize[kNumOutput] = {};  // the valid bytes of each output buffer
  struct Ready {
    int index;
    size_t size;
    int64_t pts;
    int flags;
  };
  std::deque<Ready> ready;           // filled output buffers, not yet dequeued
  std::deque<float> pending;         // decoded PCM waiting for a free output buffer
  int64_t pendingPts = 0;
  bool pendingEos = false;
  jobject inBufs = nullptr, outBufs = nullptr;  // (global) ByteBuffer[] views
  // Video: the decoder, the output Surface (mNativeObject) and the frame each output holds.
  std::unique_ptr<VideoDecoder> vdec;
  int64_t surface = 0;
  CVPixelBufferRef frame[kNumOutput] = {};
  void DropFrames() {
    for (auto& f : frame) {
      if (f) CVPixelBufferRelease(f);
      f = nullptr;
    }
  }

  Codec() {
    for (auto& b : in) b.resize(kInputSize);
    for (auto& b : out) b.resize(kOutputSize);
    std::fill(std::begin(inFree), std::end(inFree), true);
    std::fill(std::begin(outFree), std::end(outFree), true);
  }
  int bytesPerFrame() const { return (dec ? dec->channels : 2) * (float_out ? 4 : 2); }
};

jfieldID gCodecContext;
jmethodID gPostEvent;

void EnsureCodecIds(JNIEnv* env, jobject codec) {
  if (gCodecContext) return;
  jclass c = env->GetObjectClass(codec);  // MediaCodec (final): initialized by now
  gPostEvent = env->GetMethodID(c, "postEventFromNative", "(IIILjava/lang/Object;)V");
  gCodecContext = env->GetFieldID(c, "mNativeContext", "J");
  env->DeleteLocalRef(c);
}
int64_t SurfaceNative(JNIEnv* env, jobject surface) {
  if (!surface) return 0;
  if (!gSurfaceNative) {
    jclass c = env->FindClass("android/view/Surface");
    gSurfaceNative = env->GetFieldID(c, "mNativeObject", "J");
    env->DeleteLocalRef(c);
  }
  return env->GetLongField(surface, gSurfaceNative);
}

Codec* GetCodec(JNIEnv* env, jobject thiz) {
  EnsureCodecIds(env, thiz);
  auto* c = reinterpret_cast<Codec*>(env->GetLongField(thiz, gCodecContext));
  if (!c) Throw(env, "java/lang/IllegalStateException", "released");
  return c;
}

jobject BufferInfo(JNIEnv* env, size_t offset, size_t size, int64_t pts, int flags) {
  jclass c = env->FindClass("android/media/MediaCodec$BufferInfo");
  jobject info = env->NewObject(c, env->GetMethodID(c, "<init>", "()V"));
  env->CallVoidMethod(info, env->GetMethodID(c, "set", "(IIJI)V"), (jint)offset, (jint)size, (jlong)pts, (jint)flags);
  env->DeleteLocalRef(c);
  return info;
}

jobject OutputFormatMap(JNIEnv* env, Codec* c) {
  JMap m(env);
  if (c->vdec) {
    int w = c->vdec->width, h = c->vdec->height;
    m.PutString("mime", "video/raw");
    m.PutInt("width", w);
    m.PutInt("height", h);
    m.PutInt("stride", w);
    m.PutInt("slice-height", h);
    m.PutInt("crop-left", 0);
    m.PutInt("crop-top", 0);
    m.PutInt("crop-right", w - 1);
    m.PutInt("crop-bottom", h - 1);
    m.PutInt("color-format", c->surface ? 0x7F000789 /* Surface */ : 0x7F420888 /* YUV420Flexible */);
    return m.get();
  }
  m.PutString("mime", "audio/raw");
  m.PutInt("sample-rate", c->dec ? c->dec->rate : 44100);
  m.PutInt("channel-count", c->dec ? c->dec->channels : 2);
  m.PutInt("pcm-encoding", c->float_out ? 4 : 2);
  return m.get();
}

void Post(JNIEnv* env, Codec* c, int what, int index, jobject obj) {
  if (!c->self) return;
  env->CallVoidMethod(c->self, gPostEvent, EVENT_CALLBACK, what, index, obj);
  if (obj) env->DeleteLocalRef(obj);
}

/// Moves decoded PCM into free output buffers (lock held). Returns the newly ready ones.
std::vector<Codec::Ready> FillOutputs(Codec* c) {
  std::vector<Codec::Ready> made;
  size_t frameBytes = (size_t)c->bytesPerFrame();
  while (!c->pending.empty() || c->pendingEos) {
    int idx = -1;
    for (int i = 0; i < kNumOutput; i++) if (c->outFree[i]) { idx = i; break; }
    if (idx < 0) break;
    size_t frames = std::min(c->pending.size() / (frameBytes / (c->float_out ? 4 : 2)), kOutputSize / frameBytes);
    size_t samples = frames * frameBytes / (c->float_out ? 4 : 2);
    uint8_t* dst = c->out[idx].data();
    for (size_t i = 0; i < samples; i++) {
      float v = c->pending.front();
      c->pending.pop_front();
      if (c->float_out) {
        memcpy(dst + i * 4, &v, 4);
      } else {
        int s = (int)lrintf(std::max(-1.f, std::min(1.f, v)) * 32767.f);
        int16_t s16 = (int16_t)s;
        memcpy(dst + i * 2, &s16, 2);
      }
    }
    int flags = 0;
    if (c->pending.empty() && c->pendingEos) {
      flags = FLAG_END_OF_STREAM;
      c->pendingEos = false;
    }
    if (samples == 0 && flags == 0) break;
    c->outFree[idx] = false;
    c->outSize[idx] = samples * (c->float_out ? 4 : 2);
    Codec::Ready r{idx, samples * (c->float_out ? 4 : 2), c->pendingPts, flags};
    int rate = c->dec ? c->dec->rate : 44100;
    c->pendingPts += (int64_t)frames * 1000000 / std::max(1, rate);
    c->ready.push_back(r);
    made.push_back(r);
  }
  return made;
}

/// Moves decoded frames into free output buffers (lock held). Returns the newly ready ones.
std::vector<Codec::Ready> FillVideoOutputs(Codec* c) {
  std::vector<Codec::Ready> made;
  for (;;) {
    int idx = -1;
    for (int i = 0; i < kNumOutput; i++) if (c->outFree[i]) { idx = i; break; }
    if (idx < 0) break;
    CVPixelBufferRef f = nullptr;
    int64_t pts = 0;
    int flags = 0;
    if (!c->vdec->Take(&f, &pts)) {
      if (!(c->pendingEos && c->vdec->Drained())) break;
      flags = FLAG_END_OF_STREAM;  // an empty buffer after the last frame
      c->pendingEos = false;
      pts = c->pendingPts;
    }
    if (c->vdec->sizeChanged) {  // dimensions known (or changed): format change first
      c->vdec->sizeChanged = false;
      c->formatPending = true;
    }
    c->outFree[idx] = false;
    c->frame[idx] = f;
    size_t size = f ? (size_t)c->vdec->width * c->vdec->height * 3 / 2 : 0;
    c->outSize[idx] = 0;
    if (f) c->pendingPts = pts;
    Codec::Ready r{idx, size, pts, flags};
    c->ready.push_back(r);
    made.push_back(r);
  }
  return made;
}

void cs_init(JNIEnv*, jclass) {}

void cs_setup(JNIEnv* env, jobject thiz, jstring name, jboolean nameIsType, jboolean encoder, jint, jint) {
  std::string n = Str(env, name);
  int d = nameIsType ? CodecByMime(n) : CodecByName(n);
  MLOG("codec setup %s type=%d encoder=%d -> %d", n.c_str(), nameIsType, encoder, d);
  if (encoder || d < 0) {
    Throw(env, "java/lang/IllegalArgumentException", "no codec for " + n + (encoder ? " (encoder)" : ""));
    return;
  }
  EnsureCodecIds(env, thiz);
  auto* c = new Codec;
  c->desc = d;
  c->self = env->NewGlobalRef(thiz);
  env->SetLongField(thiz, gCodecContext, reinterpret_cast<jlong>(c));
}

void ReleaseCodec(JNIEnv* env, jobject thiz) {
  EnsureCodecIds(env, thiz);
  auto* c = reinterpret_cast<Codec*>(env->GetLongField(thiz, gCodecContext));
  if (!c) return;
  env->SetLongField(thiz, gCodecContext, 0);
  if (c->self) env->DeleteGlobalRef(c->self);
  c->DropFrames();
  if (c->inBufs) env->DeleteGlobalRef(c->inBufs);
  if (c->outBufs) env->DeleteGlobalRef(c->outBufs);
  delete c;
}
void cs_release(JNIEnv* env, jobject thiz) { ReleaseCodec(env, thiz); }
void cs_finalize(JNIEnv* env, jobject thiz) { ReleaseCodec(env, thiz); }

void cs_configure(JNIEnv* env, jobject thiz, jobjectArray keys, jobjectArray values, jobject surface, jobject crypto,
                  jobject, jint flags) {
  Codec* c = GetCodec(env, thiz);
  MLOG("codec configure %p flags=%d crypto=%d", c, flags, crypto != nullptr);
  if (!c) return;
  if (flags & 1) {
    Throw(env, "java/lang/IllegalArgumentException", "no encoders");
    return;
  }
  std::lock_guard<std::mutex> g(c->lock);
  c->format = Format::From(env, keys, values);
  // Input buffers hold what the app says one access unit may take (max-input-size); a video
  // key frame can be far larger than an audio packet: at least an uncompressed 4:2:0 frame.
  size_t inSize = std::max<size_t>(kInputSize, (size_t)std::max<int64_t>(0, c->format.Int("max-input-size", 0)));
  if (IsVideo(c->desc)) {
    int64_t w = c->format.Int("width", 1920), h = c->format.Int("height", 1080);
    inSize = std::max(inSize, (size_t)std::max<int64_t>(w * h * 3 / 2, 1 << 20));
  }
  if (c->in[0].size() != inSize) {
    for (auto& b : c->in) b.assign(inSize, 0);
    if (c->inBufs) {  // views of the old buffers
      env->DeleteGlobalRef(c->inBufs);
      c->inBufs = nullptr;
    }
  }
  if (IsVideo(c->desc)) {
    c->vdec = std::make_unique<VideoDecoder>(strcmp(kCodecs[c->desc].mime, "video/hevc") == 0);
    c->vdec->Init(c->format);
    c->surface = SurfaceNative(env, surface);
    MLOG("video codec %s %dx%d surface=%lld", kCodecs[c->desc].name, c->vdec->width, c->vdec->height,
         (long long)c->surface);
    return;
  }
  c->float_out = c->format.Int("pcm-encoding", 2) == 4;
  c->dec = MakeDecoder(c->desc);
  if (!c->dec || !c->dec->Init(c->format)) {
    c->dec.reset();
    Throw(env, "java/lang/IllegalArgumentException", std::string("can't configure ") + kCodecs[c->desc].name);
  }
}

void cs_setCallback(JNIEnv* env, jobject thiz, jobject cb) {
  if (Codec* c = GetCodec(env, thiz)) c->async = cb != nullptr;
}

void cs_start(JNIEnv* env, jobject thiz) {
  Codec* c = GetCodec(env, thiz);
  MLOG("codec start %p", c);
  if (!c) return;
  if (!c->dec && !c->vdec) {
    Throw(env, "java/lang/IllegalStateException", "not configured");
    return;
  }
  std::vector<int> avail;
  {
    std::lock_guard<std::mutex> g(c->lock);
    c->started = true;
    c->formatPending = true;
    for (int i = 0; i < kNumInput; i++) if (c->inFree[i]) avail.push_back(i);
  }
  if (c->async) {
    Post(env, c, CB_OUTPUT_FORMAT_CHANGE, 0, OutputFormatMap(env, c));
    {
      std::lock_guard<std::mutex> g(c->lock);
      c->formatPending = false;
    }
    for (int i : avail) Post(env, c, CB_INPUT_AVAILABLE, i, nullptr);
  }
}

void ResetQueues(Codec* c) {
  std::fill(std::begin(c->inFree), std::end(c->inFree), true);
  std::fill(std::begin(c->outFree), std::end(c->outFree), true);
  c->ready.clear();
  c->pending.clear();
  c->pendingEos = false;
  c->eosQueued = false;
  c->DropFrames();
  if (c->vdec) c->vdec->Flush();
}

void cs_stop(JNIEnv* env, jobject thiz) {
  Codec* c = GetCodec(env, thiz);
  if (!c) return;
  std::lock_guard<std::mutex> g(c->lock);
  c->started = false;
  ResetQueues(c);
  if (c->dec) c->dec->Flush();
}
void cs_reset(JNIEnv* env, jobject thiz) {
  cs_stop(env, thiz);
  if (Codec* c = GetCodec(env, thiz)) {
    c->dec.reset();
    c->vdec.reset();
  }
}

void cs_flush(JNIEnv* env, jobject thiz) {
  Codec* c = GetCodec(env, thiz);
  if (!c) return;
  std::vector<int> avail;
  {
    std::lock_guard<std::mutex> g(c->lock);
    ResetQueues(c);
    if (c->dec) c->dec->Flush();
    for (int i = 0; i < kNumInput; i++) avail.push_back(i);
  }
  // (async: buffers come back after start(), which apps call after flush)
}

jint cs_dequeueInput(JNIEnv* env, jobject thiz, jlong timeoutUs) {
  Codec* c = GetCodec(env, thiz);
  MLOG("codec dequeueInput %p timeout=%lld", c, (long long)timeoutUs);
  if (!c) return INFO_TRY_AGAIN_LATER;
  std::lock_guard<std::mutex> g(c->lock);
  if (!c->started) {
    Throw(env, "java/lang/IllegalStateException", "not started");
    return INFO_TRY_AGAIN_LATER;
  }
  if (c->eosQueued) return INFO_TRY_AGAIN_LATER;
  for (int i = 0; i < kNumInput; i++) {
    if (c->inFree[i]) {
      c->inFree[i] = false;
      return i;
    }
  }
  return INFO_TRY_AGAIN_LATER;  // (input is consumed when queued: only while outputs are full)
}

void QueueInput(JNIEnv* env, Codec* c, int index, int offset, int size, int64_t pts, int flags) {
  std::vector<Codec::Ready> made;
  bool inputBack = false;
  {
    std::lock_guard<std::mutex> g(c->lock);
    if (index < 0 || index >= kNumInput || offset < 0 || size < 0 || (size_t)offset + size > c->in[index].size()) {
      Throw(env, "java/lang/IllegalArgumentException", "bad input buffer");
      return;
    }
    if (c->vdec) {
      if (size > 0) c->vdec->Decode(c->in[index].data() + offset, (size_t)size, pts);
      if (flags & FLAG_END_OF_STREAM) {
        c->vdec->Drain();
        c->pendingEos = true;
        c->eosQueued = true;
        c->pendingPts = pts;
      }
      c->inFree[index] = true;
      inputBack = !c->eosQueued;
      made = FillVideoOutputs(c);
    } else {
    std::vector<float> pcm;
    if (!(flags & FLAG_CODEC_CONFIG) && size > 0) c->dec->Decode(c->in[index].data() + offset, (size_t)size, &pcm);
    if (c->pending.empty()) c->pendingPts = pts;
    c->pending.insert(c->pending.end(), pcm.begin(), pcm.end());
    if (flags & FLAG_END_OF_STREAM) {
      c->pendingEos = true;
      c->eosQueued = true;
    }
    c->inFree[index] = true;
    inputBack = !c->eosQueued;
    made = FillOutputs(c);
    }
  }
  if (c->async) {
    bool formatNow = false;
    {
      std::lock_guard<std::mutex> g(c->lock);
      if (c->vdec && c->formatPending && !made.empty()) {
        c->formatPending = false;
        formatNow = true;
      }
    }
    if (formatNow) Post(env, c, CB_OUTPUT_FORMAT_CHANGE, 0, OutputFormatMap(env, c));
    for (auto& r : made) Post(env, c, CB_OUTPUT_AVAILABLE, r.index, BufferInfo(env, 0, r.size, r.pts, r.flags));
    std::lock_guard<std::mutex> g(c->lock);
    for (auto& r : made) {  // (posted: no longer waiting for dequeueOutputBuffer)
      auto it = std::find_if(c->ready.begin(), c->ready.end(), [&](const Codec::Ready& x) { return x.index == r.index; });
      if (it != c->ready.end()) c->ready.erase(it);
    }
  }
  if (c->async && inputBack) {
    {
      std::lock_guard<std::mutex> g(c->lock);
      c->inFree[index] = false;
    }
    Post(env, c, CB_INPUT_AVAILABLE, index, nullptr);
  }
}

void cs_queueInput(JNIEnv* env, jobject thiz, jint index, jint offset, jint size, jlong pts, jint flags) {
  Codec* c = GetCodec(env, thiz);
  if (!c) return;
  if (!c->started) {
    Throw(env, "java/lang/IllegalStateException", "not started");
    return;
  }
  QueueInput(env, c, index, offset, size, pts, flags);
}
void cs_queueSecureInput(JNIEnv* env, jobject, jint, jint, jobject, jlong, jint) {
  Throw(env, "android/media/MediaCodec$CryptoException", "no secure codecs");
}

jint cs_dequeueOutput(JNIEnv* env, jobject thiz, jobject info, jlong timeoutUs) {
  Codec* c = GetCodec(env, thiz);
  MLOG("codec dequeueOutput %p timeout=%lld", c, (long long)timeoutUs);
  if (!c) return INFO_TRY_AGAIN_LATER;
  std::lock_guard<std::mutex> g(c->lock);
  if (c->formatPending) {
    c->formatPending = false;
    return INFO_OUTPUT_FORMAT_CHANGED;
  }
  if (c->ready.empty()) c->vdec ? (void)FillVideoOutputs(c) : (void)FillOutputs(c);
  if (c->vdec && c->formatPending) {  // the frame size became known with these outputs
    c->formatPending = false;
    return INFO_OUTPUT_FORMAT_CHANGED;
  }
  if (c->ready.empty()) return INFO_TRY_AGAIN_LATER;
  Codec::Ready r = c->ready.front();
  c->ready.pop_front();
  jclass bi = env->GetObjectClass(info);
  env->CallVoidMethod(info, env->GetMethodID(bi, "set", "(IIJI)V"), 0, (jint)r.size, (jlong)r.pts, (jint)r.flags);
  env->DeleteLocalRef(bi);
  return r.index;
}

void cs_releaseOutput(JNIEnv* env, jobject thiz, jint index, jboolean render, jboolean, jlong) {
  Codec* c = GetCodec(env, thiz);
  if (!c) return;
  std::vector<Codec::Ready> made;
  CVPixelBufferRef show = nullptr;
  int64_t surface = 0;
  {
    std::lock_guard<std::mutex> g(c->lock);
    if (index < 0 || index >= kNumOutput) return;
    c->outFree[index] = true;
    if (c->vdec) {
      if (render && c->surface && c->frame[index]) {
        show = c->frame[index];
        surface = c->surface;
      } else if (c->frame[index]) {
        CVPixelBufferRelease(c->frame[index]);
      }
      c->frame[index] = nullptr;
    }
    made = c->vdec ? FillVideoOutputs(c) : FillOutputs(c);  // decoded data waiting for this buffer
    if (c->async) {
      for (auto& r : made) {
        auto it = std::find_if(c->ready.begin(), c->ready.end(), [&](const Codec::Ready& x) { return x.index == r.index; });
        if (it != c->ready.end()) c->ready.erase(it);
      }
    }
  }
  if (show) {
    PostToSurface(surface, show);
    CVPixelBufferRelease(show);
  }
  if (c->async)
    for (auto& r : made) Post(env, c, CB_OUTPUT_AVAILABLE, r.index, BufferInfo(env, 0, r.size, r.pts, r.flags));
}

void cs_setSurface(JNIEnv* env, jobject thiz, jobject surface) {  // MediaCodec.setOutputSurface
  Codec* c = GetCodec(env, thiz);
  if (!c) return;
  std::lock_guard<std::mutex> g(c->lock);
  c->surface = SurfaceNative(env, surface);
}
void cs_setVideoScalingMode(JNIEnv*, jobject, jint) {}

jobject MakeView(JNIEnv* env, Bytes& b) {
  jobject buf = env->NewDirectByteBuffer(b.data(), (jlong)b.size());
  jclass bo = env->FindClass("java/nio/ByteOrder");
  jobject native = env->CallStaticObjectMethod(bo, env->GetStaticMethodID(bo, "nativeOrder", "()Ljava/nio/ByteOrder;"));
  jclass bbc = env->FindClass("java/nio/ByteBuffer");
  jobject r = env->CallObjectMethod(buf, env->GetMethodID(bbc, "order", "(Ljava/nio/ByteOrder;)Ljava/nio/ByteBuffer;"), native);
  if (r) env->DeleteLocalRef(r);
  env->DeleteLocalRef(native);
  env->DeleteLocalRef(bo);
  env->DeleteLocalRef(bbc);
  return buf;
}

jobject cs_getBuffer(JNIEnv* env, jobject thiz, jboolean input, jint index) {
  Codec* c = GetCodec(env, thiz);
  if (!c) return nullptr;
  if (index < 0 || index >= (input ? kNumInput : kNumOutput)) return nullptr;
  // Video frames stay in the decoder's pixel buffers: as on Android with an output Surface,
  // there is no output ByteBuffer (players then size nothing by the frame's bytes).
  if (!input && c->vdec) return nullptr;
  jobject buf = MakeView(env, input ? c->in[index] : c->out[index]);
  if (!input) {  // the valid part: [0, size)
    size_t size;
    {
      std::lock_guard<std::mutex> g(c->lock);
      size = c->outSize[index];
    }
    jclass bbc = env->FindClass("java/nio/Buffer");
    jobject r = env->CallObjectMethod(buf, env->GetMethodID(bbc, "limit", "(I)Ljava/nio/Buffer;"), (jint)size);
    if (r) env->DeleteLocalRef(r);
    env->DeleteLocalRef(bbc);
  }
  return buf;
}

jobjectArray cs_getBuffers(JNIEnv* env, jobject thiz, jboolean input) {
  Codec* c = GetCodec(env, thiz);
  if (!c) return nullptr;
  if (!input && c->vdec) {  // (see cs_getBuffer) an array of nulls
    jclass bbc = env->FindClass("java/nio/ByteBuffer");
    jobjectArray a = env->NewObjectArray(kNumOutput, bbc, nullptr);
    env->DeleteLocalRef(bbc);
    return a;
  }
  jobject& cache = input ? c->inBufs : c->outBufs;
  if (!cache) {
    int n = input ? kNumInput : kNumOutput;
    jclass bbc = env->FindClass("java/nio/ByteBuffer");
    jobjectArray a = env->NewObjectArray(n, bbc, nullptr);
    for (int i = 0; i < n; i++) {
      jobject v = MakeView(env, input ? c->in[i] : c->out[i]);
      env->SetObjectArrayElement(a, i, v);
      env->DeleteLocalRef(v);
    }
    cache = env->NewGlobalRef(a);
    env->DeleteLocalRef(a);
    env->DeleteLocalRef(bbc);
  }
  return static_cast<jobjectArray>(env->NewLocalRef(cache));
}

jobject cs_getOutputFormat(JNIEnv* env, jobject thiz, jint) {
  Codec* c = GetCodec(env, thiz);
  return c ? OutputFormatMap(env, c) : nullptr;
}
jobject cs_getFormat(JNIEnv* env, jobject thiz, jboolean input) {
  Codec* c = GetCodec(env, thiz);
  if (!c) return nullptr;
  if (!input) return OutputFormatMap(env, c);
  JMap m(env);
  m.PutString("mime", kCodecs[c->desc].mime);
  if (c->vdec) {
    m.PutInt("width", c->vdec->width);
    m.PutInt("height", c->vdec->height);
    m.PutInt("max-input-size", (int)c->in[0].size());
    return m.get();
  }
  m.PutInt("sample-rate", c->dec ? c->dec->rate : 44100);
  m.PutInt("channel-count", c->dec ? c->dec->channels : 2);
  m.PutInt("max-input-size", (int)c->in[0].size());
  return m.get();
}

jstring cs_getCanonicalName(JNIEnv* env, jobject thiz) {
  Codec* c = GetCodec(env, thiz);
  return c ? env->NewStringUTF(kCodecs[c->desc].name) : nullptr;
}
void cs_setParameters(JNIEnv*, jobject, jobjectArray, jobjectArray) {}
void cs_signalEos(JNIEnv* env, jobject thiz) {
  Throw(env, "java/lang/IllegalStateException", "no input surface");
}

// ---- MediaCodecList / MediaCodecInfo ---------------------------------------------------------
jobject Capabilities(JNIEnv* env, int i) {
  jclass pl = env->FindClass("android/media/MediaCodecInfo$CodecProfileLevel");
  jobjectArray levels;
  jintArray colors;
  JMap def(env);
  def.PutString("mime", kCodecs[i].mime);
  JMap caps(env);
  if (IsVideo(i)) {
    // What VideoToolbox decodes on iPhones: AVC Baseline/Main/High (level 5.2), HEVC Main/Main10 (5.1).
    bool hevc = strcmp(kCodecs[i].mime, "video/hevc") == 0;
    std::vector<std::pair<int, int>> pls = hevc ? std::vector<std::pair<int, int>>{{1, 0x10000}, {2, 0x10000}}
                                                : std::vector<std::pair<int, int>>{{1, 0x10000}, {2, 0x10000}, {8, 0x10000}};
    levels = env->NewObjectArray((jsize)pls.size(), pl, nullptr);
    jmethodID ctor = env->GetMethodID(pl, "<init>", "()V");
    jfieldID fp = env->GetFieldID(pl, "profile", "I"), fl = env->GetFieldID(pl, "level", "I");
    for (size_t k = 0; k < pls.size(); k++) {
      jobject o = env->NewObject(pl, ctor);
      env->SetIntField(o, fp, pls[k].first);
      env->SetIntField(o, fl, pls[k].second);
      env->SetObjectArrayElement(levels, (jsize)k, o);
      env->DeleteLocalRef(o);
    }
    jint fmts[] = {0x7F420888, 0x7F000789};  // YUV420Flexible, Surface
    colors = env->NewIntArray(2);
    env->SetIntArrayRegion(colors, 0, 2, fmts);
    caps.PutString("size-range", "2x2-4096x4096");
    caps.PutString("alignment", "2x2");
    caps.PutString("block-size", "16x16");
    caps.PutString("blocks-per-second-range", "1-1958400");
    caps.PutString("frame-rate-range", "1-240");
    caps.PutString("bitrate-range", "1-240000000");
    caps.PutInt("feature-adaptive-playback", 1);  // features are Integers (CodecCapabilities)
  } else {
  levels = env->NewObjectArray(0, pl, nullptr);
  colors = env->NewIntArray(0);
  caps.PutString("max-channel-count", "8");
  caps.PutString("sample-rate-ranges", "7350-192000");
  caps.PutString("bitrate-range", "8000-960000");
  }
  jclass cc = env->FindClass("android/media/MediaCodecInfo$CodecCapabilities");
  jobject r = env->NewObject(cc,
                             env->GetMethodID(cc, "<init>",
                                              "([Landroid/media/MediaCodecInfo$CodecProfileLevel;[IZLjava/util/Map;Ljava/util/Map;)V"),
                             levels, colors, JNI_FALSE, def.get(), caps.get());
  env->DeleteLocalRef(cc);
  env->DeleteLocalRef(pl);
  return r;
}

// ANDROPKG_TEST_NO_CODECS: an empty MediaCodecList (debugging apps that choose formats by it).
jint cl_count(JNIEnv* env, jclass) {
  MLOG("codec list queried");
  if (MediaTrace()) {  // who asks (Thread.dumpStack() prints to stderr)
    jclass t = env->FindClass("java/lang/Thread");
    env->CallStaticVoidMethod(t, env->GetStaticMethodID(t, "dumpStack", "()V"));
    env->ExceptionClear();
  }
  return getenv("ANDROPKG_TEST_NO_CODECS") ? 0 : kNumCodecs;
}
jstring cl_name(JNIEnv* env, jclass, jint i) { return i >= 0 && i < kNumCodecs ? env->NewStringUTF(kCodecs[i].name) : nullptr; }
jint cl_attributes(JNIEnv*, jclass, jint) { return 4; }  // FLAG_IS_SOFTWARE_ONLY
jobjectArray cl_types(JNIEnv* env, jclass, jint i) {
  if (i < 0 || i >= kNumCodecs) return nullptr;
  jobjectArray a = env->NewObjectArray(1, env->FindClass("java/lang/String"), nullptr);
  env->SetObjectArrayElement(a, 0, env->NewStringUTF(kCodecs[i].mime));
  return a;
}
jobject cl_caps(JNIEnv* env, jclass, jint i, jstring) { return i >= 0 && i < kNumCodecs ? Capabilities(env, i) : nullptr; }
jint cl_find(JNIEnv* env, jclass, jstring name) { return CodecByName(Str(env, name)); }
jobject cl_global(JNIEnv* env, jclass) { return JMap(env).get(); }

jobject cs_getOwnCodecInfo(JNIEnv* env, jobject thiz) {
  Codec* c = GetCodec(env, thiz);
  if (!c) return nullptr;
  jclass cc = env->FindClass("android/media/MediaCodecInfo$CodecCapabilities");
  jobjectArray caps = env->NewObjectArray(1, cc, nullptr);
  jobject cap = Capabilities(env, c->desc);
  env->SetObjectArrayElement(caps, 0, cap);
  jclass ci = env->FindClass("android/media/MediaCodecInfo");
  jstring name = env->NewStringUTF(kCodecs[c->desc].name);
  return env->NewObject(ci, env->GetMethodID(ci, "<init>", "(Ljava/lang/String;Ljava/lang/String;I[Landroid/media/MediaCodecInfo$CodecCapabilities;)V"),
                        name, name, 4, caps);
}

// ---- MediaExtractor ---------------------------------------------------------------------------
struct Sample {
  Bytes data;
  int64_t timeUs;
  bool sync = true;
};
struct Track {
  std::string mime;
  int rate = 0, channels = 0;
  int width = 0, height = 0;  // (video)
  float frameRate = 0;
  int64_t durationUs = -1;
  std::vector<Bytes> csd;
  std::vector<Sample> samples;           // (compressed tracks)
  std::unique_ptr<andropkg_media::PcmDecoder> pcm;  // (audio/raw tracks: decoded on demand)
  Bytes rawChunk;
  int64_t rawFrame = 0;
  bool rawEnd = false;
  size_t maxSize = 0;
  bool selected = false;
  size_t cursor = 0;
};
struct Extractor {
  std::vector<std::unique_ptr<Track>> tracks;
  std::string container;
};

jfieldID gExtractorContext;

void EnsureExtractorIds(JNIEnv* env, jobject extractor) {
  if (gExtractorContext) return;
  jclass c = env->FindClass("android/media/MediaExtractor");  // initialized by now (an instance exists)
  gExtractorContext = env->GetFieldID(c, "mNativeContext", "J");
  env->DeleteLocalRef(c);
}

Extractor* GetExtractor(JNIEnv* env, jobject thiz) {
  EnsureExtractorIds(env, thiz);
  return reinterpret_cast<Extractor*>(env->GetLongField(thiz, gExtractorContext));
}

// Ogg pages -> packets (with the granule position of the page they end on).
struct OggPacket {
  Bytes data;
  int64_t granule;
};
std::vector<OggPacket> ReadOgg(const Bytes& file) {
  std::vector<OggPacket> packets;
  Bytes cur;
  size_t pos = 0;
  while (pos + 27 <= file.size() && !memcmp(&file[pos], "OggS", 4)) {
    int64_t granule;
    memcpy(&granule, &file[pos + 6], 8);
    int nseg = file[pos + 26];
    if (pos + 27 + nseg > file.size()) break;
    const uint8_t* segs = &file[pos + 27];
    size_t body = pos + 27 + nseg;
    for (int i = 0; i < nseg && body <= file.size(); i++) {
      size_t len = std::min<size_t>(segs[i], file.size() - body);
      cur.insert(cur.end(), file.begin() + body, file.begin() + body + len);
      body += segs[i];
      if (segs[i] < 255) {
        packets.push_back({std::move(cur), granule});
        cur.clear();
      }
    }
    pos = body;
  }
  return packets;
}

bool OpenOgg(Extractor* x, const Bytes& file) {
  std::vector<OggPacket> p = ReadOgg(file);
  if (p.size() < 3) return false;
  auto t = std::make_unique<Track>();
  const Bytes& head = p[0].data;
  if (head.size() >= 30 && head[0] == 1 && !memcmp(&head[1], "vorbis", 6)) {
    t->mime = "audio/vorbis";
    t->channels = head[11];
    memcpy(&t->rate, &head[12], 4);
    t->csd = {head, p[2].data};  // identification, setup
    for (size_t i = 3; i < p.size(); i++) {
      Bytes d = p[i].data;
      int32_t pageSamples = -1;  // Android's trailing page-sample count
      d.insert(d.end(), reinterpret_cast<uint8_t*>(&pageSamples), reinterpret_cast<uint8_t*>(&pageSamples) + 4);
      int64_t prev = i > 3 ? p[i - 1].granule : 0;
      t->samples.push_back({std::move(d), prev > 0 && t->rate ? prev * 1000000 / t->rate : 0});
    }
  } else if (head.size() >= 19 && !memcmp(head.data(), "OpusHead", 8)) {
    t->mime = "audio/opus";
    t->channels = head[9];
    t->rate = 48000;
    int preskip = head[10] | (head[11] << 8);
    int64_t delayNs = (int64_t)preskip * 1000000000LL / 48000, prerollNs = 80000000;
    Bytes d1(8), d2(8);
    memcpy(d1.data(), &delayNs, 8);
    memcpy(d2.data(), &prerollNs, 8);
    t->csd = {head, d1, d2};
    for (size_t i = 2; i < p.size(); i++) {
      int64_t prev = p[i - 1].granule;
      t->samples.push_back({p[i].data, prev > 0 ? std::max<int64_t>(0, prev - preskip) * 1000000 / 48000 : 0});
    }
  } else {
    return false;
  }
  int64_t last = p.back().granule;
  if (last > 0 && t->rate) t->durationUs = last * 1000000 / t->rate;
  for (auto& s : t->samples) t->maxSize = std::max(t->maxSize, s.data.size());
  x->container = "audio/ogg";
  x->tracks.push_back(std::move(t));
  return true;
}

// MP4/M4A/ADTS/MP3 through AudioFile: compressed packets.
struct FileReader {
  const Bytes* data;
};
OSStatus AFRead(void* user, SInt64 pos, UInt32 n, void* buf, UInt32* got) {
  auto* r = static_cast<FileReader*>(user);
  if (pos < 0 || (size_t)pos >= r->data->size()) {
    *got = 0;
    return noErr;
  }
  *got = (UInt32)std::min<size_t>(n, r->data->size() - pos);
  memcpy(buf, r->data->data() + pos, *got);
  return noErr;
}
SInt64 AFSize(void* user) { return (SInt64) static_cast<FileReader*>(user)->data->size(); }

/// The AudioSpecificConfig inside an esds cookie.
Bytes AscFromEsds(const Bytes& esds) {
  for (size_t i = 0; i + 2 < esds.size(); i++) {
    if (esds[i] != 5) continue;
    size_t j = i + 1, len = 0;
    for (int k = 0; k < 4 && j < esds.size(); k++) {
      len = (len << 7) | (esds[j] & 0x7f);
      if (!(esds[j++] & 0x80)) break;
    }
    if (len >= 2 && len <= 64 && j + len <= esds.size()) return Bytes(esds.begin() + j, esds.begin() + j + len);
  }
  return {};
}

// Video tracks of an MP4/MOV (AVAssetReader, compressed samples). Samples are converted to
// Annex-B and the parameter sets become csd-* as Android's MPEG4Extractor delivers them.
bool OpenVideoTracks(Extractor* x, const Bytes& file) {
  @autoreleasepool {
    NSString* path = [NSTemporaryDirectory() stringByAppendingPathComponent:
                                                 [NSString stringWithFormat:@"andropkg-extract-%@.mp4", NSUUID.UUID.UUIDString]];
    if (![[NSData dataWithBytesNoCopy:(void*)file.data() length:file.size() freeWhenDone:NO] writeToFile:path atomically:NO]) {
      return false;
    }
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path] options:nil];
    bool added = false;
    for (AVAssetTrack* track in [asset tracksWithMediaType:AVMediaTypeVideo]) {
      CMFormatDescriptionRef fd = (__bridge CMFormatDescriptionRef)track.formatDescriptions.firstObject;
      if (!fd) continue;
      FourCharCode codec = CMFormatDescriptionGetMediaSubType(fd);
      bool hevc = codec == kCMVideoCodecType_HEVC;
      if (codec != kCMVideoCodecType_H264 && !hevc) continue;
      auto t = std::make_unique<Track>();
      t->mime = hevc ? "video/hevc" : "video/avc";
      CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(fd);
      t->width = dim.width;
      t->height = dim.height;
      t->frameRate = track.nominalFrameRate;
      t->durationUs = (int64_t)(CMTimeGetSeconds(asset.duration) * 1e6);
      static const uint8_t kStart[] = {0, 0, 0, 1};
      size_t count = 0;
      int nalHeader = 4;
      if (hevc) {
        CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, 0, nullptr, nullptr, &count, &nalHeader);
      } else {
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 0, nullptr, nullptr, &count, &nalHeader);
      }
      Bytes all;
      for (size_t k = 0; k < count; k++) {
        const uint8_t* ps = nullptr;
        size_t len = 0;
        OSStatus st = hevc ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, k, &ps, &len, nullptr, nullptr)
                           : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, k, &ps, &len, nullptr, nullptr);
        if (st != noErr) continue;
        Bytes one(kStart, kStart + 4);
        one.insert(one.end(), ps, ps + len);
        if (hevc) {
          all.insert(all.end(), one.begin(), one.end());  // HEVC: VPS+SPS+PPS in csd-0
        } else {
          t->csd.push_back(one);  // AVC: csd-0 SPS, csd-1 PPS
        }
      }
      if (hevc && !all.empty()) t->csd.push_back(all);
      NSError* error = nil;
      AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&error];
      AVAssetReaderTrackOutput* out = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:nil];
      if (!reader || ![reader canAddOutput:out]) continue;
      [reader addOutput:out];
      if (![reader startReading]) continue;
      while (CMSampleBufferRef sb = [out copyNextSampleBuffer]) {
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
        size_t total = bb ? CMBlockBufferGetDataLength(bb) : 0;
        Bytes avcc(total);
        if (total) CMBlockBufferCopyDataBytes(bb, 0, total, avcc.data());
        Sample smp;
        for (size_t p = 0; p + nalHeader <= total;) {  // length-prefixed NALs -> start codes
          size_t len = 0;
          for (int b = 0; b < nalHeader; b++) len = len << 8 | avcc[p + b];
          p += nalHeader;
          if (p + len > total) break;
          smp.data.insert(smp.data.end(), kStart, kStart + 4);
          smp.data.insert(smp.data.end(), avcc.begin() + p, avcc.begin() + p + len);
          p += len;
        }
        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
        smp.timeUs = CMTIME_IS_VALID(pts) ? (int64_t)(CMTimeGetSeconds(pts) * 1e6) : 0;
        CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
        if (att && CFArrayGetCount(att) > 0) {
          auto d = (CFDictionaryRef)CFArrayGetValueAtIndex(att, 0);
          CFBooleanRef notSync = nullptr;
          smp.sync = !(CFDictionaryGetValueIfPresent(d, kCMSampleAttachmentKey_NotSync, (const void**)&notSync) &&
                       notSync && CFBooleanGetValue(notSync));
        }
        CFRelease(sb);
        if (smp.data.empty()) continue;
        t->maxSize = std::max(t->maxSize, smp.data.size());
        t->samples.push_back(std::move(smp));
      }
      // Samples come in decode order; readSampleData serves them so (pts may go back: B-frames).
      if (!t->samples.empty()) {
        x->tracks.push_back(std::move(t));
        added = true;
      }
    }
    [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    if (added) x->container = "video/mp4";
    return added;
  }
}

bool OpenAudioFile(Extractor* x, const Bytes& file) {
  FileReader reader{&file};
  AudioFileID af = nullptr;
  if (AudioFileOpenWithCallbacks(&reader, AFRead, nullptr, AFSize, nullptr, 0, &af) != noErr) return false;
  AudioStreamBasicDescription asbd{};
  UInt32 sz = sizeof asbd;
  AudioFileGetProperty(af, kAudioFilePropertyDataFormat, &sz, &asbd);
  bool aac = asbd.mFormatID == kAudioFormatMPEG4AAC || asbd.mFormatID == kAudioFormatMPEG4AAC_HE ||
             asbd.mFormatID == kAudioFormatMPEG4AAC_HE_V2;
  bool mp3 = asbd.mFormatID == kAudioFormatMPEGLayer3;
  if (!aac && !mp3) {
    AudioFileClose(af);
    return false;
  }
  auto t = std::make_unique<Track>();
  t->mime = aac ? "audio/mp4a-latm" : "audio/mpeg";
  t->rate = (int)asbd.mSampleRate;
  t->channels = (int)asbd.mChannelsPerFrame;
  if (aac) {
    UInt32 csz = 0;
    if (AudioFileGetPropertyInfo(af, kAudioFilePropertyMagicCookieData, &csz, nullptr) == noErr && csz) {
      Bytes cookie(csz);
      AudioFileGetProperty(af, kAudioFilePropertyMagicCookieData, &csz, cookie.data());
      Bytes asc = AscFromEsds(cookie);
      if (!asc.empty()) t->csd.push_back(asc);
    }
  }
  UInt64 count = 0;
  sz = sizeof count;
  AudioFileGetProperty(af, kAudioFilePropertyAudioDataPacketCount, &sz, &count);
  UInt32 maxPacket = 0;
  sz = sizeof maxPacket;
  AudioFileGetProperty(af, kAudioFilePropertyMaximumPacketSize, &sz, &maxPacket);
  if (!maxPacket) maxPacket = 8192;
  int fpp = asbd.mFramesPerPacket ? (int)asbd.mFramesPerPacket : (aac ? 1024 : 1152);
  std::vector<uint8_t> buf(maxPacket * 64);
  std::vector<AudioStreamPacketDescription> descs(64);
  for (UInt64 p = 0; p < count;) {
    UInt32 bytes = (UInt32)buf.size(), n = 64;
    if (AudioFileReadPacketData(af, false, &bytes, descs.data(), (SInt64)p, &n, buf.data()) != noErr || n == 0) break;
    for (UInt32 i = 0; i < n; i++) {
      const uint8_t* s = buf.data() + descs[i].mStartOffset;
      t->samples.push_back({Bytes(s, s + descs[i].mDataByteSize), (int64_t)(p + i) * fpp * 1000000 / std::max(1, t->rate)});
      t->maxSize = std::max<size_t>(t->maxSize, descs[i].mDataByteSize);
    }
    p += n;
  }
  AudioFileClose(af);
  t->durationUs = (int64_t)count * fpp * 1000000 / std::max(1, t->rate);
  x->container = aac ? "audio/mp4" : "audio/mpeg";
  x->tracks.push_back(std::move(t));
  return true;
}

// Anything else Core Audio decodes: an audio/raw track (16-bit PCM), decoded as read.
bool OpenRaw(Extractor* x, int fd, int64_t offset, int64_t length) {
  auto dec = andropkg_media::OpenDecoder(fd, offset, length);
  if (!dec) return false;
  auto t = std::make_unique<Track>();
  t->mime = "audio/raw";
  t->rate = dec->rate();
  t->channels = dec->channels();
  if (dec->frames() > 0) t->durationUs = dec->frames() * 1000000 / std::max(1, t->rate);
  t->maxSize = 4096 * 2 * (size_t)t->channels;
  t->pcm = std::move(dec);
  x->container = "audio/raw";
  x->tracks.push_back(std::move(t));
  return true;
}

void OpenSource(JNIEnv* env, jobject thiz, int fd, int64_t offset, int64_t length) {
  Extractor* x = GetExtractor(env, thiz);
  if (!x) return;
  struct stat st;
  if (fstat(fd, &st) != 0) {
    Throw(env, "java/io/IOException", "bad file descriptor");
    return;
  }
  if (length < 0 || offset + length > st.st_size) length = st.st_size - offset;
  Bytes file((size_t)std::max<int64_t>(0, length));
  ssize_t got = pread(fd, file.data(), file.size(), offset);
  if (got < 0) got = 0;
  file.resize((size_t)got);
  x->tracks.clear();
  if (andropkg_media::IsOgg(file.data(), file.size())) {
    if (OpenOgg(x, file)) return;
  } else {
    bool video = OpenVideoTracks(x, file);
    if (OpenAudioFile(x, file) || video) return;  // (an MP4's audio track comes from AudioFile)
  }
  if (OpenRaw(x, fd, offset, length)) return;
  Throw(env, "java/io/IOException", "Failed to instantiate extractor.");
}

void ex_init(JNIEnv*, jclass) {}
void ex_setup(JNIEnv* env, jobject thiz) { EnsureExtractorIds(env, thiz); env->SetLongField(thiz, gExtractorContext, reinterpret_cast<jlong>(new Extractor)); }
void ex_release(JNIEnv* env, jobject thiz) {
  delete GetExtractor(env, thiz);
  env->SetLongField(thiz, gExtractorContext, 0);
}

void ex_setDataSourceFd(JNIEnv* env, jobject thiz, jobject fdObj, jlong offset, jlong length) {
  jclass fdc = env->FindClass("java/io/FileDescriptor");
  int fd = env->GetIntField(fdObj, env->GetFieldID(fdc, "descriptor", "I"));
  MLOG("extractor setDataSource fd=%d offset=%lld length=%lld", fd, (long long)offset, (long long)length);
  OpenSource(env, thiz, fd, offset, length);
}
void ex_setDataSourcePath(JNIEnv* env, jobject thiz, jobject, jstring jpath, jobjectArray, jobjectArray) {
  std::string path = Str(env, jpath);
  MLOG("extractor setDataSource path %s", path.c_str());
  if (path.rfind("file://", 0) == 0) path = path.substr(7);
  if (path.find("://") != std::string::npos) {
    Throw(env, "java/io/IOException", "Failed to instantiate extractor (network sources: not yet).");
    return;
  }
  char buf[1024];
  int fd = open(andropkg_translate_path(path.c_str(), buf, sizeof buf), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    Throw(env, "java/io/IOException", "Failed to open " + path);
    return;
  }
  OpenSource(env, thiz, fd, 0, -1);
  close(fd);
}
void ex_setDataSourceMedia(JNIEnv* env, jobject thiz, jobject source) {
  // A MediaDataSource: read it whole into a temporary file.
  jclass c = env->GetObjectClass(source);
  jlong size = env->CallLongMethod(source, env->GetMethodID(c, "getSize", "()J"));
  MLOG("extractor setDataSource MediaDataSource size=%lld", (long long)size);
  jmethodID readAt = env->GetMethodID(c, "readAt", "(J[BII)I");
  char path[1024];
  snprintf(path, sizeof path, "%s/andropkg-extractor-XXXXXX", getenv("TMPDIR") ?: "/tmp");
  int fd = mkstemp(path);
  if (fd < 0) {
    Throw(env, "java/io/IOException", "temporary file");
    return;
  }
  unlink(path);
  jbyteArray chunk = env->NewByteArray(65536);
  for (jlong pos = 0; size < 0 || pos < size;) {
    jint n = env->CallIntMethod(source, readAt, pos, chunk, 0, 65536);
    if (env->ExceptionCheck() || n <= 0) break;
    jbyte* b = env->GetByteArrayElements(chunk, nullptr);
    (void)!pwrite(fd, b, (size_t)n, pos);
    env->ReleaseByteArrayElements(chunk, b, JNI_ABORT);
    pos += n;
  }
  if (!env->ExceptionCheck()) OpenSource(env, thiz, fd, 0, -1);
  close(fd);
}

jint ex_trackCount(JNIEnv* env, jobject thiz) {
  Extractor* x = GetExtractor(env, thiz);
  return x ? (jint)x->tracks.size() : 0;
}

jobject ex_trackFormat(JNIEnv* env, jobject thiz, jint i) {
  Extractor* x = GetExtractor(env, thiz);
  if (!x || i < 0 || i >= (jint)x->tracks.size()) {
    Throw(env, "java/lang/IllegalArgumentException", "bad track index");
    return nullptr;
  }
  Track& t = *x->tracks[i];
  JMap m(env);
  m.PutString("mime", t.mime);
  if (t.width > 0) {
    m.PutInt("width", t.width);
    m.PutInt("height", t.height);
    if (t.frameRate > 0) m.PutInt("frame-rate", (int)lrintf(t.frameRate));
  } else {
    m.PutInt("sample-rate", t.rate);
    m.PutInt("channel-count", t.channels);
  }
  if (t.durationUs >= 0) m.PutLong("durationUs", t.durationUs);
  m.PutInt("max-input-size", (int)std::max<size_t>(t.maxSize + 16, 8192));
  if (t.mime == "audio/raw") m.PutInt("pcm-encoding", 2);
  for (size_t k = 0; k < t.csd.size(); k++) m.PutBuffer(("csd-" + std::to_string(k)).c_str(), t.csd[k]);
  return m.get();
}
jobject ex_fileFormat(JNIEnv* env, jobject thiz) {
  Extractor* x = GetExtractor(env, thiz);
  JMap m(env);
  if (x) m.PutString("mime", x->container);
  return m.get();
}

void ex_select(JNIEnv* env, jobject thiz, jint i) {
  Extractor* x = GetExtractor(env, thiz);
  if (x && i >= 0 && i < (jint)x->tracks.size()) x->tracks[i]->selected = true;
}
void ex_unselect(JNIEnv* env, jobject thiz, jint i) {
  Extractor* x = GetExtractor(env, thiz);
  if (x && i >= 0 && i < (jint)x->tracks.size()) x->tracks[i]->selected = false;
}

/// The raw track's next chunk (decoded if needed); false at the end.
bool RawChunk(Track& t) {
  if (!t.rawChunk.empty()) return true;
  if (t.rawEnd) return false;
  std::vector<float> f(4096 * (size_t)t.channels);
  int n = t.pcm->Read(f.data(), 4096);
  if (n <= 0) {
    t.rawEnd = true;
    return false;
  }
  t.rawChunk.resize((size_t)n * t.channels * 2);
  for (size_t i = 0; i < (size_t)n * t.channels; i++) {
    int16_t s = (int16_t)lrintf(std::max(-1.f, std::min(1.f, f[i])) * 32767.f);
    memcpy(&t.rawChunk[i * 2], &s, 2);
  }
  return true;
}

/// The selected track whose next sample comes first, or -1 at the end.
int NextTrack(Extractor* x) {
  int best = -1;
  int64_t bestTime = INT64_MAX;
  for (size_t i = 0; i < x->tracks.size(); i++) {
    Track& t = *x->tracks[i];
    if (!t.selected) continue;
    int64_t time;
    if (t.pcm) {
      if (!RawChunk(t)) continue;
      time = t.rawFrame * 1000000 / std::max(1, t.rate);
    } else {
      if (t.cursor >= t.samples.size()) continue;
      time = t.samples[t.cursor].timeUs;
    }
    if (time < bestTime) best = (int)i, bestTime = time;
  }
  return best;
}

jint ex_readSampleData(JNIEnv* env, jobject thiz, jobject buf, jint offset) {
  Extractor* x = GetExtractor(env, thiz);
  if (!x) return -1;
  int ti = NextTrack(x);
  if (ti < 0) return -1;
  Track& t = *x->tracks[ti];
  const Bytes& data = t.pcm ? t.rawChunk : t.samples[t.cursor].data;
  jlong cap = env->GetDirectBufferCapacity(buf);
  auto* dst = static_cast<uint8_t*>(env->GetDirectBufferAddress(buf));
  jclass bc = env->FindClass("java/nio/ByteBuffer");
  if (!dst) {  // a heap buffer
    jbyteArray a = static_cast<jbyteArray>(env->CallObjectMethod(buf, env->GetMethodID(bc, "array", "()[B")));
    jint aoff = env->CallIntMethod(buf, env->GetMethodID(bc, "arrayOffset", "()I"));
    if (!a || offset + (jlong)data.size() > env->GetArrayLength(a) - aoff) {
      Throw(env, "java/lang/IllegalArgumentException", "buffer too small");
      return -1;
    }
    env->SetByteArrayRegion(a, aoff + offset, (jsize)data.size(), reinterpret_cast<const jbyte*>(data.data()));
  } else {
    if (offset + (jlong)data.size() > cap) {
      Throw(env, "java/lang/IllegalArgumentException", "buffer too small");
      return -1;
    }
    memcpy(dst + offset, data.data(), data.size());
  }
  jclass bufc = env->FindClass("java/nio/Buffer");
  jobject r1 = env->CallObjectMethod(buf, env->GetMethodID(bufc, "limit", "(I)Ljava/nio/Buffer;"), (jint)(offset + data.size()));
  jobject r2 = env->CallObjectMethod(buf, env->GetMethodID(bufc, "position", "(I)Ljava/nio/Buffer;"), offset);
  if (r1) env->DeleteLocalRef(r1);
  if (r2) env->DeleteLocalRef(r2);
  return (jint)data.size();
}

jboolean ex_advance(JNIEnv* env, jobject thiz) {
  Extractor* x = GetExtractor(env, thiz);
  if (!x) return JNI_FALSE;
  int ti = NextTrack(x);
  if (ti < 0) return JNI_FALSE;
  Track& t = *x->tracks[ti];
  if (t.pcm) {
    t.rawFrame += (int64_t)t.rawChunk.size() / (2 * t.channels);
    t.rawChunk.clear();
  } else {
    t.cursor++;
  }
  return NextTrack(x) >= 0;
}

jint ex_sampleTrack(JNIEnv* env, jobject thiz) {
  Extractor* x = GetExtractor(env, thiz);
  return x ? NextTrack(x) : -1;
}
jlong ex_sampleTime(JNIEnv* env, jobject thiz) {
  Extractor* x = GetExtractor(env, thiz);
  int ti = x ? NextTrack(x) : -1;
  if (ti < 0) return -1;
  Track& t = *x->tracks[ti];
  return t.pcm ? t.rawFrame * 1000000 / std::max(1, t.rate) : t.samples[t.cursor].timeUs;
}
jlong ex_sampleSize(JNIEnv* env, jobject thiz) {
  Extractor* x = GetExtractor(env, thiz);
  int ti = x ? NextTrack(x) : -1;
  if (ti < 0) return -1;
  Track& t = *x->tracks[ti];
  return (jlong)(t.pcm ? t.rawChunk.size() : t.samples[t.cursor].data.size());
}
jint ex_sampleFlags(JNIEnv* env, jobject thiz) {
  Extractor* x = GetExtractor(env, thiz);
  int ti = x ? NextTrack(x) : -1;
  if (ti < 0) return -1;
  Track& t = *x->tracks[ti];
  return t.pcm || t.samples[t.cursor].sync ? 1 /* SAMPLE_FLAG_SYNC */ : 0;
}

void ex_seekTo(JNIEnv* env, jobject thiz, jlong timeUs, jint mode) {
  Extractor* x = GetExtractor(env, thiz);
  if (!x) return;
  for (auto& tp : x->tracks) {
    Track& t = *tp;
    if (!t.selected) continue;
    if (t.pcm) {
      int64_t frame = std::max<int64_t>(0, timeUs) * t.rate / 1000000;
      t.pcm->Seek(frame);
      t.rawFrame = frame;
      t.rawChunk.clear();
      t.rawEnd = false;
    } else {
      // The last sync sample at or before timeUs (every audio sample is one); video samples are
      // in decode order, so compare presentation times of sync samples only.
      size_t i = 0;
      for (size_t k = 0; k < t.samples.size(); k++) {
        if (t.samples[k].sync && t.samples[k].timeUs <= timeUs) i = k;
      }
      t.cursor = i;
    }
  }
}

jlong ex_cachedDuration(JNIEnv*, jobject) { return -1; }
jboolean ex_cacheEos(JNIEnv*, jobject) { return JNI_TRUE; }
jboolean ex_cryptoInfo(JNIEnv*, jobject, jobject) { return JNI_FALSE; }

#define M(n, s, f) {n, s, reinterpret_cast<void*>(f)}

}  // namespace

namespace andropkg_media {

void RegisterCodecs(JNIEnv* env) {
  // Field and method IDs are looked up on first use (EnsureIds), not here: GetFieldID would
  // initialize MediaCodec/MediaExtractor, whose static initializers load this very library;
  // with one thread loading it from MediaCodec.<clinit> and another in MediaExtractor.<clinit>,
  // each waited for the other.
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return;
  }
  static const JNINativeMethod kCodec[] = {
      M("native_init", "()V", cs_init),
      M("native_setup", "(Ljava/lang/String;ZZII)V", cs_setup),
      M("native_release", "()V", cs_release),
      M("native_finalize", "()V", cs_finalize),
      M("native_reset", "()V", cs_reset),
      M("native_configure",
        "([Ljava/lang/String;[Ljava/lang/Object;Landroid/view/Surface;Landroid/media/MediaCrypto;Landroid/os/IHwBinder;I)V",
        cs_configure),
      M("native_setCallback", "(Landroid/media/MediaCodec$Callback;)V", cs_setCallback),
      M("native_start", "()V", cs_start),
      M("native_stop", "()V", cs_stop),
      M("native_flush", "()V", cs_flush),
      M("native_dequeueInputBuffer", "(J)I", cs_dequeueInput),
      M("native_queueInputBuffer", "(IIIJI)V", cs_queueInput),
      M("native_queueSecureInputBuffer", "(IILandroid/media/MediaCodec$CryptoInfo;JI)V", cs_queueSecureInput),
      M("native_dequeueOutputBuffer", "(Landroid/media/MediaCodec$BufferInfo;J)I", cs_dequeueOutput),
      M("releaseOutputBuffer", "(IZZJ)V", cs_releaseOutput),
      M("getBuffer", "(ZI)Ljava/nio/ByteBuffer;", cs_getBuffer),
      M("getBuffers", "(Z)[Ljava/nio/ByteBuffer;", cs_getBuffers),
      M("getOutputFormatNative", "(I)Ljava/util/Map;", cs_getOutputFormat),
      M("getFormatNative", "(Z)Ljava/util/Map;", cs_getFormat),
      M("getCanonicalName", "()Ljava/lang/String;", cs_getCanonicalName),
      M("getOwnCodecInfo", "()Landroid/media/MediaCodecInfo;", cs_getOwnCodecInfo),
      M("setParameters", "([Ljava/lang/String;[Ljava/lang/Object;)V", cs_setParameters),
      M("signalEndOfInputStream", "()V", cs_signalEos),
      M("native_setSurface", "(Landroid/view/Surface;)V", cs_setSurface),
      M("setVideoScalingMode", "(I)V", cs_setVideoScalingMode),
  };
  RegisterDeclared(env, "android/media/MediaCodec", kCodec, sizeof kCodec / sizeof *kCodec);
  static const JNINativeMethod kList[] = {
      M("native_init", "()V", cs_init),
      M("native_getCodecCount", "()I", cl_count),
      M("getCodecName", "(I)Ljava/lang/String;", cl_name),
      M("getCanonicalName", "(I)Ljava/lang/String;", cl_name),
      M("getAttributes", "(I)I", cl_attributes),
      M("getSupportedTypes", "(I)[Ljava/lang/String;", cl_types),
      M("getCodecCapabilities", "(ILjava/lang/String;)Landroid/media/MediaCodecInfo$CodecCapabilities;", cl_caps),
      M("findCodecByName", "(Ljava/lang/String;)I", cl_find),
      M("native_getGlobalSettings", "()Ljava/util/Map;", cl_global),
  };
  RegisterDeclared(env, "android/media/MediaCodecList", kList, sizeof kList / sizeof *kList);
  static const JNINativeMethod kExtractor[] = {
      M("native_init", "()V", ex_init),
      M("native_setup", "()V", ex_setup),
      M("native_finalize", "()V", ex_release),
      M("release", "()V", ex_release),
      M("setDataSource", "(Ljava/io/FileDescriptor;JJ)V", ex_setDataSourceFd),
      M("nativeSetDataSource", "(Landroid/os/IBinder;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V",
        ex_setDataSourcePath),
      M("setDataSource", "(Landroid/media/MediaDataSource;)V", ex_setDataSourceMedia),
      M("getTrackCount", "()I", ex_trackCount),
      M("getTrackFormatNative", "(I)Ljava/util/Map;", ex_trackFormat),
      M("getFileFormatNative", "()Ljava/util/Map;", ex_fileFormat),
      M("selectTrack", "(I)V", ex_select),
      M("unselectTrack", "(I)V", ex_unselect),
      M("seekTo", "(JI)V", ex_seekTo),
      M("advance", "()Z", ex_advance),
      M("readSampleData", "(Ljava/nio/ByteBuffer;I)I", ex_readSampleData),
      M("getSampleTrackIndex", "()I", ex_sampleTrack),
      M("getSampleTime", "()J", ex_sampleTime),
      M("getSampleSize", "()J", ex_sampleSize),
      M("getSampleFlags", "()I", ex_sampleFlags),
      M("getCachedDuration", "()J", ex_cachedDuration),
      M("hasCacheReachedEndOfStream", "()Z", ex_cacheEos),
      M("getSampleCryptoInfo", "(Landroid/media/MediaCodec$CryptoInfo;)Z", ex_cryptoInfo),
  };
  RegisterDeclared(env, "android/media/MediaExtractor", kExtractor, sizeof kExtractor / sizeof *kExtractor);
}

}  // namespace andropkg_media
