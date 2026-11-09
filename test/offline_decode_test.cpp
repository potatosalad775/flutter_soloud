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
#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "../src/native_decoder/native_audio_decoder.h"
#include "offline_fixtures.h"

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
#if defined(__APPLE__)
  check(d.result == OFFLINE_DECODE_OK, "platform codec reads it");
#endif
  check(d.result == OFFLINE_DECODE_OK || d.result == OFFLINE_DECODE_UNSUPPORTED_FORMAT,
        "read properly or refused");
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

std::vector<unsigned char> id3Tag(size_t body, unsigned char fill = 0) {
  std::vector<unsigned char> b = {'I', 'D', '3', 4, 0, 0};
  b.push_back(static_cast<unsigned char>((body >> 21) & 0x7F));
  b.push_back(static_cast<unsigned char>((body >> 14) & 0x7F));
  b.push_back(static_cast<unsigned char>((body >> 7) & 0x7F));
  b.push_back(static_cast<unsigned char>(body & 0x7F));
  b.insert(b.end(), body, fill);
  return b;
}

std::vector<unsigned char> joined(std::initializer_list<std::vector<unsigned char>> parts) {
  std::vector<unsigned char> b;
  for (const auto &part : parts) {
    b.insert(b.end(), part.begin(), part.end());
  }
  return b;
}

std::vector<unsigned char> toneMp3() {
  return std::vector<unsigned char>(std::begin(kToneMp3), std::end(kToneMp3));
}

std::vector<unsigned char> toneAdts() {
  return std::vector<unsigned char>(std::begin(kToneAdts), std::end(kToneAdts));
}

/// Bytes with no zero, no MP3 frame and no signature in them.
std::vector<unsigned char> junkBytes(size_t size) {
  std::vector<unsigned char> b(size);
  for (size_t i = 0; i < size; i++) {
    b[i] = static_cast<unsigned char>(1 + (i * 31) % 200);
  }
  return b;
}

/// Writes [bytes] under [name] and decodes it.
Decoded decodeBytes(const char *name, const std::vector<unsigned char> &bytes) {
  const std::string path = tmpPath(name);
  writeFile(path, bytes);
  Decoded d = decode(path);
  std::remove(path.c_str());
  return d;
}

/// [d] is the fixture's audio, sample for sample: this is what tells a stream decoded
/// from its first frame from noise, or from a stream the decoder resynchronised into.
void checkSame(const Decoded &d, const Decoded &want, const std::string &label) {
  check(d.result == want.result, label + " result " + std::to_string((int)d.result));
  check(d.frames == want.frames, label + " frames " + std::to_string(d.frames) + " want " +
                                     std::to_string(want.frames));
  check(d.samples == want.samples, label + " samples equal the untagged decode");
}

/// A steady tone keeps one phase against absolute time, so a block of audio dropped,
/// repeated or out of order shows as a jump. Comparing a decode with another decode
/// by the same route cannot see that, and neither can its length or overall level.
bool phaseHolds(const Decoded &d, int channel, double frequency) {
  constexpr int64_t kBlock = 1024;
  const double w = 2 * M_PI * frequency / d.sampleRate;
  bool haveFirst = false;
  double first = 0;
  // The two ends are where the codec's priming and padding are.
  for (int64_t start = 4096; start + kBlock + 4096 < d.frames; start += kBlock) {
    double a = 0;
    double b = 0;
    for (int64_t i = start; i < start + kBlock; i++) {
      const double s = d.samples[i * d.channels + channel];
      a += s * std::cos(w * i);
      b += s * std::sin(w * i);
    }
    const double phase = std::atan2(b, a);
    if (!haveFirst) {
      first = phase;
      haveFirst = true;
    }
    if (std::fabs(std::remainder(phase - first, 2 * M_PI)) > 0.4) {
      return false;
    }
  }
  return haveFirst;
}

/// What actually proves the audio is right: the rate, the channels, the length, the
/// tone and its continuity, none of which is taken from another decode of the file.
void checkIsTone(const Decoded &d, int rate, double seconds, const std::string &label) {
  check(d.result == OFFLINE_DECODE_OK, label + " decodes");
  if (d.result != OFFLINE_DECODE_OK) {
    return;
  }
  check(d.sampleRate == rate, label + " rate " + std::to_string(d.sampleRate));
  check(d.channels == 1, label + " channels " + std::to_string(d.channels));
  const double length = static_cast<double>(d.frames) / d.sampleRate;
  check(std::fabs(length - seconds) < 0.15, label + " length " + std::to_string(length));
  check(level(d, 0, 440) > 0.1, label + " is the tone");
  check(phaseHolds(d, 0, 440), label + " stays in phase");
}

