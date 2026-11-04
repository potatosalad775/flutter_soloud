#pragma once

#include <cstdint>

namespace soloud_offline {

/// Result of an offline decode. Mirrored by `OfflineDecodeResult` in Dart.
typedef enum OfflineDecodeResult {
  OFFLINE_DECODE_OK = 0,
  /// A null pointer or an empty or negative range was passed.
  OFFLINE_DECODE_INVALID_PARAMETER = 1,
  /// The file could not be opened or read.
  OFFLINE_DECODE_FILE_ERROR = 2,
  /// Nothing on this platform recognised the file, or it holds no audio.
  OFFLINE_DECODE_UNSUPPORTED_FORMAT = 3,
  /// The format was recognised but decoding it failed, or allocation did.
  OFFLINE_DECODE_FAILED = 4,
} OfflineDecodeResult;

/// Decodes an audio file, or part of one, to interleaved 32-bit float PCM.
///
/// Covers WAV, FLAC, MP3, Ogg Opus, Vorbis and FLAC, and whatever else the
/// platform's codecs read. A file with no signature it recognises is refused
/// rather than guessed at. It touches no engine state, so it works whether or not
/// the engine is up, and two decodes can overlap.
///
/// [startMicros] and [endMicros] select a range, with a negative [endMicros]
/// meaning the end of the file. Only the range is held in memory and decoding
/// stops at its end, except for platform-decoded formats, which decode the whole
/// file first. A range that holds no audio is OFFLINE_DECODE_UNSUPPORTED_FORMAT.
///
/// On success [samples] is a buffer of `frames * channels` floats, to be released
/// with [freeDecodedPcm]. On failure the outputs are left untouched.
OfflineDecodeResult decodeFileToPcm(const char *path, int64_t startMicros,
                                    int64_t endMicros, float **samples,
                                    int64_t *frames, int *channels,
                                    int *sampleRate);

void freeDecodedPcm(void *samples);

}  // namespace soloud_offline
