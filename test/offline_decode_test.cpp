// Tests for the offline decoder: formats, frame-exact ranges and refusals.
//
// Build & run with test/run_offline_decode_test.sh
//
// Each source is encoded from a known buffer. A range must equal the same frames
// of the whole-file decode exactly, so the assertions need no tolerance for what
// a lossy codec did to the audio.

#include "../src/mixeroutput/offline_decoder.h"
#include "../src/mixeroutput/offline_encoder.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using soloud_offline::decodeFileToPcm;
using soloud_offline::encodePcmToFile;
using soloud_offline::freeDecodedPcm;
using soloud_offline::OFFLINE_DECODE_FILE_ERROR;
using soloud_offline::OFFLINE_DECODE_INVALID_PARAMETER;
using soloud_offline::OFFLINE_DECODE_OK;
using soloud_offline::OFFLINE_DECODE_UNSUPPORTED_FORMAT;
using soloud_offline::OFFLINE_ENCODE_OK;
using soloud_offline::OfflineDecodeResult;

namespace {

int gFailures = 0;

constexpr double kSeconds = 20.0;

void check(bool condition, const std::string &what) {
  if (!condition) {
    printf("  FAIL: %s\n", what.c_str());
    gFailures++;
  }
}

std::string tmpPath(const std::string &name) {
  const char *dir = std::getenv("TMPDIR");
  std::string base = (dir != nullptr) ? dir : "/tmp/";
  if (!base.empty() && base.back() != '/') {
    base.push_back('/');
  }
  return base + "offline_decode_" + name;
}

struct Decoded {
  OfflineDecodeResult result = OFFLINE_DECODE_OK;
  std::vector<float> samples;
  int64_t frames = 0;
  int channels = 0;
  int sampleRate = 0;
};

Decoded decode(const std::string &path, int64_t startMicros = 0,
               int64_t endMicros = -1) {
  Decoded out;
  float *samples = nullptr;
  out.result = decodeFileToPcm(path.c_str(), startMicros, endMicros, &samples,
                               &out.frames, &out.channels, &out.sampleRate);
  if (out.result == OFFLINE_DECODE_OK) {
    out.samples.assign(samples, samples + out.frames * out.channels);
    freeDecodedPcm(samples);
  }
  return out;
}

/// A different tone per channel, so a swap or a collapse changes the samples.
std::vector<float> makeSignal(int sampleRate, int channels, double seconds) {
  const size_t frames = static_cast<size_t>(sampleRate * seconds);
  std::vector<float> out(frames * channels);
  for (size_t i = 0; i < frames; i++) {
    for (int ch = 0; ch < channels; ch++) {
      out[i * channels + ch] = static_cast<float>(
          0.4 * std::sin(2 * M_PI * (440.0 * (ch + 1)) * i / sampleRate));
    }
  }
  return out;
}

/// Goertzel level of [frequency] in [channel].
double level(const Decoded &d, int channel, double frequency) {
  const double w = 2 * M_PI * frequency / d.sampleRate;
  double a = 0;
  double b = 0;
  for (int64_t i = 0; i < d.frames; i++) {
    const double s = d.samples[i * d.channels + channel];
    a += s * std::cos(w * i);
    b += s * std::sin(w * i);
  }
  return 2 * std::sqrt(a * a + b * b) / d.frames;
}

std::string encode(const std::vector<float> &pcm, int sampleRate, int channels,
                   const char *name, MixerOutputFormat format) {
  const std::string path = tmpPath(name);
  const auto result =
      encodePcmToFile(pcm.data(), static_cast<int64_t>(pcm.size()), sampleRate,
                      channels, format, 192000, path.c_str());
  check(result == OFFLINE_ENCODE_OK, std::string("encoding ") + name);
  return path;
}

/// Every range must be exactly those frames of the whole decode. Rates differ by
/// format, so the frame numbers are worked out from the decoded rate.
void checkRanges(const std::string &path, const char *name) {
  const Decoded whole = decode(path);
  check(whole.result == OFFLINE_DECODE_OK, std::string(name) + " whole decode");
  if (whole.result != OFFLINE_DECODE_OK) {
    return;
  }

  const int64_t ranges[][2] = {
      {0, 500000}, {250000, 750000}, {1000000, 1000100}, {15000000, 15500000},
      {19500000, -1}};
  for (const auto &range : ranges) {
    const std::string label = std::string(name) + " " + std::to_string(range[0]) +
                              ".." + std::to_string(range[1]);
    const Decoded part = decode(path, range[0], range[1]);
    check(part.result == OFFLINE_DECODE_OK, label + " decodes");
    if (part.result != OFFLINE_DECODE_OK) {
      continue;
    }
    const int64_t first = std::llround(range[0] * (double)whole.sampleRate / 1e6);
    const int64_t last = range[1] < 0
                             ? whole.frames
                             : std::llround(range[1] * (double)whole.sampleRate / 1e6);
    check(part.channels == whole.channels, label + " channels");
    check(part.sampleRate == whole.sampleRate, label + " rate");
    check(part.frames == last - first, label + " length " + std::to_string(part.frames) +
                                           " want " + std::to_string(last - first));
    if (part.frames != last - first) {
      continue;
    }
    bool same = true;
    for (size_t i = 0; i < part.samples.size(); i++) {
      if (part.samples[i] != whole.samples[first * whole.channels + i]) {
        same = false;
        break;
      }
    }
    check(same, label + " samples equal the whole decode");
  }
}

void testFormat(const char *name, const char *file, MixerOutputFormat format, int sampleRate,
                int wantRate) {
  printf("%s\n", name);
  // Long enough that an Ogg file takes several of the decoder's read chunks.
  const auto pcm = makeSignal(sampleRate, 2, kSeconds);
  const std::string path = encode(pcm, sampleRate, 2, file, format);

  const Decoded d = decode(path);
  check(d.result == OFFLINE_DECODE_OK, "decodes");
  if (d.result != OFFLINE_DECODE_OK) {
    return;
  }
  check(d.channels == 2, "channels");
  check(d.sampleRate == wantRate, "rate " + std::to_string(d.sampleRate));
  const double seconds = (double)d.frames / d.sampleRate;
  check(std::fabs(seconds - kSeconds) < 0.03, "length " + std::to_string(seconds));
  check(level(d, 0, 440) > 4 * level(d, 1, 440), "left is 440 Hz");
  check(level(d, 1, 880) > 4 * level(d, 0, 880), "right is 880 Hz");
  checkRanges(path, name);
  std::remove(path.c_str());
}

void writeFile(const std::string &path, const std::vector<unsigned char> &bytes) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char *>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
}

