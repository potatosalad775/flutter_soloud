// Round-trip tests for the offline PCM-to-file encoder.
//
// Build & run with test/run_offline_encode_test.sh
//
// Each format is encoded from a known buffer and decoded back, so the
// assertions are about what a decoder actually recovers — rate, channel
// count, length, and for the lossless formats the samples themselves.

#include "../src/mixeroutput/offline_encoder.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#define DR_WAV_IMPLEMENTATION
#include "../src/soloud/src/audiosource/wav/dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#include "../src/soloud/src/audiosource/wav/dr_flac.h"

using soloud_offline::encodePcmToFile;
using soloud_offline::OFFLINE_ENCODE_ENCODER_ERROR;
using soloud_offline::OFFLINE_ENCODE_FILE_ERROR;
using soloud_offline::OFFLINE_ENCODE_INVALID_PARAMETER;
using soloud_offline::OFFLINE_ENCODE_OK;
using soloud_offline::OFFLINE_ENCODE_UNSUPPORTED_FORMAT;
using soloud_offline::OfflineEncodeResult;

namespace {

int gFailures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    printf("  FAIL: %s\n", what);
    gFailures++;
  }
}

/// Returns by value: two paths are routinely held live at once, so a shared
/// static buffer would silently alias them.
std::string tmpPath(const char *name) {
  const char *dir = std::getenv("TMPDIR");
  std::string base = (dir != nullptr) ? dir : "/tmp/";
  if (!base.empty() && base.back() != '/') {
    base.push_back('/');
  }
  return base + name;
}

/// A 16-bit-exact ramp plus tone, so a lossless round-trip can be compared
/// sample for sample without worrying about float rounding.
std::vector<float> makeQuantizedSignal(int sampleRate, int channels,
                                       double seconds) {
  const size_t frames = static_cast<size_t>(sampleRate * seconds);
  std::vector<float> out;
  out.reserve(frames * static_cast<size_t>(channels));
  for (size_t i = 0; i < frames; ++i) {
    for (int ch = 0; ch < channels; ++ch) {
      const double v =
          0.6 * std::sin(2.0 * M_PI * (440.0 + 110.0 * ch) *
                         static_cast<double>(i) / sampleRate);
      // Snap to the 16-bit grid the lossless encoders quantize to anyway.
      const int16_t q = static_cast<int16_t>(v * 32767.0);
      out.push_back(static_cast<float>(q) / 32767.0f);
    }
  }
  return out;
}

size_t fileSize(const std::string &path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  return f.good() ? static_cast<size_t>(f.tellg()) : 0;
}

void testWavRoundTripIsExact() {
  printf("WAV round-trips bit-exactly\n");
  const int rate = 44100;
  const int channels = 2;
  const std::vector<float> pcm = makeQuantizedSignal(rate, channels, 0.5);
  const std::string path = tmpPath("offline_encode_test.wav");

  const OfflineEncodeResult r =
      encodePcmToFile(pcm.data(), static_cast<int64_t>(pcm.size()), rate,
                      channels, MIXER_OUTPUT_WAV, 0, path.c_str());
  check(r == OFFLINE_ENCODE_OK, "encode succeeded");

  // The header must describe the payload exactly: 44 bytes plus 16-bit PCM.
  const size_t expected = 44 + pcm.size() * sizeof(int16_t);
  printf("  file is %zu bytes (expected %zu)\n", fileSize(path), expected);
  check(fileSize(path) == expected, "size is header plus payload");

  unsigned int outChannels = 0;
  unsigned int outRate = 0;
  drwav_uint64 outFrames = 0;
  float *decoded = drwav_open_file_and_read_pcm_frames_f32(
      path.c_str(), &outChannels, &outRate, &outFrames, nullptr);
  check(decoded != nullptr, "decodes");
  if (decoded == nullptr) {
    return;
  }

  printf("  decoded %llu frames, %u ch, %u Hz\n",
         static_cast<unsigned long long>(outFrames), outChannels, outRate);
  check(outRate == static_cast<unsigned int>(rate), "sample rate survives");
  check(outChannels == static_cast<unsigned int>(channels),
        "channel count survives");
  check(outFrames == pcm.size() / channels, "frame count survives");

  double maxDiff = 0.0;
  const size_t n = std::min<size_t>(
      pcm.size(), static_cast<size_t>(outFrames) * outChannels);
  for (size_t i = 0; i < n; ++i) {
    maxDiff = std::max(maxDiff, std::abs(static_cast<double>(pcm[i]) -
                                         static_cast<double>(decoded[i])));
  }
  printf("  max sample difference: %.3e\n", maxDiff);
  // One 16-bit step of slack; the input was already snapped to that grid.
  check(maxDiff <= 1.0 / 32767.0, "samples survive the round trip");

  drwav_free(decoded, nullptr);
}

