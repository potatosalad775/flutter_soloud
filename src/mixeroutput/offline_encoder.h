#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../enums.h"

class MixerOutputEncoder;

namespace soloud_offline {

/// Result of an offline encode. Mirrors the granularity the Dart wrapper
/// needs to throw a useful exception, not the full PlayerErrors set.
typedef enum OfflineEncodeResult {
  OFFLINE_ENCODE_OK = 0,
  /// A null pointer, zero-length input, or an out-of-range rate/channel count.
  OFFLINE_ENCODE_INVALID_PARAMETER = 1,
  /// The requested format has no encoder, or the build has NO_XIPH_LIBS and
  /// the format is one of the Xiph ones.
  OFFLINE_ENCODE_UNSUPPORTED_FORMAT = 2,
  /// The output path could not be opened or a write failed part-way.
  OFFLINE_ENCODE_FILE_ERROR = 3,
  /// The encoder rejected the input format or a block of samples, or the audio
  /// is too long for a WAV file, whose sizes are 32-bit.
  OFFLINE_ENCODE_ENCODER_ERROR = 4,
  /// A conversion found more channels than the encoders take, which is two.
  OFFLINE_ENCODE_TOO_MANY_CHANNELS = 5,
} OfflineEncodeResult_t;

/// Whether two UTF-8 paths name one existing file, by identity rather than spelling.
bool isSameFile(const char *a, const char *b);

/// Writes one audio file from interleaved float PCM handed over in pieces, so
/// that nobody has to hold the whole of the audio to encode it.
///
/// Whatever does not reach [finish] is removed from disk, so a failure part-way
/// leaves no partial file for anything to mistake for a finished one.
class OfflineFileWriter {
 public:
  /// [format] a MixerOutputFormat. Only OPUS, VORBIS, FLAC and WAV encode.
  /// [bitrate] target bitrate in bits/second for lossy formats, or 0 for the
  ///   encoder default. Ignored by FLAC and WAV.
  /// [outputPath] UTF-8 path to write. Truncated if it exists.
  OfflineFileWriter(MixerOutputFormat format, int bitrate, const char *outputPath);
  ~OfflineFileWriter();

  OfflineFileWriter(const OfflineFileWriter &) = delete;
  OfflineFileWriter &operator=(const OfflineFileWriter &) = delete;

  /// Creates the encoder and the file. [sampleRate] is the input rate in Hz:
  /// Opus always encodes at 48 kHz and resamples internally, every other format
  /// keeps it. [channels] is 1 or 2.
  OfflineEncodeResult open(int sampleRate, int channels);

  /// Encodes [sampleCount] samples, a whole number of frames.
  OfflineEncodeResult write(const float *pcm, int64_t sampleCount);

  /// Completes the file. Only a result of OFFLINE_ENCODE_OK means it is on disk
  /// in full: the last of the data can still fail to reach it when it is closed.
  OfflineEncodeResult finish();

 private:
  bool writeBytes(const std::vector<uint8_t> &bytes);
  bool patchFlacLength();
  void abandon();

  const MixerOutputFormat mFormat;
  const int mBitrate;
  const std::string mPath;
  std::unique_ptr<MixerOutputEncoder> mEncoder;
  std::FILE *mFile = nullptr;
  bool mHasPatchableHeader = false;
  int mChannels = 0;
  int64_t mSamples = 0;
  std::vector<uint8_t> mBlock;
};

/// Encode an in-memory buffer of interleaved float PCM to a file on disk.
///
/// This is the offline counterpart to the mixer-capture path: it drives the
/// same `MixerOutputEncoder` implementations, but over a caller-supplied
/// buffer instead of the live render ring, and it owns the file itself.
///
/// [pcm] interleaved float samples in [-1.0, 1.0].
/// [sampleCount] total number of samples, i.e. frames * channels.
/// [sampleRate] input sample rate in Hz. Opus always encodes at 48 kHz and
///   resamples internally; every other format keeps the native rate.
/// [channels] 1 or 2.
/// [format] a MixerOutputFormat. Only OPUS, VORBIS, FLAC and WAV encode.
/// [bitrate] target bitrate in bits/second for lossy formats. Pass 0 for the
///   encoder default. Ignored by FLAC and WAV.
/// [outputPath] UTF-8 path to write. Truncated if it exists.
///
/// This runs to completion on the calling thread and takes CPU-seconds on long
/// files, so callers must not invoke it on a thread that renders audio or
/// drives UI. The Dart wrapper hands it to `compute()` for exactly that
/// reason.
OfflineEncodeResult encodePcmToFile(const float *pcm, int64_t sampleCount,
                                    int sampleRate, int channels,
                                    MixerOutputFormat format, int bitrate,
                                    const char *outputPath);

}  // namespace soloud_offline
