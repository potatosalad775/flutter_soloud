#pragma once

#include <cstddef>
#include <cstdint>

#include "../enums.h"

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
  /// The encoder rejected the input format or a block of samples.
  OFFLINE_ENCODE_ENCODER_ERROR = 4,
} OfflineEncodeResult_t;

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