void testFlacRoundTripIsExact() {
  printf("FLAC round-trips bit-exactly\n");
  const int rate = 44100;
  const int channels = 2;
  const std::vector<float> pcm = makeQuantizedSignal(rate, channels, 0.5);
  const std::string path = tmpPath("offline_encode_test.flac");

  const OfflineEncodeResult r =
      encodePcmToFile(pcm.data(), static_cast<int64_t>(pcm.size()), rate,
                      channels, MIXER_OUTPUT_FLAC, 0, path.c_str());
  check(r == OFFLINE_ENCODE_OK, "encode succeeded");

  // Lossless, but it is still compression: it must be smaller than WAV.
  const size_t wavSize = 44 + pcm.size() * sizeof(int16_t);
  printf("  %zu bytes vs %zu for WAV\n", fileSize(path), wavSize);
  check(fileSize(path) > 0, "file is non-empty");
  check(fileSize(path) < wavSize, "smaller than the equivalent WAV");

  unsigned int outChannels = 0;
  unsigned int outRate = 0;
  drflac_uint64 outFrames = 0;
  float *decoded = drflac_open_file_and_read_pcm_frames_f32(
      path.c_str(), &outChannels, &outRate, &outFrames, nullptr);
  check(decoded != nullptr, "decodes");
  if (decoded == nullptr) {
    return;
  }

  printf("  decoded %llu frames, %u ch, %u Hz\n",
         static_cast<unsigned long long>(outFrames), outChannels, outRate);
  check(outRate == static_cast<unsigned int>(rate), "sample rate survives");
  check(outChannels == static_cast<unsigned int>(channels),
        "channel count survives");
  check(outFrames == pcm.size() / channels, "frame count survives");

  double maxDiff = 0.0;
  const size_t n = std::min<size_t>(
      pcm.size(), static_cast<size_t>(outFrames) * outChannels);
  for (size_t i = 0; i < n; ++i) {
    maxDiff = std::max(maxDiff, std::abs(static_cast<double>(pcm[i]) -
                                         static_cast<double>(decoded[i])));
  }
  printf("  max sample difference: %.3e\n", maxDiff);
  check(maxDiff <= 1.0 / 32767.0, "FLAC is lossless to the 16-bit grid");

  drflac_free(decoded, nullptr);
}

void testMonoWorks() {
  printf("mono encodes and decodes\n");
  const int rate = 48000;
  const std::vector<float> pcm = makeQuantizedSignal(rate, 1, 0.25);
  const std::string path = tmpPath("offline_encode_test_mono.flac");

  const OfflineEncodeResult r =
      encodePcmToFile(pcm.data(), static_cast<int64_t>(pcm.size()), rate, 1,
                      MIXER_OUTPUT_FLAC, 0, path.c_str());
  check(r == OFFLINE_ENCODE_OK, "encode succeeded");

  unsigned int outChannels = 0;
  unsigned int outRate = 0;
  drflac_uint64 outFrames = 0;
  float *decoded = drflac_open_file_and_read_pcm_frames_f32(
      path.c_str(), &outChannels, &outRate, &outFrames, nullptr);
  check(decoded != nullptr, "decodes");
  if (decoded == nullptr) {
    return;
  }
  check(outChannels == 1, "stays mono");
  check(outRate == static_cast<unsigned int>(rate), "sample rate survives");
  check(outFrames == pcm.size(), "frame count survives");
  drflac_free(decoded, nullptr);
}

void testLossyFormatsProduceValidFiles() {
  printf("Opus and Vorbis produce non-empty Ogg streams\n");
  const int rate = 44100;
  const int channels = 2;
  const std::vector<float> pcm = makeQuantizedSignal(rate, channels, 1.0);

  struct {
    MixerOutputFormat format;
    const char *name;
    const char *magic;
  } cases[] = {
      {MIXER_OUTPUT_OPUS, "offline_encode_test.opus", "OggS"},
      {MIXER_OUTPUT_VORBIS, "offline_encode_test.ogg", "OggS"},
  };

  for (const auto &c : cases) {
    const std::string path = tmpPath(c.name);
    const OfflineEncodeResult r =
        encodePcmToFile(pcm.data(), static_cast<int64_t>(pcm.size()), rate,
                        channels, c.format, 0, path.c_str());
    check(r == OFFLINE_ENCODE_OK, "encode succeeded");

    std::ifstream f(path, std::ios::binary);
    char magic[4] = {0, 0, 0, 0};
    f.read(magic, 4);
    printf("  %s: %zu bytes, magic %.4s\n", c.name, fileSize(path), magic);
    check(std::memcmp(magic, c.magic, 4) == 0, "starts with an Ogg page");
    check(fileSize(path) > 1000, "carries real payload");
    // Lossy: must be well under the uncompressed size.
    check(fileSize(path) < pcm.size() * sizeof(int16_t), "actually compressed");
  }
}

