#pragma once

#include <cstdint>

namespace soloud_offline {

/// Result of an offline decode. Mirrored by `OfflineDecodeResult` in Dart.
typedef enum OfflineDecodeResult {
  OFFLINE_DECODE_OK = 0,
  /// A null pointer was passed.
  OFFLINE_DECODE_INVALID_PARAMETER = 1,
  /// The file could not be opened or read.
  OFFLINE_DECODE_FILE_ERROR = 2,
  /// Nothing on this platform recognised the file, or it holds no audio.
  OFFLINE_DECODE_UNSUPPORTED_FORMAT = 3,
  /// The format was recognised but decoding it failed, or allocation did.
  OFFLINE_DECODE_FAILED = 4,
} OfflineDecodeResult;

/// Decodes a whole audio file to interleaved 32-bit float PCM.
///
/// Covers what the engine can load into memory, including the platform-decoded
/// formats and Ogg Opus. It touches no engine state, so it works whether or not
/// the engine is up, and two decodes can overlap.
///
/// On success [samples] is a buffer of `frames * channels` floats, to be released
/// with [freeDecodedPcm]. On failure the outputs are left untouched.
OfflineDecodeResult decodeFileToPcm(const char *path, float **samples,
                                    int64_t *frames, int *channels,
                                    int *sampleRate);

void freeDecodedPcm(float *samples);

}  // namespace soloud_offline
