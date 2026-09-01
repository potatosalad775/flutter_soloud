#include "offline_encoder.h"

#include <cstdio>
#include <memory>
#include <vector>

#include "mixer_output_encoder.h"

namespace soloud_offline {

namespace {

/// Frames handed to the encoder per call. Large enough that the per-call
/// overhead disappears, small enough that the intermediate vectors stay in
/// cache and a long file does not balloon peak memory.
constexpr int64_t kFramesPerBlock = 8192;

/// RAII wrapper so every early return closes the file.
class FileHandle {
 public:
  explicit FileHandle(const char *path) : m_file(std::fopen(path, "wb")) {}
  ~FileHandle() {
    if (m_file != nullptr) {
      std::fclose(m_file);
    }
  }

  FileHandle(const FileHandle &) = delete;
  FileHandle &operator=(const FileHandle &) = delete;

  bool ok() const { return m_file != nullptr; }
  std::FILE *get() const { return m_file; }

  bool write(const std::vector<uint8_t> &bytes) {
    if (bytes.empty()) {
      return true;
    }
    return std::fwrite(bytes.data(), 1, bytes.size(), m_file) == bytes.size();
  }

 private:
  std::FILE *m_file = nullptr;
};

}  // namespace

OfflineEncodeResult encodePcmToFile(const float *pcm, int64_t sampleCount,
                                    int sampleRate, int channels,
                                    MixerOutputFormat format, int bitrate,
                                    const char *outputPath) {
  if (pcm == nullptr || outputPath == nullptr || sampleCount <= 0 ||
      sampleRate <= 0 || (channels != 1 && channels != 2)) {
    return OFFLINE_ENCODE_INVALID_PARAMETER;
  }

  // A partial frame means the caller got its channel count wrong; encoding it
  // would silently swap the channels from that point on.
  if (sampleCount % channels != 0) {
    return OFFLINE_ENCODE_INVALID_PARAMETER;
  }

  std::unique_ptr<MixerOutputEncoder> encoder(
      MixerOutputEncoder::create(format));
  if (!encoder) {
    return OFFLINE_ENCODE_UNSUPPORTED_FORMAT;
  }

  if (bitrate > 0) {
    encoder->setBitrate(bitrate);
  }
  // Offline: the CPU is paid once, so take the best the encoder can do.
  encoder->setComplexity(10);

  // Unlike live capture, an offline encode knows exactly how much audio there
  // is. FLAC needs this to write a usable STREAMINFO.
  encoder->setTotalFrames(sampleCount / channels);

  if (!encoder->initialize(sampleRate, channels)) {
    return OFFLINE_ENCODE_ENCODER_ERROR;
  }

  FileHandle file(outputPath);
  if (!file.ok()) {
    return OFFLINE_ENCODE_FILE_ERROR;
  }

  // WAV writes its header up front and patches the size fields at the end;
  // the compressed formats return false here and emit nothing.
  std::vector<uint8_t> header;
  const bool hasPatchableHeader = encoder->writeCurrentHeader(header);
  if (hasPatchableHeader && !file.write(header)) {
    return OFFLINE_ENCODE_FILE_ERROR;
  }

  std::vector<uint8_t> block;
  const int64_t samplesPerBlock = kFramesPerBlock * channels;

  for (int64_t offset = 0; offset < sampleCount; offset += samplesPerBlock) {
    const int64_t remaining = sampleCount - offset;
    const size_t thisBlock = static_cast<size_t>(
        remaining < samplesPerBlock ? remaining : samplesPerBlock);

    if (!encoder->encode(pcm + offset, thisBlock, block)) {
      return OFFLINE_ENCODE_ENCODER_ERROR;
    }
    if (!file.write(block)) {
      return OFFLINE_ENCODE_FILE_ERROR;
    }
  }

  std::vector<uint8_t> tail;
  if (!encoder->finalize(tail)) {
    return OFFLINE_ENCODE_ENCODER_ERROR;
  }
  if (!file.write(tail)) {
    return OFFLINE_ENCODE_FILE_ERROR;
  }

  if (hasPatchableHeader) {
    // Now that the payload length is known, rewrite the header in place.
    if (!encoder->writeCurrentHeader(header)) {
      return OFFLINE_ENCODE_ENCODER_ERROR;
    }
    if (std::fseek(file.get(), 0, SEEK_SET) != 0) {
      return OFFLINE_ENCODE_FILE_ERROR;
    }
    if (!file.write(header)) {
      return OFFLINE_ENCODE_FILE_ERROR;
    }
  }

  return OFFLINE_ENCODE_OK;
}

}  // namespace soloud_offline
