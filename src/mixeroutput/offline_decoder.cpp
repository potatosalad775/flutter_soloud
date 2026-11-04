#include "offline_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../audiobuffer/stream_decoder.h"
#include "../native_decoder/native_audio_decoder.h"
#include "../soloud/src/audiosource/wav/dr_flac.h"
#include "../soloud/src/audiosource/wav/dr_mp3.h"
#include "../soloud/src/audiosource/wav/dr_wav.h"
#include "soloud_file.h"

namespace soloud_offline {

namespace {

/// Frames decoded per block, which also bounds the working memory.
constexpr size_t kBlockFrames = 4096;

/// Encoded bytes fed to the Ogg decoders at a time.
constexpr unsigned int kOggChunkBytes = 64 * 1024;

/// Enough to see past the zero padding some MP3 files start with.
constexpr size_t kHeaderBytes = 4096;

/// Collects the requested part of the audio into one interleaved buffer, which
/// the caller takes over. Nothing outside the range is ever held, so memory
/// follows the length of the range and not of the file.
class PcmSink {
 public:
  PcmSink(int64_t startMicros, int64_t endMicros)
      : mStartMicros(startMicros), mEndMicros(endMicros) {}

  ~PcmSink() { std::free(mBuffer); }

  PcmSink(const PcmSink &) = delete;
  PcmSink &operator=(const PcmSink &) = delete;

  void setFormat(int channels, int sampleRate) {
    mChannels = channels;
    mSampleRate = sampleRate;
    mStartFrame = toFrame(mStartMicros);
    mEndFrame = mEndMicros < 0 ? -1 : toFrame(mEndMicros);
  }

  int64_t startFrame() const { return mStartFrame; }

  /// False once the end of the range has been reached.
  bool wantsMore() const { return mEndFrame < 0 || mPosition < mEndFrame; }

  /// Avoids growing the buffer when the length is known up front.
  bool reserve(int64_t totalFrames) {
    const int64_t end = mEndFrame < 0 ? totalFrames : std::min(totalFrames, mEndFrame);
    return end <= mStartFrame || ensureCapacity(end - mStartFrame);
  }

  /// Records that the decoder itself moved to [frame].
  void seekedTo(int64_t frame) { mPosition = frame; }

  /// Takes the next [frames] frames of the file. False if allocation failed.
  bool append(const float *interleaved, int64_t frames) {
    const int64_t first = std::max<int64_t>(mStartFrame, mPosition);
    const int64_t pastEnd = mPosition + frames;
    const int64_t last = mEndFrame < 0 ? pastEnd : std::min(pastEnd, mEndFrame);
    if (last > first) {
      const int64_t count = last - first;
      if (!ensureCapacity(mFrames + count)) {
        return false;
      }
      std::memcpy(mBuffer + mFrames * mChannels,
                  interleaved + (first - mPosition) * mChannels,
                  static_cast<size_t>(count * mChannels) * sizeof(float));
      mFrames += count;
    }
    mPosition = pastEnd;
    return true;
  }

  /// Hands over the buffer. A range past the end of the file holds no audio.
  OfflineDecodeResult finish(float **samples, int64_t *frames, int *channels,
                             int *sampleRate) {
    if (mFrames == 0 || mChannels <= 0 || mSampleRate <= 0) {
      return OFFLINE_DECODE_UNSUPPORTED_FORMAT;
    }
    const size_t bytes = static_cast<size_t>(mFrames * mChannels) * sizeof(float);
    if (auto *fitted = static_cast<float *>(std::realloc(mBuffer, bytes))) {
      mBuffer = fitted;
    }
    *samples = mBuffer;
    *frames = mFrames;
    *channels = mChannels;
    *sampleRate = mSampleRate;
    mBuffer = nullptr;
    return OFFLINE_DECODE_OK;
  }

 private:
  int64_t toFrame(int64_t micros) const {
    return std::llround(static_cast<double>(micros) * mSampleRate / 1000000.0);
  }

  bool ensureCapacity(int64_t frames) {
    if (frames <= mCapacity) {
      return true;
    }
    const int64_t grown = std::max(frames, mCapacity + mCapacity / 2);
    const size_t bytes = static_cast<size_t>(grown * mChannels) * sizeof(float);
    auto *buffer = static_cast<float *>(std::realloc(mBuffer, bytes));
    if (buffer == nullptr) {
      return false;
    }
    mBuffer = buffer;
    mCapacity = grown;
    return true;
  }