#if defined(_WIN32)
/// The transform and the source reader are the same decoder, so outside their known
/// faults they must agree to the sample. The stream is repeated to cross the decoder's
/// read chunks, since a file that fits in one chunk tests something else: the end of
/// the data arriving with the first of it.
void testAdtsAgainstSourceReader() {
  const std::vector<unsigned char> once(std::begin(kTone44kAdts), std::end(kTone44kAdts));
  std::vector<unsigned char> many;
  for (int i = 0; i < 30; i++) {
    many.insert(many.end(), once.begin(), once.end());
  }
  check(many.size() > 3 * 64 * 1024, "the long stream crosses read chunks");

  const std::string names[] = {"short.aac", "long.aac"};
  const std::vector<unsigned char> *streams[] = {&once, &many};
  for (int n = 0; n < 2; n++) {
    const std::string path = tmpPath(names[n]);
    writeFile(path, *streams[n]);
    const Decoded d = decode(path);
    DecodedAudioData reader = NativeAudioDecoder::decodeFile(path.c_str());
    check(reader.success, names[n] + " source reader reads it");
    check(d.result == OFFLINE_DECODE_OK, names[n] + " decodes");
    check(static_cast<size_t>(d.frames) == reader.sampleCount, names[n] + " same length");
    if (reader.success && static_cast<size_t>(d.frames) == reader.sampleCount) {
      check(std::equal(d.samples.begin(), d.samples.end(), reader.samples),
            names[n] + " same samples as the source reader");
    }
    reader.freeSamples();
    std::remove(path.c_str());
  }
}

/// Media Foundation only states the real format of low-rate AAC with the first
/// sample. Taking it from before then made mono 22.05 kHz read as 44.1 kHz stereo, a
/// quarter of its length, and with the tone gone. M4A goes through the source reader.
void testLowRateM4a() {
  printf("low-rate M4A\n");
  const std::vector<unsigned char> m4a(std::begin(kToneM4a), std::end(kToneM4a));
  const std::string path = tmpPath("low.m4a");
  writeFile(path, m4a);
  checkIsTone(decode(path), 22050, 2.0, "22.05 kHz M4A");
  std::remove(path.c_str());
}
#else
void testAdtsAgainstSourceReader() {}
void testLowRateM4a() {}
#endif

void testMp3BehindId3AndJunk() {
  printf("MP3 behind tags and junk\n");
  const Decoded want = decodeBytes("plain.mp3", toneMp3());
  check(want.result == OFFLINE_DECODE_OK, "the fixture decodes");
  checkIsTone(want, 22050, 2.0, "the fixture");

  for (const size_t body : {size_t(0), size_t(1024), size_t(4096), size_t(1 << 20)}) {
    checkSame(decodeBytes("tagged.mp3", joined({id3Tag(body), toneMp3()})), want,
              "ID3 tag " + std::to_string(body));
  }
  checkSame(decodeBytes("two.mp3", joined({id3Tag(100), id3Tag(5000), toneMp3()})), want,
            "two tags");

  // A picture is why tags get big, and JPEG is full of FF Ex. dr_mp3 skips a tag itself,
  // but a frame run in one must not be taken for the audio either.
  const auto mp3 = toneMp3();
  const std::vector<unsigned char> fakePicture(mp3.begin(), mp3.begin() + 3000);
  std::vector<unsigned char> tagWithFrames = id3Tag(fakePicture.size());
  std::copy(fakePicture.begin(), fakePicture.end(), tagWithFrames.begin() + 10);
  checkSame(decodeBytes("picture.mp3", joined({tagWithFrames, toneMp3()})), want,
            "frames inside the tag are not audio");

  checkSame(decodeBytes("zeros.mp3", joined({std::vector<unsigned char>(1000, 0), toneMp3()})), want,
            "zero padding");
  checkSame(decodeBytes("tagzeros.mp3",
                        joined({id3Tag(5000), std::vector<unsigned char>(1000, 0), toneMp3()})),
            want, "zero padding after a tag");
  checkSame(decodeBytes("junk.mp3", joined({junkBytes(1000), toneMp3()})), want, "junk first");
  checkSame(decodeBytes("tagjunk.mp3", joined({id3Tag(5000), junkBytes(1000), toneMp3()})), want,
            "junk after a tag");
  checkSame(decodeBytes("badtag.mp3",
                        joined({std::vector<unsigned char>{'I', 'D', '3', 4, 0, 0, 0xFF, 0xFF,
                                                           0xFF, 0xFF},
                                junkBytes(100), toneMp3()})),
            want, "letters that are not a valid tag are just junk");

  // A lone header inside the junk must not end the scan early, or decoding would start
  // there and the decoder would produce whatever it found.
  auto loneHeader = junkBytes(1000);
  std::copy(mp3.begin(), mp3.begin() + 4, loneHeader.begin() + 300);
  checkSame(decodeBytes("lone.mp3", joined({loneHeader, toneMp3()})), want,
            "a lone header in the junk");

  check(decodeBytes("far.mp3", joined({junkBytes(5000), toneMp3()})).result ==
            OFFLINE_DECODE_UNSUPPORTED_FORMAT,
        "junk past the window is refused");
}

