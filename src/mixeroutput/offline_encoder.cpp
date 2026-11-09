#include "offline_encoder.h"

#include <cstdio>
#include <memory>
#include <vector>

#include "mixer_output_encoder.h"

#if defined(_WIN32) || defined(_WIN64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace soloud_offline {

namespace {

/// Frames handed to the encoder per call. Large enough that the per-call
/// overhead disappears, small enough that the intermediate vectors stay in
/// cache and a long file does not balloon peak memory.
constexpr int64_t kFramesPerBlock = 8192;

/// A RIFF chunk size is 32 bits and also counts the rest of the header, so WAV
/// output past this wraps round to a file that plays only its tail.
constexpr uint64_t kMaxWavPayloadBytes = 0xFFFFFFFFull - 36;

#if defined(_WIN32) || defined(_WIN64)
/// The paths are UTF-8, which the narrow CRT functions would read as the
/// system's code page.
std::wstring widen(const std::string &path) {
  const int length = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  if (length <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], length);
  return wide;
}
#endif

std::FILE *openForWriting(const std::string &path) {
#if defined(_WIN32) || defined(_WIN64)
  // Readable too, because FLAC's header is patched by reading part of it back.
  return _wfopen(widen(path).c_str(), L"w+b");
#else
  return std::fopen(path.c_str(), "w+b");
#endif
}

void removeFile(const std::string &path) {
#if defined(_WIN32) || defined(_WIN64)
  _wremove(widen(path).c_str());
#else
  std::remove(path.c_str());
#endif
}

#if defined(_WIN32) || defined(_WIN64)
bool fileIdentity(const std::string &path, BY_HANDLE_FILE_INFORMATION &info) {
  // No access rights are needed to ask, and sharing everything means a file open
  // elsewhere is still seen.
  HANDLE handle = CreateFileW(widen(path).c_str(), 0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  const BOOL got = GetFileInformationByHandle(handle, &info);
  CloseHandle(handle);
  return got != 0;
}
#endif

}  // namespace

bool isSameFile(const char *a, const char *b) {
#if defined(_WIN32) || defined(_WIN64)
  BY_HANDLE_FILE_INFORMATION first;
  BY_HANDLE_FILE_INFORMATION second;
  return fileIdentity(a, first) && fileIdentity(b, second) &&
         first.dwVolumeSerialNumber == second.dwVolumeSerialNumber &&
         first.nFileIndexHigh == second.nFileIndexHigh &&
         first.nFileIndexLow == second.nFileIndexLow;
#else
  struct stat first;
  struct stat second;
  return stat(a, &first) == 0 && stat(b, &second) == 0 && first.st_dev == second.st_dev &&
         first.st_ino == second.st_ino;
#endif
}

OfflineFileWriter::OfflineFileWriter(MixerOutputFormat format, int bitrate,
                                     const char *outputPath)
    : mFormat(format), mBitrate(bitrate), mPath(outputPath) {}

OfflineFileWriter::~OfflineFileWriter() { abandon(); }

void OfflineFileWriter::abandon() {
  if (mFile != nullptr) {
    std::fclose(mFile);
    mFile = nullptr;
    removeFile(mPath);
  }
}

bool OfflineFileWriter::writeBytes(const std::vector<uint8_t> &bytes) {
  return bytes.empty() ||
         std::fwrite(bytes.data(), 1, bytes.size(), mFile) == bytes.size();
}

OfflineEncodeResult OfflineFileWriter::open(int sampleRate, int channels) {
  if (sampleRate <= 0 || (channels != 1 && channels != 2)) {
    return OFFLINE_ENCODE_INVALID_PARAMETER;
  }

  mEncoder.reset(MixerOutputEncoder::create(mFormat));
  if (!mEncoder) {
    return OFFLINE_ENCODE_UNSUPPORTED_FORMAT;
  }

  if (mBitrate > 0) {
    mEncoder->setBitrate(mBitrate);
  }
  // Offline: the CPU is paid once, so take the best the encoder can do.
  mEncoder->setComplexity(10);

  if (!mEncoder->initialize(sampleRate, channels)) {
    return OFFLINE_ENCODE_ENCODER_ERROR;
  }

  mFile = openForWriting(mPath);
  if (mFile == nullptr) {
    return OFFLINE_ENCODE_FILE_ERROR;
  }
  mChannels = channels;

  // WAV writes its header up front and patches the size fields at the end;
  // the compressed formats return false here and emit nothing.
  std::vector<uint8_t> header;
  mHasPatchableHeader = mEncoder->writeCurrentHeader(header);
  if (mHasPatchableHeader && !writeBytes(header)) {
    return OFFLINE_ENCODE_FILE_ERROR;
  }
  return OFFLINE_ENCODE_OK;
}