  const int64_t mStartMicros;
  const int64_t mEndMicros;
  int mChannels = 0;
  int mSampleRate = 0;
  int64_t mStartFrame = 0;
  int64_t mEndFrame = -1;
  int64_t mPosition = 0;
  float *mBuffer = nullptr;
  int64_t mFrames = 0;
  int64_t mCapacity = 0;
};

size_t readFromFile(void *file, void *out, size_t bytes) {
  return static_cast<SoLoud::File *>(file)->read(
      static_cast<unsigned char *>(out), static_cast<unsigned int>(bytes));
}

/// Moves to [offset], from the start of the file when [fromStart].
void seekInFile(void *file, int offset, bool fromStart) {
  auto *f = static_cast<SoLoud::File *>(file);
  f->seek(fromStart ? offset : offset + static_cast<int>(f->pos()));
}

drwav_bool32 seekWav(void *file, int offset, drwav_seek_origin origin) {
  seekInFile(file, offset, origin == DRWAV_SEEK_SET);
  return DRWAV_TRUE;
}

drflac_bool32 seekFlac(void *file, int offset, drflac_seek_origin origin) {
  seekInFile(file, offset, origin == DRFLAC_SEEK_SET);
  return DRFLAC_TRUE;
}

drmp3_bool32 seekMp3(void *file, int offset, drmp3_seek_origin origin) {
  seekInFile(file, offset, origin == DRMP3_SEEK_SET);
  return DRMP3_TRUE;
}

enum class FileKind { Wav, Flac, Mp3, Ogg, Native, Unknown };

bool startsWith(const unsigned char *header, size_t size, const char *tag) {
  const size_t length = std::strlen(tag);
  return size >= length && std::memcmp(header, tag, length) == 0;
}

/// An MPEG audio frame header, ruling out the reserved values. ADTS shares the
/// sync but has layer 0, so it does not match.
bool isMp3Frame(const unsigned char *h, size_t size) {
  if (size < 3 || h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) {
    return false;
  }
  const int version = (h[1] >> 3) & 0x03;
  const int layer = (h[1] >> 1) & 0x03;
  const int bitrate = (h[2] >> 4) & 0x0F;
  const int rate = (h[2] >> 2) & 0x03;
  return version != 1 && layer != 0 && bitrate != 15 && rate != 3;
}

/// Containers no decoder here reads, which only a platform codec might.
bool isOtherPlatformContainer(const unsigned char *h, size_t size) {
  static const unsigned char kAsfGuid[] = {0x30, 0x26, 0xB2, 0x75,
                                           0x8E, 0x66, 0xCF, 0x11};
  if (startsWith(h, size, "caff")) {
    return true;
  }
  if (size >= 12 && startsWith(h, size, "FORM") &&
      (std::memcmp(h + 8, "AIFF", 4) == 0 || std::memcmp(h + 8, "AIFC", 4) == 0)) {
    return true;
  }
  return size >= sizeof(kAsfGuid) && std::memcmp(h, kAsfGuid, sizeof(kAsfGuid)) == 0;
}

/// Decides by signature rather than by asking each decoder in turn. dr_mp3
/// resynchronises on anything, so an unrecognised file would otherwise decode
/// to a fraction of a second of noise and report success.
FileKind classify(const unsigned char *h, size_t size) {
  if (startsWith(h, size, "OggS")) {
    return FileKind::Ogg;
  }
  if (size >= 12 && startsWith(h, size, "RIFF") && std::memcmp(h + 8, "WAVE", 4) == 0) {
    return FileKind::Wav;
  }
  if (startsWith(h, size, "fLaC")) {
    return FileKind::Flac;
  }
  if (startsWith(h, size, "ID3")) {
    return FileKind::Mp3;
  }
  if (NativeAudioDecoder::isSupportedNativeFormat(h, size) ||
      isOtherPlatformContainer(h, size)) {
    return FileKind::Native;
  }
  size_t skipped = 0;
  while (skipped < size && h[skipped] == 0) {
    skipped++;
  }
  return isMp3Frame(h + skipped, size - skipped) ? FileKind::Mp3 : FileKind::Unknown;
}

/// `opened == false` means the file was not what its signature promised, so a
/// platform codec gets to try. Otherwise `ok` is the decode's real outcome.
struct StreamOutcome {
  bool opened;
  bool ok;
};

StreamOutcome decodeWav(SoLoud::File &file, PcmSink &sink) {
  drwav wav;
  if (!drwav_init(&wav, readFromFile, seekWav, nullptr, &file, nullptr)) {
    return {false, false};
  }
  sink.setFormat(wav.channels, static_cast<int>(wav.sampleRate));
  bool ok = sink.reserve(static_cast<int64_t>(wav.totalPCMFrameCount));
  if (ok && sink.startFrame() > 0 &&
      drwav_seek_to_pcm_frame(&wav, static_cast<drwav_uint64>(sink.startFrame()))) {
    sink.seekedTo(sink.startFrame());
  }
  std::vector<float> block(kBlockFrames * wav.channels);
  while (ok && sink.wantsMore()) {
    const auto got = drwav_read_pcm_frames_f32(&wav, kBlockFrames, block.data());
    if (got == 0) {
      break;
    }
    ok = sink.append(block.data(), static_cast<int64_t>(got));
  }
  drwav_uninit(&wav);
  return {true, ok};
}

StreamOutcome decodeFlac(SoLoud::File &file, PcmSink &sink) {
  drflac *flac = drflac_open(readFromFile, seekFlac, nullptr, &file, nullptr);
  if (flac == nullptr) {
    return {false, false};
  }
  sink.setFormat(flac->channels, static_cast<int>(flac->sampleRate));
  bool ok = sink.reserve(static_cast<int64_t>(flac->totalPCMFrameCount));
  if (ok && sink.startFrame() > 0 &&
      drflac_seek_to_pcm_frame(flac, static_cast<drflac_uint64>(sink.startFrame()))) {
    sink.seekedTo(sink.startFrame());
  }
  std::vector<float> block(kBlockFrames * flac->channels);
  while (ok && sink.wantsMore()) {
    const auto got = drflac_read_pcm_frames_f32(flac, kBlockFrames, block.data());
    if (got == 0) {
      break;
    }
    ok = sink.append(block.data(), static_cast<int64_t>(got));
  }
  drflac_close(flac);
  return {true, ok};
}

/// Reads up to the start of the range rather than seeking: dr_mp3's own seek
/// decodes from the top anyway, so this costs the same and is exact.
StreamOutcome decodeMp3(SoLoud::File &file, PcmSink &sink) {
  drmp3 mp3;
  if (!drmp3_init(&mp3, readFromFile, seekMp3, nullptr, nullptr, &file, nullptr)) {
    return {false, false};
  }
  sink.setFormat(static_cast<int>(mp3.channels), static_cast<int>(mp3.sampleRate));
  bool ok = true;
  std::vector<float> block(kBlockFrames * mp3.channels);
  while (ok && sink.wantsMore()) {
    const auto got = drmp3_read_pcm_frames_f32(&mp3, kBlockFrames, block.data());
    if (got == 0) {
      break;
    }
    ok = sink.append(block.data(), static_cast<int64_t>(got));
  }
  drmp3_uninit(&mp3);
  return {true, ok};
}

/// Ogg Opus, Vorbis and FLAC, through the decoders the engine's own Ogg path
/// uses. Fed in chunks, so a range stops reading at its end.
OfflineDecodeResult decodeOgg(SoLoud::File &file, PcmSink &sink) {
  int rate = 48000;
  int channelCount = 2;
  auto ignoreMetadata = [](AudioMetadata) {};

  StreamDecoder decoder;
  std::vector<unsigned char> chunk;
  bool formatKnown = false;
  bool endOfFile = false;
  int emptyPasses = 0;

  while (sink.wantsMore() || !formatKnown) {
    if (!endOfFile) {
      chunk.resize(kOggChunkBytes);
      const unsigned int got = file.read(chunk.data(), kOggChunkBytes);
      chunk.resize(got);
      endOfFile = got < kOggChunkBytes;
      if (endOfFile) {
        decoder.setDataEnded();
      }
    } else {
      chunk.clear();
    }

    auto [pcm, error] = decoder.decode(chunk, &rate, &channelCount, ignoreMetadata);
    if (error == DecoderError::FormatNotSupported) {
      return OFFLINE_DECODE_UNSUPPORTED_FORMAT;
    }
    if (error != DecoderError::NoError) {
      return OFFLINE_DECODE_FAILED;
    }
    if (!pcm.empty()) {
      if (channelCount <= 0 || rate <= 0) {
        return OFFLINE_DECODE_UNSUPPORTED_FORMAT;
      }
      if (!formatKnown) {
        sink.setFormat(channelCount, rate);
        formatKnown = true;
      }
      if (!sink.append(pcm.data(), static_cast<int64_t>(pcm.size() / channelCount))) {
        return OFFLINE_DECODE_FAILED;
      }
      emptyPasses = 0;
    }
    if (endOfFile && (!decoder.hasPendingData() || (pcm.empty() && ++emptyPasses >= 64))) {
      break;
    }
  }
  return OFFLINE_DECODE_OK;
}

/// Platform codecs only offer the whole file, so this is the one path whose
/// memory follows the file and not the range.
OfflineDecodeResult decodeNative(const char *path, PcmSink &sink) {
  DecodedAudioData decoded = NativeAudioDecoder::decodeFile(path);
  if (!decoded.success || decoded.samples == nullptr || decoded.sampleCount == 0 ||
      decoded.channels == 0) {
    decoded.freeSamples();
    return OFFLINE_DECODE_UNSUPPORTED_FORMAT;
  }

  const size_t channels = decoded.channels;
  const size_t total = decoded.sampleCount;
  sink.setFormat(static_cast<int>(channels), static_cast<int>(decoded.sampleRate));

  OfflineDecodeResult result = OFFLINE_DECODE_OK;
  if (!sink.reserve(static_cast<int64_t>(total))) {
    result = OFFLINE_DECODE_FAILED;
  } else {
    size_t frame = static_cast<size_t>(
        std::min<int64_t>(std::max<int64_t>(sink.startFrame(), 0), total));
    sink.seekedTo(static_cast<int64_t>(frame));
    std::vector<float> block(kBlockFrames * channels);
    while (frame < total && sink.wantsMore()) {
      const size_t count = std::min(kBlockFrames, total - frame);
      for (size_t ch = 0; ch < channels; ch++) {
        const float *plane = decoded.samples + ch * total + frame;
        for (size_t i = 0; i < count; i++) {
          block[i * channels + ch] = plane[i];
        }
      }
      if (!sink.append(block.data(), static_cast<int64_t>(count))) {
        result = OFFLINE_DECODE_FAILED;
        break;
      }
      frame += count;
    }
  }
  decoded.freeSamples();
  return result;
}

}  // namespace

OfflineDecodeResult decodeFileToPcm(const char *path, int64_t startMicros,
                                    int64_t endMicros, float **samples,
                                    int64_t *frames, int *channels,
                                    int *sampleRate) {
  if (path == nullptr || samples == nullptr || frames == nullptr ||
      channels == nullptr || sampleRate == nullptr || startMicros < 0 ||
      (endMicros >= 0 && endMicros <= startMicros)) {
    return OFFLINE_DECODE_INVALID_PARAMETER;
  }

  // DiskFile rather than ifstream: it opens UTF-8 paths on Windows.
  SoLoud::DiskFile file;
  if (file.open(path) != SoLoud::SO_NO_ERROR) {
    return OFFLINE_DECODE_FILE_ERROR;
  }
  unsigned char header[kHeaderBytes];
  const size_t headerSize = file.read(header, sizeof(header));
  if (headerSize == 0) {
    return OFFLINE_DECODE_FILE_ERROR;
  }
  file.seek(0);

  const FileKind kind = classify(header, headerSize);
  if (kind == FileKind::Unknown) {
    return OFFLINE_DECODE_UNSUPPORTED_FORMAT;
  }

  PcmSink sink(startMicros, endMicros);
  OfflineDecodeResult result;
  if (kind == FileKind::Ogg) {
    result = decodeOgg(file, sink);
  } else if (kind == FileKind::Native) {
    result = decodeNative(path, sink);
  } else {
    const StreamOutcome outcome = kind == FileKind::Wav    ? decodeWav(file, sink)
                                  : kind == FileKind::Flac ? decodeFlac(file, sink)
                                                           : decodeMp3(file, sink);
    if (!outcome.opened) {
      result = decodeNative(path, sink);
    } else {
      result = outcome.ok ? OFFLINE_DECODE_OK : OFFLINE_DECODE_FAILED;
    }
  }
  if (result != OFFLINE_DECODE_OK) {
    return result;
  }
  return sink.finish(samples, frames, channels, sampleRate);
}

void freeDecodedPcm(void *samples) { std::free(samples); }

}  // namespace soloud_offline