void testNothingBehindTheJunk() {
  printf("refusals behind tags and junk\n");
  const auto noise = junkBytes(64 * 1024);
  check(decodeBytes("noise.bin", noise).result == OFFLINE_DECODE_UNSUPPORTED_FORMAT,
        "junk alone is refused");
  check(decodeBytes("tagnoise.mp3", joined({id3Tag(4096), noise})).result ==
            OFFLINE_DECODE_UNSUPPORTED_FORMAT,
        "a tag and junk is refused, rather than handed to dr_mp3 to resynchronise on");
  check(decodeBytes("tagonly.mp3", id3Tag(4096)).result == OFFLINE_DECODE_UNSUPPORTED_FORMAT,
        "a tag and nothing else");

  // The size says it runs off the end of the file.
  auto truncated = id3Tag(1 << 20);
  truncated.resize(2000);
  check(decodeBytes("truncated.mp3", truncated).result == OFFLINE_DECODE_UNSUPPORTED_FORMAT,
        "a tag longer than the file");

  // Isolated valid headers, as chance leaves them in PCM. One is three bytes of
  // agreement, and dr_mp3 would have accepted any of these.
  const auto mp3 = toneMp3();
  auto seeded = junkBytes(64 * 1024);
  for (size_t at = 100; at + 4 < seeded.size(); at += 997) {
    std::copy(mp3.begin(), mp3.begin() + 4, seeded.begin() + at);
  }
  check(decodeBytes("seeded.bin", seeded).result == OFFLINE_DECODE_UNSUPPORTED_FORMAT,
        "lone headers in junk are refused");

  const std::vector<unsigned char> header(mp3.begin(), mp3.begin() + 4);
  check(decodeBytes("twoheaders.bin", joined({junkBytes(500), header, header})).result ==
            OFFLINE_DECODE_UNSUPPORTED_FORMAT,
        "two headers that do not chain are refused");

  // PCM that happens to begin with a frame sync: 16-bit samples that sit near full
  // scale negative, whose repeating bytes are the likeliest accident.
  std::vector<unsigned char> pcm;
  for (int i = 0; i < 32 * 1024; i++) {
    pcm.push_back(0xFF);
    pcm.push_back(i % 2 == 0 ? 0xFB : 0xFA);
  }
  check(decodeBytes("pcm.bin", pcm).result == OFFLINE_DECODE_UNSUPPORTED_FORMAT,
        "PCM that resembles a sync is refused");
}

/// The bug the signature check fixed: CAF PCM whose samples look like MP3 frames. The
/// scan must not see them, since the signature decides first.
void testCafWithFrameLikePcm() {
  printf("CAF holding frames\n");
  const auto mp3 = toneMp3();
  std::vector<unsigned char> caf = {'c', 'a', 'f', 'f', 0, 1, 0, 0};
  caf.insert(caf.end(), mp3.begin(), mp3.end());
  const Decoded d = decodeBytes("frames.caf", caf);
  const Decoded plain = decodeBytes("plain.mp3", mp3);
  check(d.frames != plain.frames || d.result != OFFLINE_DECODE_OK,
        "a container's contents are never decoded as MP3");
}