void put32(std::vector<unsigned char> &b, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    b.push_back(static_cast<unsigned char>(v >> shift));
  }
}

void put64(std::vector<unsigned char> &b, uint64_t v) {
  put32(b, static_cast<uint32_t>(v >> 32));
  put32(b, static_cast<uint32_t>(v));
}

void putTag(std::vector<unsigned char> &b, const char *tag) {
  b.insert(b.end(), tag, tag + std::strlen(tag));
}

/// A CAF of 16-bit big-endian PCM: the file that dr_mp3 used to "decode" into a
/// fraction of a second of noise. 0.5 s at 22.05 kHz, stereo.
std::string writeCafPcm() {
  std::vector<unsigned char> b;
  putTag(b, "caff");
  b.insert(b.end(), {0, 1, 0, 0});
  putTag(b, "desc");
  put64(b, 32);
  union {
    double d;
    uint64_t u;
  } rate;
  rate.d = 22050.0;
  put64(b, rate.u);
  putTag(b, "lpcm");
  put32(b, 0);   // big-endian integer
  put32(b, 4);   // bytes per packet
  put32(b, 1);   // frames per packet
  put32(b, 2);   // channels
  put32(b, 16);  // bits
  const size_t frames = 11025;
  putTag(b, "data");
  put64(b, 4 + frames * 4);
  put32(b, 0);  // edit count
  for (size_t i = 0; i < frames; i++) {
    for (int ch = 0; ch < 2; ch++) {
      const auto s = static_cast<int16_t>(
          12000 * std::sin(2 * M_PI * (440.0 * (ch + 1)) * i / 22050.0));
      b.push_back(static_cast<unsigned char>(static_cast<uint16_t>(s) >> 8));
      b.push_back(static_cast<unsigned char>(s & 0xFF));
    }
  }
  const std::string path = tmpPath("pcm.caf");
  writeFile(path, b);
  return path;
}

