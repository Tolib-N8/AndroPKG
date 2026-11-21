// andropkg: audio decoders (see decoder.h).
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "decoder.h"
#include "ivorbisfile.h"
#include "opus.h"
#include "opus_multistream.h"

namespace andropkg_media {

bool IsOgg(const uint8_t* head, size_t n) { return n >= 4 && !memcmp(head, "OggS", 4); }

namespace {

/// A byte range of a file, read with pread (the decoder owns a dup of the fd).
struct Range {
  int fd = -1;
  int64_t start = 0, size = 0, pos = 0;
  ~Range() {
    if (fd >= 0) close(fd);
  }
  bool Open(int src, int64_t offset, int64_t length) {
    struct stat st;
    if (fstat(src, &st) != 0 || offset >= st.st_size) return false;
    fd = dup(src);
    start = offset;
    size = length < 0 ? st.st_size - offset : std::min<int64_t>(length, st.st_size - offset);
    return fd >= 0;
  }
  size_t ReadAt(int64_t at, void* buf, size_t n) {
    if (at >= size) return 0;
    n = size_t(std::min<int64_t>(int64_t(n), size - at));
    ssize_t r = pread(fd, buf, n, start + at);
    return r > 0 ? size_t(r) : 0;
  }
};

// ---- Ogg Vorbis (Tremolo) ---------------------------------------------------------------------
class VorbisDecoder : public PcmDecoder {
 public:
  ~VorbisDecoder() override {
    if (open_) ov_clear(&vf_);
  }
  bool Open(int fd, int64_t offset, int64_t length) {
    if (!range_.Open(fd, offset, length)) return false;
    ov_callbacks cb = {&ReadCb, &SeekCb, &CloseCb, &TellCb};
    if (ov_open_callbacks(&range_, &vf_, nullptr, 0, cb) != 0) return false;
    open_ = true;
    vorbis_info* vi = ov_info(&vf_, -1);
    if (!vi || vi->channels <= 0) return false;
    channels_ = vi->channels;
    rate_ = int(vi->rate);
    frames_ = ov_pcm_total(&vf_, -1);
    return true;
  }
  int rate() const override { return rate_; }
  int channels() const override { return channels_; }
  int64_t frames() const override { return frames_; }
  int Read(float* out, int max_frames) override {
    int done = 0;
    while (done < max_frames) {
      int want = std::min(max_frames - done, int(sizeof pcm_ / 2 / channels_));
      int bitstream = 0;
      long bytes = ov_read(&vf_, pcm_, want * channels_ * 2, &bitstream);
      if (bytes == OV_HOLE) continue;
      if (bytes <= 0) break;
      int n = int(bytes / 2);  // 16-bit samples
      for (int i = 0; i < n; i++) out[size_t(done) * channels_ + i] = pcm_[i] / 32768.f;
      done += n / channels_;
    }
    return done;
  }
  bool Seek(int64_t frame) override { return ov_pcm_seek(&vf_, frame) == 0; }

 private:
  static size_t ReadCb(void* ptr, size_t size, size_t nmemb, void* src) {
    auto* r = static_cast<Range*>(src);
    size_t got = r->ReadAt(r->pos, ptr, size * nmemb);
    r->pos += int64_t(got);
    return size ? got / size : 0;
  }
  static int SeekCb(void* src, ogg_int64_t offset, int whence) {
    auto* r = static_cast<Range*>(src);
    int64_t p = whence == SEEK_SET ? offset : whence == SEEK_CUR ? r->pos + offset : r->size + offset;
    if (p < 0 || p > r->size) return -1;
    r->pos = p;
    return 0;
  }
  static int CloseCb(void*) { return 0; }
  static long TellCb(void* src) { return long(static_cast<Range*>(src)->pos); }

