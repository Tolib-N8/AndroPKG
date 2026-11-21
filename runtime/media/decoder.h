// andropkg: audio file -> PCM (float, interleaved), for SoundPool samples and for MediaPlayer
// formats AVPlayer cannot play.
//   Ogg Vorbis: AOSP's Tremolo (libvorbisidec, vorbisfile API)
//   Ogg Opus:   libopus, with a small Ogg page reader here
//   the rest:   Core Audio's ExtAudioFile (MP3, AAC/M4A, WAV, FLAC, AIFF, CAF, ...)
#pragma once
#include <stdint.h>

#include <memory>
#include <string>

namespace andropkg_media {

class PcmDecoder {
 public:
  virtual ~PcmDecoder() = default;
  virtual int rate() const = 0;
  virtual int channels() const = 0;
  /// Total frames, or -1 if unknown.
  virtual int64_t frames() const = 0;
  /// Reads up to `max_frames` interleaved frames; returns frames read, 0 at the end.
  virtual int Read(float* out, int max_frames) = 0;
  virtual bool Seek(int64_t frame) = 0;
};

/// Opens [offset, offset + length) of `fd` (length < 0: to the end). The decoder keeps its own
/// duplicate of the descriptor. Returns null if the format is not recognized or not supported.
std::unique_ptr<PcmDecoder> OpenDecoder(int fd, int64_t offset, int64_t length);

/// True if the bytes start an Ogg stream (AVFoundation can't play those).
bool IsOgg(const uint8_t* head, size_t n);

}  // namespace andropkg_media
