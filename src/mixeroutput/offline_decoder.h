#pragma once

#include <cstdint>

#include "../enums.h"
#include "offline_encoder.h"

namespace soloud_offline {

/// Result of an offline decode. Mirrored by `OfflineDecodeResult` in Dart.
typedef enum OfflineDecodeResult {
  OFFLINE_DECODE_OK = 0,
  /// A null pointer or an empty or negative range was passed, or a conversion was
  /// asked to write over its own source.
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
/// platform's codecs read, behind any ID3v2 tags. A file with no signature it
/// recognises is refused rather than guessed at. MP3 alone is also looked for
/// behind junk in the first 4 KB, where it takes a run of consecutive frames to
/// count, since a lone frame header turns up by chance. It touches no engine state,
/// so it works whether or not the engine is up, and two decodes can overlap.
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

/// What a conversion did. It succeeded only if both results are OK.
struct OfflineConvertResult {
  OfflineDecodeResult decode = OFFLINE_DECODE_OK;
  OfflineEncodeResult encode = OFFLINE_ENCODE_OK;
  /// The source's channel count, if it got as far as being known. This is what
  /// to report for OFFLINE_ENCODE_TOO_MANY_CHANNELS.
  int channels = 0;
};

/// Writes an audio file, or part of one, to another file, re-encoded as [format].
///
/// Decoded audio goes straight to the encoder a block at a time, so memory does
/// not follow the length of the audio. The exception is a format only a platform
/// codec reads, such as M4A, which still holds all of the decoded PCM once.
///
/// Reads what [decodeFileToPcm] reads, with the same meaning for the range. The
/// encoders take mono and stereo only, and more channels are refused rather than
/// mixed down, because channel order depends on the decoder.
///
/// Unlike decoding first and encoding after, a source can fail after output has
/// begun. On failure no partial file is left at [outputPath]. It is created only
/// once there is audio for it, so a file that cannot be read never touches what
/// was there before. It may not be the source itself.
///
/// Touches no engine state. Runs to completion on the calling thread and takes
/// CPU-seconds on long files, so keep it off any thread that renders audio or
/// drives UI.
OfflineConvertResult convertFile(const char *sourcePath, const char *outputPath,
                                 int64_t startMicros, int64_t endMicros,
                                 MixerOutputFormat format, int bitrate);

}  // namespace soloud_offline