void testAdtsBehindId3() {
  printf("ADTS behind tags\n");
  const Decoded want = decodeBytes("plain.aac", toneAdts());
#if defined(_WIN32) || defined(__APPLE__)
  check(want.result == OFFLINE_DECODE_OK, "the fixture decodes");
#endif
  if (want.result != OFFLINE_DECODE_OK) {
    // No AAC decoder on this machine. The one thing that may not happen is a pass
    // through dr_mp3 reporting success.
    check(decodeBytes("tagged.aac", joined({id3Tag(4096), toneAdts()})).result != OFFLINE_DECODE_OK,
          "ADTS with no decoder is refused");
    return;
  }
  checkIsTone(want, 22050, 2.0, "the fixture");
  testAdtsAgainstSourceReader();

  for (const size_t body :
       {size_t(0), size_t(1024), size_t(2031), size_t(2032), size_t(4096), size_t(65536), size_t(1 << 20)}) {
    checkSame(decodeBytes("tagged.aac", joined({id3Tag(body), toneAdts()})), want,
              "ID3 tag " + std::to_string(body));
  }
  // This is what must never reach the migration: it used to be Media Foundation's MP3
  // handler reporting success on one frame, whatever the extension hid.
  checkSame(decodeBytes("named.mp3", toneAdts()), want, "ADTS named .mp3");
  checkSame(decodeBytes("taggednamed.mp3", joined({id3Tag(4096), toneAdts()})), want,
            "tagged ADTS named .mp3");

  // Frames in a tag are not the audio behind it.
  const auto mp3 = toneMp3();
  std::vector<unsigned char> tag = id3Tag(3000);
  std::copy(mp3.begin(), mp3.begin() + 3000, tag.begin() + 10);
  checkSame(decodeBytes("mp3intag.aac", joined({tag, toneAdts()})), want,
            "MP3 frames in the tag of an AAC file");

  // The extra 128 bytes an ID3v1 tag leaves after the last frame.
  std::vector<unsigned char> trailer = {'T', 'A', 'G'};
  trailer.resize(128, 0x20);
  checkSame(decodeBytes("trailer.aac", joined({toneAdts(), trailer})), want, "an ID3v1 trailer");

  // A range, with the tag in front, must count from the audio and not the file.
  const std::string path = tmpPath("rangetag.aac");
  writeFile(path, joined({id3Tag(4096), toneAdts()}));
  const Decoded part = decode(path, 500000, 1000000);
  const int64_t first = std::llround(0.5 * want.sampleRate);
  const int64_t count = std::llround(1.0 * want.sampleRate) - first;
  check(part.frames == count, "range length " + std::to_string(part.frames));
  if (part.frames == count) {
    check(std::equal(part.samples.begin(), part.samples.end(),
                     want.samples.begin() + first * want.channels),
          "range samples equal the whole decode");
  }
  std::remove(path.c_str());
}

/// dr_flac and dr_wav seek by offsets they counted themselves. Behind a tag those need
/// shifting, or every range comes from the wrong place in the file.
void testSeekingBehindATag() {
  printf("seeking behind a tag\n");
  const auto pcm = makeSignal(44100, 2, kSeconds);
  const std::string flac = encode(pcm, 44100, 2, "behind.flac", MIXER_OUTPUT_FLAC);
  const std::string wav = encode(pcm, 44100, 2, "behind.wav", MIXER_OUTPUT_WAV);
  for (const auto &source : {flac, wav}) {
    std::ifstream in(source, std::ios::binary);
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(in),
                                           std::istreambuf_iterator<char>()};
    in.close();
    const std::string tagged = source + ".tagged";
    writeFile(tagged, joined({id3Tag(6000), bytes}));
    checkSame(decode(tagged), decode(source), source + " behind a tag");
    checkRanges(tagged, "behind a tag");
    std::remove(tagged.c_str());
    std::remove(source.c_str());
  }
}

}  // namespace

int main() {
  testMp3BehindId3AndJunk();
  testNothingBehindTheJunk();
  testCafWithFrameLikePcm();
  testAdtsBehindId3();
  testLowRateM4a();
  testSeekingBehindATag();
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