void testBitrateIsHonoured() {
  printf("the Opus bitrate argument changes the file size\n");
  const int rate = 48000;
  const int channels = 2;
  const std::vector<float> pcm = makeQuantizedSignal(rate, channels, 2.0);

  const std::string lowPath = tmpPath("offline_encode_low.opus");
  const std::string highPath = tmpPath("offline_encode_high.opus");

  check(encodePcmToFile(pcm.data(), static_cast<int64_t>(pcm.size()), rate,
                        channels, MIXER_OUTPUT_OPUS, 64000,
                        lowPath.c_str()) == OFFLINE_ENCODE_OK,
        "64 kbps encode succeeded");
  check(encodePcmToFile(pcm.data(), static_cast<int64_t>(pcm.size()), rate,
                        channels, MIXER_OUTPUT_OPUS, 256000,
                        highPath.c_str()) == OFFLINE_ENCODE_OK,
        "256 kbps encode succeeded");

  const size_t low = fileSize(lowPath);
  const size_t high = fileSize(highPath);
  printf("  64 kbps -> %zu bytes, 256 kbps -> %zu bytes\n", low, high);
  check(high > low * 2, "the higher bitrate produces a much larger file");

  // Two seconds at 64 kbps is ~16 kB; allow generous slack for VBR.
  check(low > 8000 && low < 32000, "64 kbps lands in the right ballpark");
}

void testErrorPaths() {
  printf("bad inputs are rejected, each with its own code\n");
  const std::vector<float> pcm(1000, 0.0f);
  const std::string path = tmpPath("offline_encode_should_not_exist.wav");

  check(encodePcmToFile(nullptr, 100, 44100, 2, MIXER_OUTPUT_WAV, 0,
                        path.c_str()) ==
            OFFLINE_ENCODE_INVALID_PARAMETER,
        "null PCM pointer");
  check(encodePcmToFile(pcm.data(), 0, 44100, 2, MIXER_OUTPUT_WAV, 0,
                        path.c_str()) ==
            OFFLINE_ENCODE_INVALID_PARAMETER,
        "zero samples");
  check(encodePcmToFile(pcm.data(), 100, 0, 2, MIXER_OUTPUT_WAV, 0,
                        path.c_str()) ==
            OFFLINE_ENCODE_INVALID_PARAMETER,
        "zero sample rate");
  check(encodePcmToFile(pcm.data(), 100, 44100, 3, MIXER_OUTPUT_WAV, 0,
                        path.c_str()) ==
            OFFLINE_ENCODE_INVALID_PARAMETER,
        "unsupported channel count");
  check(encodePcmToFile(pcm.data(), 101, 44100, 2, MIXER_OUTPUT_WAV, 0,
                        path.c_str()) ==
            OFFLINE_ENCODE_INVALID_PARAMETER,
        "sample count is not a whole number of frames");
  check(encodePcmToFile(pcm.data(), 100, 44100, 2, MIXER_OUTPUT_PCM_S16LE, 0,
                        path.c_str()) == OFFLINE_ENCODE_UNSUPPORTED_FORMAT,
        "raw PCM has no encoder");
  check(encodePcmToFile(pcm.data(), 100, 44100, 2, MIXER_OUTPUT_WAV, 0,
                        "/nonexistent-directory-xyz/out.wav") ==
            OFFLINE_ENCODE_FILE_ERROR,
        "unwritable path");
  check(encodePcmToFile(pcm.data(), 100, 44100, 2, MIXER_OUTPUT_WAV, 0,
                        nullptr) == OFFLINE_ENCODE_INVALID_PARAMETER,
        "null output path");
}

void testExistingFileIsTruncated() {
  printf("an existing file is replaced, not appended to\n");
  const std::string path = tmpPath("offline_encode_truncate.wav");
  const int rate = 44100;

  const std::vector<float> big = makeQuantizedSignal(rate, 2, 1.0);
  check(encodePcmToFile(big.data(), static_cast<int64_t>(big.size()), rate, 2,
                        MIXER_OUTPUT_WAV, 0, path.c_str()) == OFFLINE_ENCODE_OK,
        "first encode succeeded");
  const size_t bigSize = fileSize(path);

  const std::vector<float> small = makeQuantizedSignal(rate, 2, 0.1);
  check(encodePcmToFile(small.data(), static_cast<int64_t>(small.size()), rate,
                        2, MIXER_OUTPUT_WAV, 0, path.c_str()) == OFFLINE_ENCODE_OK,
        "second encode succeeded");
  const size_t smallSize = fileSize(path);

  printf("  %zu bytes then %zu bytes\n", bigSize, smallSize);
  check(smallSize < bigSize, "the shorter encode really shrank the file");
  check(smallSize == 44 + small.size() * sizeof(int16_t),
        "size matches the second payload exactly");
}

}  // namespace

int main() {
  testWavRoundTripIsExact();
  testFlacRoundTripIsExact();
  testMonoWorks();
  testLossyFormatsProduceValidFiles();
  testBitrateIsHonoured();
  testErrorPaths();
  testExistingFileIsTruncated();

  if (gFailures != 0) {
    printf("\n%d FAILURE(S)\n", gFailures);
    return 1;
  }
  printf("\nAll offline encode tests passed.\n");
  return 0;
}
