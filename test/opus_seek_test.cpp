// Seek-accuracy tests for the Opus path in MBOggDecoder.
//
// Build & run with test/run_opus_seek_test.sh
//
// The test encodes a slow linear chirp, so a short window taken anywhere in
// the file identifies its own position by frequency alone. That makes the
// assertion "the seek landed where it said it would" rather than merely
// "the seek returned true".

#include "../src/mixeroutput/offline_encoder.h"
#include "../src/soloud/src/audiosource/wav/mb_ogg.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using SoLoud::DiskFile;
using SoLoud::MBOggDecoder;

namespace {

int gFailures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    printf("  FAIL: %s\n", what);
    gFailures++;
  }
}

constexpr int kRate = 48000;
constexpr double kDurationSeconds = 120.0;
constexpr double kStartHz = 200.0;
constexpr double kHzPerSecond = 60.0;

/// Instantaneous frequency of the chirp at time [t].
double freqAt(double t) { return kStartHz + kHzPerSecond * t; }

std::string tmpPath(const char *name) {
  const char *dir = std::getenv("TMPDIR");
  std::string base = (dir != nullptr) ? dir : "/tmp/";
  if (!base.empty() && base.back() != '/') {
    base.push_back('/');
  }
  return base + name;
}

bool writeChirp(const std::string &path) {
  const size_t n = static_cast<size_t>(kRate * kDurationSeconds);
  std::vector<float> pcm(n);
  double phase = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate;
    phase += 2.0 * M_PI * freqAt(t) / kRate;
    pcm[i] = 0.5f * static_cast<float>(std::sin(phase));
  }
  return soloud_offline::encodePcmToFile(
             pcm.data(), static_cast<int64_t>(pcm.size()), kRate, 1,
             MIXER_OUTPUT_OPUS, 0, path.c_str()) ==
         soloud_offline::OFFLINE_ENCODE_OK;
}

/// Dominant frequency of the next window, by zero-crossing rate. Crude, but
/// unambiguous for a single swept sine and immune to codec noise.
double measureFrequency(MBOggDecoder &dec, unsigned int atSample) {
  if (!dec.seek(atSample)) {
    return -1.0;
  }
  const unsigned int kWindow = 4096;
  std::vector<float> buf(kWindow * MAX_CHANNELS, 0.0f);
  const unsigned int got = dec.read(buf.data(), kWindow, kWindow);
  if (got < kWindow / 2) {
    return -1.0;
  }
  int crossings = 0;
  for (unsigned int i = 1; i < got; ++i) {
    if ((buf[i - 1] < 0.0f) != (buf[i] < 0.0f)) {
      crossings++;
    }
  }
  return static_cast<double>(crossings) * kRate / (2.0 * got);
}

double timeSeekMs(MBOggDecoder &dec, unsigned int atSample) {
  const auto start = std::chrono::steady_clock::now();
  dec.seek(atSample);
  std::vector<float> buf(4096 * MAX_CHANNELS);
  dec.read(buf.data(), 4096, 4096);
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

}  // namespace

int main() {
  const std::string path = tmpPath("opus_seek_test.opus");
  printf("encoding a %.0f s chirp\n", kDurationSeconds);
  if (!writeChirp(path)) {
    printf("  FAIL: could not encode the test file\n");
    return 1;
  }

  DiskFile file;
  if (file.open(path.c_str()) != 0) {
    printf("  FAIL: could not open the encoded file\n");
    return 1;
  }

  MBOggDecoder dec;
  if (!dec.open(&file)) {
    printf("  FAIL: decoder rejected the file\n");
    return 1;
  }

  check(dec.getCodecType() == MBOggDecoder::CODEC_OPUS, "codec is Opus");
  check(dec.getChannels() == 1, "channel count survives");
  check(dec.getSampleRate() == kRate, "sample rate survives");

  printf("seeks land at the right place\n");
  for (const double t : {0.0, 1.0, 10.0, 30.0, 60.0, 90.0, 119.0}) {
    const unsigned int sample = static_cast<unsigned int>(t * kRate);
    const double want = freqAt(t);
    const double got = measureFrequency(dec, sample);
    const double errorHz = (got < 0.0) ? 1e9 : std::abs(got - want);
    printf("  %6.1f s: expected ~%.1f Hz, measured %.1f Hz (%.1f Hz off)\n", t,
           want, got, errorHz);
    // The 4096-sample window spans 85 ms, over which the chirp itself sweeps
    // 5 Hz, so anything inside ~15 Hz is landing sample-accurately.
    check(errorHz < 15.0, "seek landed at the expected position");
  }

  printf("seeking backwards works as well as forwards\n");
  {
    const double late = measureFrequency(dec, static_cast<unsigned int>(100 * kRate));
    const double early = measureFrequency(dec, static_cast<unsigned int>(5 * kRate));
    printf("  100 s then 5 s: %.1f Hz then %.1f Hz\n", late, early);
    check(std::abs(late - freqAt(100.0)) < 15.0, "forward seek");
    check(std::abs(early - freqAt(5.0)) < 15.0, "backward seek");
  }

  printf("a deep seek does not cost more than a shallow one\n");
  {
    // Warm the file cache so this measures the search, not the first read.
    timeSeekMs(dec, static_cast<unsigned int>(2 * kRate));
    const double shallow = timeSeekMs(dec, static_cast<unsigned int>(2 * kRate));
    const double deep = timeSeekMs(dec, static_cast<unsigned int>(115 * kRate));
    printf("  2 s: %.2f ms | 115 s: %.2f ms (%.1fx)\n", shallow, deep,
           deep / shallow);
    // Before bisection this ratio was ~44x and grew with file length.
    check(deep < shallow * 8.0 + 5.0, "deep seek is not linear in position");
  }

  printf("seeking past the end fails rather than hanging\n");
  {
    const unsigned int past =
        static_cast<unsigned int>((kDurationSeconds + 30.0) * kRate);
    const bool ok = dec.seek(past);
    printf("  seek past end returned %s\n", ok ? "true" : "false");
    check(!ok, "refuses a target beyond the stream");
  }

  if (gFailures != 0) {
    printf("\n%d FAILURE(S)\n", gFailures);
    return 1;
  }
  printf("\nAll Opus seek tests passed.\n");
  return 0;
}
