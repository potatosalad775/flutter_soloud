#include "offline_decoder.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include "../audiobuffer/stream_decoder.h"
#include "soloud_file.h"
#include "soloud_wav.h"

namespace soloud_offline {

namespace {

/// Opus always decodes at this rate, whatever the source was.
constexpr int kOpusSampleRate = 48000;

OfflineDecodeResult copyOut(const float *source, size_t count, float **samples) {
  auto *buffer = static_cast<float *>(std::malloc(count * sizeof(float)));
  if (buffer == nullptr) {
    return OFFLINE_DECODE_FAILED;
  }
  std::memcpy(buffer, source, count * sizeof(float));
  *samples = buffer;
  return OFFLINE_DECODE_OK;
}

/// Wav holds planar data, each channel [SoLoud::Wav::mSampleCount] apart, of
/// which only [SoLoud::Wav::mActualSampleCount] frames are valid.
OfflineDecodeResult interleave(const SoLoud::Wav &wav, float **samples,
                               int64_t *frames, int *channels, int *sampleRate) {
  const size_t frameCount = wav.mActualSampleCount;
  const size_t channelCount = wav.mChannels;
  if (frameCount == 0 || channelCount == 0 || wav.mData == nullptr) {
    return OFFLINE_DECODE_UNSUPPORTED_FORMAT;
  }

  auto *buffer =
      static_cast<float *>(std::malloc(frameCount * channelCount * sizeof(float)));
  if (buffer == nullptr) {
    return OFFLINE_DECODE_FAILED;
  }
  for (size_t ch = 0; ch < channelCount; ch++) {
    const float *plane = wav.mData + ch * wav.mSampleCount;
    for (size_t i = 0; i < frameCount; i++) {
      buffer[i * channelCount + ch] = plane[i];
    }
  }

  *samples = buffer;
  *frames = static_cast<int64_t>(frameCount);
  *channels = static_cast<int>(channelCount);
  *sampleRate = static_cast<int>(wav.mBaseSamplerate);
  return OFFLINE_DECODE_OK;
}

/// Ogg Opus and Ogg FLAC, which SoLoud::Wav does not read. The loop is
/// `decodeOggXiphBufferStream` in player.cpp, without the stream at the end.
OfflineDecodeResult decodeOggXiph(std::vector<unsigned char> &bytes,
                                  float **samples, int64_t *frames,
                                  int *channels, int *sampleRate) {
  int rate = kOpusSampleRate;
  int channelCount = 2;
  auto ignoreMetadata = [](AudioMetadata) {};

  StreamDecoder decoder;
  auto [pcm, error] = decoder.decode(bytes, &rate, &channelCount, ignoreMetadata);
  decoder.setDataEnded();

  std::vector<unsigned char> noMoreData;
  int emptyPasses = 0;
  while (error == DecoderError::NoError) {
    auto [more, moreError] =
        decoder.decode(noMoreData, &rate, &channelCount, ignoreMetadata);
    error = moreError;
    if (!more.empty()) {
      pcm.insert(pcm.end(), more.begin(), more.end());
      emptyPasses = 0;
    }
    if (!decoder.hasPendingData() || (more.empty() && ++emptyPasses >= 64)) {
      break;
    }
  }

  if (error == DecoderError::FormatNotSupported) {
    return OFFLINE_DECODE_UNSUPPORTED_FORMAT;
  }
  if (error != DecoderError::NoError) {
    return OFFLINE_DECODE_FAILED;
  }
  if (pcm.empty() || channelCount <= 0 || rate <= 0) {
    return OFFLINE_DECODE_UNSUPPORTED_FORMAT;
  }

  const OfflineDecodeResult copied = copyOut(pcm.data(), pcm.size(), samples);
  if (copied != OFFLINE_DECODE_OK) {
    return copied;
  }
  *frames = static_cast<int64_t>(pcm.size() / channelCount);
  *channels = channelCount;
  *sampleRate = rate;
  return OFFLINE_DECODE_OK;
}

/// DiskFile rather than ifstream: it opens UTF-8 paths on Windows.
bool readWholeFile(const char *path, std::vector<unsigned char> &bytes) {
  SoLoud::DiskFile file;
  if (file.open(path) != SoLoud::SO_NO_ERROR) {
    return false;
  }
  const unsigned int length = file.length();
  if (length == 0) {
    return false;
  }
  bytes.resize(length);
  return file.read(bytes.data(), length) == length;
}

}  // namespace

OfflineDecodeResult decodeFileToPcm(const char *path, float **samples,
                                    int64_t *frames, int *channels,
                                    int *sampleRate) {
  if (path == nullptr || samples == nullptr || frames == nullptr ||
      channels == nullptr || sampleRate == nullptr) {
    return OFFLINE_DECODE_INVALID_PARAMETER;
  }

  {
    SoLoud::Wav wav;
    if (wav.load(path) == SoLoud::SO_NO_ERROR) {
      return interleave(wav, samples, frames, channels, sampleRate);
    }
  }

  std::vector<unsigned char> bytes;
  if (!readWholeFile(path, bytes)) {
    return OFFLINE_DECODE_FILE_ERROR;
  }
  if (bytes.size() >= 4 && std::memcmp(bytes.data(), "OggS", 4) == 0) {
    return decodeOggXiph(bytes, samples, frames, channels, sampleRate);
  }
  return OFFLINE_DECODE_UNSUPPORTED_FORMAT;
}

void freeDecodedPcm(float *samples) { std::free(samples); }

}  // namespace soloud_offline