  Range range_;
  OggVorbis_File vf_ = {};
  bool open_ = false;
  int channels_ = 0, rate_ = 0;
  int64_t frames_ = -1;
  int16_t pcm_[4096];
};

// ---- Ogg Opus (libopus + a minimal Ogg page reader) -------------------------------------------
class OpusFileDecoder : public PcmDecoder {
 public:
  ~OpusFileDecoder() override {
    if (dec_) opus_multistream_decoder_destroy(dec_);
  }
  bool Open(int fd, int64_t offset, int64_t length) {
    if (!range_.Open(fd, offset, length)) return false;
    // Index every page (offset, granule) once: cheap (headers only) and makes seeking exact.
    for (int64_t at = 0; at + 27 <= range_.size;) {
      uint8_t h[27 + 255];
      if (range_.ReadAt(at, h, 27) != 27 || memcmp(h, "OggS", 4)) break;
      int nseg = h[26];
      if (range_.ReadAt(at + 27, h + 27, size_t(nseg)) != size_t(nseg)) break;
      int64_t body = 0;
      for (int i = 0; i < nseg; i++) body += h[27 + i];
      int64_t granule;
      memcpy(&granule, h + 6, 8);
      pages_.push_back({at, granule});
      at += 27 + nseg + body;
    }
    if (pages_.empty()) return false;
    std::vector<uint8_t> head;
    if (!NextPacket(&head) || head.size() < 19 || memcmp(head.data(), "OpusHead", 8)) return false;
    channels_ = head[9];
    preskip_ = head[10] | head[11] << 8;
    int family = head.size() > 18 ? head[18] : 0;
    int streams = 1, coupled = channels_ > 1 ? 1 : 0;
    uint8_t mapping[255] = {0, 1};
    if (family != 0 && head.size() >= size_t(21 + channels_)) {
      streams = head[19];
      coupled = head[20];
      memcpy(mapping, head.data() + 21, size_t(channels_));
    }
    int err = 0;
    dec_ = opus_multistream_decoder_create(48000, channels_, streams, coupled, mapping, &err);
    if (err != OPUS_OK || !dec_) return false;
    std::vector<uint8_t> tags;
    if (!NextPacket(&tags)) return false;  // OpusTags
    audio_start_page_ = page_;
    int64_t last = -1;
    for (auto it = pages_.rbegin(); it != pages_.rend(); ++it) {
      if (it->granule >= 0) {
        last = it->granule;
        break;
      }
    }
    frames_ = last > preskip_ ? last - preskip_ : -1;
    skip_ = preskip_;
    return true;
  }
  int rate() const override { return 48000; }
  int channels() const override { return channels_; }
  int64_t frames() const override { return frames_; }

  int Read(float* out, int max_frames) override {
    int done = 0;
    while (done < max_frames) {
      if (pcm_pos_ < pcm_len_) {
        int n = std::min(max_frames - done, pcm_len_ - pcm_pos_);
        memcpy(out + size_t(done) * channels_, &pcm_[size_t(pcm_pos_) * channels_], size_t(n) * channels_ * sizeof(float));
        pcm_pos_ += n;
        done += n;
        continue;
      }
      std::vector<uint8_t> pkt;
      if (!NextPacket(&pkt)) break;
      int n = opus_multistream_decode_float(dec_, pkt.data(), int(pkt.size()), pcm_, 5760, 0);
      if (n < 0) continue;
      int drop = int(std::min<int64_t>(skip_, n));
      skip_ -= drop;
      pcm_pos_ = drop;
      pcm_len_ = n;
    }
    return done;
  }

  bool Seek(int64_t frame) override {
    // Start decoding 80 ms early (Opus pre-roll) from the page before the target.
    int64_t target = frame + preskip_;
    int64_t from = std::max<int64_t>(0, target - 3840);
    size_t p = audio_start_page_;
    for (size_t i = audio_start_page_; i < pages_.size(); i++) {
      if (pages_[i].granule >= 0 && pages_[i].granule <= from) p = i + 1;
    }
    p = std::min(p, pages_.size() - 1);
    int64_t base = p > audio_start_page_ ? pages_[p - 1].granule : 0;
    page_ = p;
    seg_ = 0;
    lacing_.clear();
    partial_.clear();
    pcm_pos_ = pcm_len_ = 0;
    opus_multistream_decoder_ctl(dec_, OPUS_RESET_STATE);
    skip_ = std::max<int64_t>(0, target - base);
    return true;
  }

 private:
  struct Page {
    int64_t offset, granule;
  };

  /// Loads page `page_`'s lacing table and body.
  bool LoadPage() {
    if (page_ >= pages_.size()) return false;
    uint8_t h[27 + 255];
    int64_t at = pages_[page_].offset;
    range_.ReadAt(at, h, 27);
    int nseg = h[26];
    range_.ReadAt(at + 27, h + 27, size_t(nseg));
    lacing_.assign(h + 27, h + 27 + nseg);
    int64_t body = 0;
    for (uint8_t l : lacing_) body += l;
    body_.resize(size_t(body));
    range_.ReadAt(at + 27 + nseg, body_.data(), body_.size());
    body_pos_ = 0;
    seg_ = 0;
    loaded_ = page_;
    return true;
  }

  bool NextPacket(std::vector<uint8_t>* out) {
    for (;;) {
      if (loaded_ != page_ || seg_ >= lacing_.size()) {
        if (loaded_ == page_ && seg_ >= lacing_.size()) page_++;
        if (!LoadPage()) return false;
      }
      while (seg_ < lacing_.size()) {
        uint8_t l = lacing_[seg_++];
        partial_.insert(partial_.end(), body_.begin() + long(body_pos_), body_.begin() + long(body_pos_ + l));
        body_pos_ += l;
        if (l < 255) {  // packet complete
          out->swap(partial_);
          partial_.clear();
          return true;
        }
      }
    }
  }