OfflineEncodeResult OfflineFileWriter::write(const float *pcm, int64_t sampleCount) {
  if (mFile == nullptr) {
    return OFFLINE_ENCODE_INVALID_PARAMETER;
  }
  if (mHasPatchableHeader &&
      static_cast<uint64_t>(mSamples + sampleCount) * sizeof(int16_t) > kMaxWavPayloadBytes) {
    return OFFLINE_ENCODE_ENCODER_ERROR;
  }

  const int64_t samplesPerBlock = kFramesPerBlock * mChannels;
  for (int64_t offset = 0; offset < sampleCount; offset += samplesPerBlock) {
    const int64_t remaining = sampleCount - offset;
    const size_t thisBlock = static_cast<size_t>(
        remaining < samplesPerBlock ? remaining : samplesPerBlock);

    if (!mEncoder->encode(pcm + offset, thisBlock, mBlock)) {
      return OFFLINE_ENCODE_ENCODER_ERROR;
    }
    if (!writeBytes(mBlock)) {
      return OFFLINE_ENCODE_FILE_ERROR;
    }
    mBlock.clear();
  }
  mSamples += sampleCount;
  return OFFLINE_ENCODE_OK;
}

/// Writes the frame count into STREAMINFO, which is what lets a decoder report
/// the stream's length. libFLAC would do it itself at the end given a seek
/// callback, but the encoder streams its bytes out rather than owning a file, and
/// a conversion only knows the count once the source has run out.
///
/// STREAMINFO is the first block, after the 4-byte magic and a 4-byte block
/// header. The count is its last 36 bits before the MD5, starting in the low
/// nibble of the byte at file offset 21.
bool OfflineFileWriter::patchFlacLength() {
  const uint64_t frames = static_cast<uint64_t>(mSamples / mChannels);
  unsigned char first = 0;
  if (std::fseek(mFile, 21, SEEK_SET) != 0 ||
      std::fread(&first, 1, 1, mFile) != 1) {
    return false;
  }
  const unsigned char field[5] = {
      static_cast<unsigned char>((first & 0xF0) | ((frames >> 32) & 0x0F)),
      static_cast<unsigned char>(frames >> 24),
      static_cast<unsigned char>(frames >> 16),
      static_cast<unsigned char>(frames >> 8),
      static_cast<unsigned char>(frames),
  };
  return std::fseek(mFile, 21, SEEK_SET) == 0 &&
         std::fwrite(field, 1, sizeof(field), mFile) == sizeof(field);
}

OfflineEncodeResult OfflineFileWriter::finish() {
  if (mFile == nullptr) {
    return OFFLINE_ENCODE_INVALID_PARAMETER;
  }

  std::vector<uint8_t> tail;
  if (!mEncoder->finalize(tail)) {
    return OFFLINE_ENCODE_ENCODER_ERROR;
  }
  if (!writeBytes(tail)) {
    return OFFLINE_ENCODE_FILE_ERROR;
  }

  if (mHasPatchableHeader) {
    // Now that the payload length is known, rewrite the header in place.
    std::vector<uint8_t> header;
    if (!mEncoder->writeCurrentHeader(header)) {
      return OFFLINE_ENCODE_ENCODER_ERROR;
    }
    if (std::fseek(mFile, 0, SEEK_SET) != 0 || !writeBytes(header)) {
      return OFFLINE_ENCODE_FILE_ERROR;
    }
  } else if (mFormat == MIXER_OUTPUT_FLAC && !patchFlacLength()) {
    return OFFLINE_ENCODE_FILE_ERROR;
  }

  // Buffered data goes out here, so this is where a full disk can still show up,
  // and callers delete the source file on the strength of this result.
  if (std::fflush(mFile) != 0 || std::ferror(mFile) != 0) {
    return OFFLINE_ENCODE_FILE_ERROR;
  }
  std::FILE *file = mFile;
  mFile = nullptr;
  if (std::fclose(file) != 0) {
    removeFile(mPath);
    return OFFLINE_ENCODE_FILE_ERROR;
  }
  return OFFLINE_ENCODE_OK;
}

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

  OfflineFileWriter writer(format, bitrate, outputPath);
  OfflineEncodeResult result = writer.open(sampleRate, channels);
  if (result == OFFLINE_ENCODE_OK) {
    result = writer.write(pcm, sampleCount);
  }
  if (result == OFFLINE_ENCODE_OK) {
    result = writer.finish();
  }
  return result;
}

}  // namespace soloud_offline