void testRefusals() {
  printf("refusals\n");
  // PCM-looking bytes that dr_mp3 would resynchronise on, with no signature.
  std::vector<unsigned char> noise(64 * 1024);
  for (size_t i = 0; i < noise.size(); i++) {
    noise[i] = static_cast<unsigned char>((i * 2654435761u) >> 13);
  }
  noise[0] = 'J';
  noise[1] = 'U';
  noise[2] = 'N';
  noise[3] = 'K';
  const std::string junk = tmpPath("junk.bin");
  writeFile(junk, noise);
  check(decode(junk).result == OFFLINE_DECODE_UNSUPPORTED_FORMAT, "unknown file refused");
  std::remove(junk.c_str());

  check(decode(tmpPath("does_not_exist.flac")).result == OFFLINE_DECODE_FILE_ERROR,
        "missing file");

  const std::string empty = tmpPath("empty.wav");
  writeFile(empty, {});
  check(decode(empty).result == OFFLINE_DECODE_FILE_ERROR, "empty file");
  std::remove(empty.c_str());

  const auto pcm = makeSignal(22050, 2, 0.5);
  const std::string path = encode(pcm, 22050, 2, "range.flac", MIXER_OUTPUT_FLAC);
  check(decode(path, -1, 100).result == OFFLINE_DECODE_INVALID_PARAMETER, "negative start");
  check(decode(path, 200, 100).result == OFFLINE_DECODE_INVALID_PARAMETER, "end before start");
  check(decode(path, 100, 100).result == OFFLINE_DECODE_INVALID_PARAMETER, "empty range");
  check(decode(path, 60000000, -1).result == OFFLINE_DECODE_UNSUPPORTED_FORMAT,
        "range past the end holds no audio");
  check(decode(path, 400000, 60000000).frames == std::llround(0.1 * 22050),
        "a range running over stops at the end of the file");
  std::remove(path.c_str());
}

void testCafPcm() {
  printf("CAF PCM\n");
  const std::string path = writeCafPcm();
  const Decoded d = decode(path);
  // Apple's codecs read it. Elsewhere it must be refused: junk is the failure.
  check(d.result == OFFLINE_DECODE_OK, "platform codec reads it");
  if (d.result == OFFLINE_DECODE_OK) {
    check(d.frames == 11025, "length " + std::to_string(d.frames));
    check(d.channels == 2, "channels");
    check(level(d, 0, 440) > 4 * level(d, 1, 440), "left is 440 Hz");
    check(level(d, 1, 880) > 4 * level(d, 0, 880), "right is 880 Hz");
    const Decoded part = decode(path, 100000, 200000);
    check(part.frames == std::llround(0.1 * 22050), "range of a platform decode");
  }
  std::remove(path.c_str());
}

}  // namespace

int main() {
  testFormat("WAV", "a.wav", MIXER_OUTPUT_WAV, 44100, 44100);
  testFormat("FLAC", "a.flac", MIXER_OUTPUT_FLAC, 44100, 44100);
  testFormat("Ogg Opus", "a.opus", MIXER_OUTPUT_OPUS, 44100, 48000);
  testFormat("Ogg Vorbis", "a.ogg", MIXER_OUTPUT_VORBIS, 44100, 44100);
  testRefusals();
  testCafPcm();
  if (gFailures == 0) {
    printf("ALL PASSED\n");
    return 0;
  }
  printf("%d FAILED\n", gFailures);
  return 1;
}