  Range range_;
  std::vector<Page> pages_;
  size_t page_ = 0, loaded_ = SIZE_MAX, seg_ = 0, audio_start_page_ = 0;
  std::vector<uint8_t> lacing_, body_, partial_;
  size_t body_pos_ = 0;
  OpusMSDecoder* dec_ = nullptr;
  int channels_ = 0, preskip_ = 0;
  int64_t frames_ = -1, skip_ = 0;
  float pcm_[5760 * 8];
  int pcm_pos_ = 0, pcm_len_ = 0;
};

// ---- Everything Core Audio reads (ExtAudioFile) ----------------------------------------------
class ExtDecoder : public PcmDecoder {
 public:
  ~ExtDecoder() override {
    if (file_) ExtAudioFileDispose(file_);
    if (!temp_.empty()) unlink(temp_.c_str());
  }
  bool Open(int fd, int64_t offset, int64_t length) {
    struct stat st;
    char path[MAXPATHLEN];
    if (fstat(fd, &st) != 0) return false;
    if (length < 0) length = st.st_size - offset;
    NSString* p;
    if (offset == 0 && length >= st.st_size && fcntl(fd, F_GETPATH, path) == 0) {
      p = [NSString stringWithUTF8String:path];
    } else {  // a slice (e.g. a raw resource inside the APK): copy it out
      p = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"andropkg-pcm-%@",
                                                                                            [NSUUID UUID].UUIDString]];
      int out = open(p.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (out < 0) return false;
      std::vector<uint8_t> buf(1 << 16);
      for (int64_t done = 0; done < length;) {
        ssize_t r = pread(fd, buf.data(), size_t(std::min<int64_t>(int64_t(buf.size()), length - done)), offset + done);
        if (r <= 0) break;
        write(out, buf.data(), size_t(r));
        done += r;
      }
      close(out);
      temp_ = p.fileSystemRepresentation;
    }
    if (ExtAudioFileOpenURL((__bridge CFURLRef)[NSURL fileURLWithPath:p], &file_) != noErr) return false;
    AudioStreamBasicDescription in = {};
    UInt32 sz = sizeof in;
    if (ExtAudioFileGetProperty(file_, kExtAudioFileProperty_FileDataFormat, &sz, &in) != noErr) return false;
    rate_ = int(in.mSampleRate);
    channels_ = int(in.mChannelsPerFrame);
    if (rate_ <= 0 || channels_ <= 0) return false;
    AudioStreamBasicDescription f = {};
    f.mSampleRate = in.mSampleRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = UInt32(channels_);
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = f.mBytesPerPacket = UInt32(4 * channels_);
    f.mFramesPerPacket = 1;
    if (ExtAudioFileSetProperty(file_, kExtAudioFileProperty_ClientDataFormat, sizeof f, &f) != noErr) return false;
    SInt64 total = 0;
    sz = sizeof total;
    if (ExtAudioFileGetProperty(file_, kExtAudioFileProperty_FileLengthFrames, &sz, &total) == noErr) frames_ = total;
    return true;
  }
  int rate() const override { return rate_; }
  int channels() const override { return channels_; }
  int64_t frames() const override { return frames_; }
  int Read(float* out, int max_frames) override {
    AudioBufferList list;
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = UInt32(channels_);
    list.mBuffers[0].mDataByteSize = UInt32(max_frames * channels_ * 4);
    list.mBuffers[0].mData = out;
    UInt32 n = UInt32(max_frames);
    if (ExtAudioFileRead(file_, &n, &list) != noErr) return 0;
    return int(n);
  }
  bool Seek(int64_t frame) override { return ExtAudioFileSeek(file_, frame) == noErr; }

 private:
  ExtAudioFileRef file_ = nullptr;
  std::string temp_;
  int rate_ = 0, channels_ = 0;
  int64_t frames_ = -1;
};

}  // namespace

std::unique_ptr<PcmDecoder> OpenDecoder(int fd, int64_t offset, int64_t length) {
  uint8_t head[64] = {};
  ssize_t n = pread(fd, head, sizeof head, offset);
  if (n <= 0) return nullptr;
  if (IsOgg(head, size_t(n))) {
    // The first page carries the codec's identification header.
    if (n >= 36 && !memcmp(head + 28, "OpusHead", 8)) {
      auto d = std::make_unique<OpusFileDecoder>();
      if (d->Open(fd, offset, length)) return d;
      return nullptr;
    }
    auto d = std::make_unique<VorbisDecoder>();
    if (d->Open(fd, offset, length)) return d;
    return nullptr;
  }
  auto d = std::make_unique<ExtDecoder>();
  if (d->Open(fd, offset, length)) return d;
  return nullptr;
}

}  // namespace andropkg_media
