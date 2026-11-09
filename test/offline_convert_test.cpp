// Tests for offline file-to-file conversion: that it holds no copy of the audio,
// and what it leaves behind when it fails.
//
// Build & run with test/run_offline_convert_test.sh, or
// test\run_offline_tests_windows.bat
//
// The output of a conversion is checked against the same source decoded and encoded
// the long way round, so the assertions need no tolerance for what a codec did.

#include "../src/mixeroutput/offline_decoder.h"
#include "../src/mixeroutput/offline_encoder.h"

#include "offline_fixtures.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

using soloud_offline::convertFile;
using soloud_offline::decodeFileToPcm;
using soloud_offline::encodePcmToFile;
using soloud_offline::freeDecodedPcm;
using soloud_offline::OFFLINE_DECODE_FILE_ERROR;
using soloud_offline::OFFLINE_DECODE_INVALID_PARAMETER;
using soloud_offline::OFFLINE_DECODE_OK;
using soloud_offline::OFFLINE_DECODE_UNSUPPORTED_FORMAT;
using soloud_offline::OFFLINE_ENCODE_FILE_ERROR;
using soloud_offline::OFFLINE_ENCODE_OK;
using soloud_offline::OFFLINE_ENCODE_TOO_MANY_CHANNELS;
using soloud_offline::OFFLINE_ENCODE_UNSUPPORTED_FORMAT;
using soloud_offline::OfflineConvertResult;
using soloud_offline::OfflineDecodeResult;
using soloud_offline::OfflineFileWriter;

namespace {

int gFailures = 0;

void check(bool condition, const std::string &what) {
  if (!condition) {
    printf("  FAIL: %s\n", what.c_str());
    gFailures++;
  }
}

std::string tmpPath(const std::string &name) {
  const char *dir = std::getenv("TMPDIR");
  std::string base = (dir != nullptr) ? dir : "/tmp/";
  if (!base.empty() && base.back() != '/' && base.back() != '\\') {
    base.push_back('/');
  }
  return base + "offline_convert_" + name;
}

bool exists(const std::string &path) { return std::ifstream(path, std::ios::binary).good(); }

std::vector<unsigned char> readBytes(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void writeBytes(const std::string &path, const std::vector<unsigned char> &bytes) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char *>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
}

struct Decoded {
  OfflineDecodeResult result = OFFLINE_DECODE_OK;
  std::vector<float> samples;
  int64_t frames = 0;
  int channels = 0;
  int sampleRate = 0;
};

Decoded decode(const std::string &path, int64_t startMicros = 0, int64_t endMicros = -1) {
  Decoded out;
  float *samples = nullptr;
  out.result = decodeFileToPcm(path.c_str(), startMicros, endMicros, &samples, &out.frames,
                               &out.channels, &out.sampleRate);
  if (out.result == OFFLINE_DECODE_OK) {
    out.samples.assign(samples, samples + out.frames * out.channels);
    freeDecodedPcm(samples);
  }
  return out;
}

/// A different tone per channel, so a swap or a collapse changes the samples. Written
/// a block at a time, so that making a long source costs no memory of its own.
std::string writeLongWav(const char *name, int sampleRate, double seconds) {
  const std::string path = tmpPath(name);
  OfflineFileWriter writer(MIXER_OUTPUT_WAV, 0, path.c_str());
  check(writer.open(sampleRate, 2) == OFFLINE_ENCODE_OK, std::string("opening ") + name);
  std::vector<float> block(8192 * 2);
  const int64_t total = static_cast<int64_t>(sampleRate * seconds);
  for (int64_t frame = 0; frame < total; frame += 8192) {
    const int64_t count = std::min<int64_t>(8192, total - frame);
    for (int64_t i = 0; i < count; i++) {
      const double t = static_cast<double>(frame + i) / sampleRate;
      block[i * 2] = static_cast<float>(0.4 * std::sin(2 * M_PI * 440.0 * t));
      block[i * 2 + 1] = static_cast<float>(0.4 * std::sin(2 * M_PI * 880.0 * t));
    }
    check(writer.write(block.data(), count * 2) == OFFLINE_ENCODE_OK, "writing block");
  }
  check(writer.finish() == OFFLINE_ENCODE_OK, std::string("finishing ") + name);
  return path;
}

/// The highest the process's memory has ever been, in bytes.
size_t peakMemory() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
  return counters.PeakPagefileUsage;
#else
  struct rusage usage {};
  getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
  return static_cast<size_t>(usage.ru_maxrss);
#else
  return static_cast<size_t>(usage.ru_maxrss) * 1024;
#endif
#endif
}

/// Goes first: the peak only ever rises, so it must be read before anything in this
/// process has legitimately held a lot.
void testMemoryDoesNotFollowLength() {
  printf("memory\n");
  constexpr double kSeconds = 300.0;
  constexpr int kRate = 44100;
  const size_t onePcmCopy = static_cast<size_t>(kSeconds * kRate * 2 * sizeof(float));
  // Enough for the codecs' state and the read chunks, and a small part of one copy.
  const size_t allowed = onePcmCopy / 8;

  const std::string wav = writeLongWav("long.wav", kRate, kSeconds);
  const std::string flac = tmpPath("long.flac");
  const std::string opus = tmpPath("long.opus");
  const std::string vorbis = tmpPath("long.ogg");
  const std::string fromOgg = tmpPath("long_from_ogg.flac");

  const struct {
    const char *name;
    const std::string &source;
    const std::string &dest;
    MixerOutputFormat format;
  } steps[] = {
      {"WAV to FLAC", wav, flac, MIXER_OUTPUT_FLAC},
      {"FLAC to Opus", flac, opus, MIXER_OUTPUT_OPUS},
      {"WAV to Vorbis", wav, vorbis, MIXER_OUTPUT_VORBIS},
      {"Opus to FLAC", opus, fromOgg, MIXER_OUTPUT_FLAC},
  };
  for (const auto &step : steps) {
    const size_t before = peakMemory();
    const auto r = convertFile(step.source.c_str(), step.dest.c_str(), 0, -1, step.format, 192000);
    const size_t rise = peakMemory() - before;
    check(r.decode == OFFLINE_DECODE_OK && r.encode == OFFLINE_ENCODE_OK,
          std::string(step.name) + " converts");
    printf("  %s: peak rose %.1f MB, one PCM copy is %.0f MB\n", step.name, rise / 1048576.0,
           onePcmCopy / 1048576.0);
    check(rise < allowed, std::string(step.name) + " stays well under one copy of the audio");
  }

  const Decoded result = decode(flac);
  check(result.frames == static_cast<int64_t>(kSeconds * kRate), "the long FLAC has every frame");
  for (const auto &path : {wav, flac, opus, vorbis, fromOgg}) {
    std::remove(path.c_str());
  }
}

/// The result must match what decoding first and encoding after would have written.
void testMatchesDecodeThenEncode() {
  printf("same output as decode then encode\n");
  const std::string wav = writeLongWav("short.wav", 44100, 8.0);
  const Decoded source = decode(wav);

  // FLAC and WAV are deterministic and lossless here, so compare decoded samples
  // exactly. Encoders that embed a random stream serial cannot compare as bytes.
  for (const auto format : {MIXER_OUTPUT_FLAC, MIXER_OUTPUT_WAV}) {
    const char *ext = format == MIXER_OUTPUT_FLAC ? "a.flac" : "a.wav";
    const std::string direct = tmpPath(std::string("direct_") + ext);
    const std::string via = tmpPath(std::string("via_") + ext);
    const auto r = convertFile(wav.c_str(), direct.c_str(), 0, -1, format, 0);
    check(r.decode == OFFLINE_DECODE_OK && r.encode == OFFLINE_ENCODE_OK, std::string(ext) + " converts");
    check(encodePcmToFile(source.samples.data(), static_cast<int64_t>(source.samples.size()),
                          source.sampleRate, source.channels, format, 0,
                          via.c_str()) == OFFLINE_ENCODE_OK,
          std::string(ext) + " encodes");
    const Decoded a = decode(direct);
    const Decoded b = decode(via);
    check(a.frames == b.frames && a.frames > 0, std::string(ext) + " same length");
    check(a.samples == b.samples, std::string(ext) + " same samples");
    check(readBytes(direct) == readBytes(via), std::string(ext) + " same bytes");
    std::remove(direct.c_str());
    std::remove(via.c_str());
  }

  // Opus resamples, so what matters is that it still reports the right length.
  const std::string opus = tmpPath("a.opus");
  const auto r = convertFile(wav.c_str(), opus.c_str(), 0, -1, MIXER_OUTPUT_OPUS, 192000);
  check(r.decode == OFFLINE_DECODE_OK && r.encode == OFFLINE_ENCODE_OK, "Opus converts");
  const Decoded o = decode(opus);
  check(o.sampleRate == 48000, "Opus is 48 kHz");
  check(std::fabs((double)o.frames / o.sampleRate - 8.0) < 0.03,
        "Opus length " + std::to_string((double)o.frames / o.sampleRate));
  std::remove(opus.c_str());
  std::remove(wav.c_str());
}

/// A conversion only finds out how long the audio was at the end, unlike an encode
/// of a buffer, so this is the case that needs STREAMINFO patching. It must not rely
/// on the source declaring a length, which is why an MP3 is the source.
void testFlacDeclaresItsLength() {
  printf("FLAC length\n");
  const std::string mp3 = tmpPath("tone.mp3");
  writeBytes(mp3, std::vector<unsigned char>(std::begin(kToneMp3), std::end(kToneMp3)));
  const std::string flac = tmpPath("from_mp3.flac");
  const auto r = convertFile(mp3.c_str(), flac.c_str(), 0, -1, MIXER_OUTPUT_FLAC, 0);
  check(r.decode == OFFLINE_DECODE_OK && r.encode == OFFLINE_ENCODE_OK, "converts");

  const Decoded source = decode(mp3);
  check(decode(flac).frames == source.frames, "every frame arrived");

  // The field itself: 36 bits starting in the low nibble of the byte at offset 21,
  // with the bits around it left as libFLAC wrote them.
  const auto bytes = readBytes(flac);
  check(bytes.size() > 26 && std::memcmp(bytes.data(), "fLaC", 4) == 0, "is a FLAC file");
  if (bytes.size() > 26) {
    const uint64_t stated = (static_cast<uint64_t>(bytes[21] & 0x0F) << 32) |
                            (static_cast<uint64_t>(bytes[22]) << 24) |
                            (static_cast<uint64_t>(bytes[23]) << 16) |
                            (static_cast<uint64_t>(bytes[24]) << 8) | bytes[25];
    check(static_cast<int64_t>(stated) == source.frames,
          "STREAMINFO states the length, " + std::to_string(stated));
    // 16 bits per sample, stored as 15, is what sits above the count in that byte.
    check((bytes[21] >> 4) == 0x0F, "the bits per sample beside it survive");
  }

  std::remove(flac.c_str());
  std::remove(mp3.c_str());
}

void testRange() {
  printf("range\n");
  const std::string wav = writeLongWav("range.wav", 44100, 6.0);
  const Decoded whole = decode(wav);

  const std::string flac = tmpPath("range.flac");
  const auto r = convertFile(wav.c_str(), flac.c_str(), 1500000, 2500000, MIXER_OUTPUT_FLAC, 0);
  check(r.decode == OFFLINE_DECODE_OK && r.encode == OFFLINE_ENCODE_OK, "converts");
  const Decoded part = decode(flac);
  check(part.frames == 44100, "length " + std::to_string(part.frames));
  // Through 16-bit FLAC and back, each sample stays within one quantisation step.
  bool close = part.frames == 44100;
  const int64_t first = std::llround(1.5 * 44100);
  for (int64_t i = 0; close && i < part.frames * 2; i++) {
    close = std::fabs(part.samples[i] - whole.samples[first * 2 + i]) < 2.0 / 32768;
  }
  check(close, "the samples are those of the range");

  const std::string past = tmpPath("past.flac");
  const auto beyond = convertFile(wav.c_str(), past.c_str(), 60000000, -1, MIXER_OUTPUT_FLAC, 0);
  check(beyond.decode == OFFLINE_DECODE_UNSUPPORTED_FORMAT, "a range past the end holds no audio");
  check(!exists(past), "and leaves no file");
  std::remove(flac.c_str());
  std::remove(wav.c_str());
}

void testFailures() {
  printf("failures\n");
  const std::string wav = writeLongWav("source.wav", 22050, 1.0);
  const std::string dest = tmpPath("dest.flac");
  std::remove(dest.c_str());

  check(convertFile(tmpPath("does_not_exist.flac").c_str(), dest.c_str(), 0, -1, MIXER_OUTPUT_FLAC, 0).decode ==
            OFFLINE_DECODE_FILE_ERROR,
        "missing source");
  check(!exists(dest), "a missing source creates nothing");

  check(convertFile(nullptr, dest.c_str(), 0, -1, MIXER_OUTPUT_FLAC, 0).decode ==
            OFFLINE_DECODE_INVALID_PARAMETER,
        "null source");
  check(convertFile(wav.c_str(), nullptr, 0, -1, MIXER_OUTPUT_FLAC, 0).decode ==
            OFFLINE_DECODE_INVALID_PARAMETER,
        "null output");
  check(convertFile(wav.c_str(), dest.c_str(), 200, 100, MIXER_OUTPUT_FLAC, 0).decode ==
            OFFLINE_DECODE_INVALID_PARAMETER,
        "backwards range");

  std::vector<unsigned char> noise(64 * 1024);
  for (size_t i = 0; i < noise.size(); i++) {
    noise[i] = static_cast<unsigned char>((i * 2654435761u) >> 13);
  }
  noise[0] = 'J';
  const std::string junk = tmpPath("junk.bin");
  writeBytes(junk, noise);
  writeBytes(dest, {1, 2, 3});
  const auto unreadable = convertFile(junk.c_str(), dest.c_str(), 0, -1, MIXER_OUTPUT_FLAC, 0);
  check(unreadable.decode == OFFLINE_DECODE_UNSUPPORTED_FORMAT, "unreadable source");
  check(readBytes(dest) == std::vector<unsigned char>({1, 2, 3}),
        "an unreadable source leaves what was at the destination alone");
  std::remove(dest.c_str());
  std::remove(junk.c_str());

  // Writing over the source would truncate it mid-read: the only copy, in the case
  // of a library rewrite.
  const auto before = readBytes(wav);
  const auto self = convertFile(wav.c_str(), wav.c_str(), 0, -1, MIXER_OUTPUT_WAV, 0);
  check(self.decode == OFFLINE_DECODE_INVALID_PARAMETER, "converting a file over itself is refused");
  check(readBytes(wav) == before, "and leaves it intact");

  const std::string unsupported = tmpPath("unsupported.bin");
  const auto noEncoder = convertFile(wav.c_str(), unsupported.c_str(), 0, -1, MIXER_OUTPUT_PCM_F32LE, 0);
  check(noEncoder.encode == OFFLINE_ENCODE_UNSUPPORTED_FORMAT, "no encoder for the format");
  check(!exists(unsupported), "and no file");

  const std::string nowhere = tmpPath("no_such_directory/out.flac");
  check(convertFile(wav.c_str(), nowhere.c_str(), 0, -1, MIXER_OUTPUT_FLAC, 0).encode ==
            OFFLINE_ENCODE_FILE_ERROR,
        "output directory missing");
  std::remove(wav.c_str());
}

/// A WAV with [channels] channels of silence, written by hand because the encoders
/// only produce one or two.
void writeWavHeader(std::vector<unsigned char> &b, int channels, int rate, uint32_t dataBytes) {
  const auto put32 = [&](uint32_t v) {
    for (int i = 0; i < 4; i++) b.push_back(static_cast<unsigned char>(v >> (8 * i)));
  };
  const auto put16 = [&](uint16_t v) {
    b.push_back(static_cast<unsigned char>(v));
    b.push_back(static_cast<unsigned char>(v >> 8));
  };
  const auto tag = [&](const char *t) { b.insert(b.end(), t, t + 4); };
  tag("RIFF");
  put32(36 + dataBytes);
  tag("WAVE");
  tag("fmt ");
  put32(16);
  put16(1);
  put16(static_cast<uint16_t>(channels));
  put32(rate);
  put32(rate * channels * 2);
  put16(static_cast<uint16_t>(channels * 2));
  put16(16);
  tag("data");
  put32(dataBytes);
}

void testTooManyChannels() {
  printf("too many channels\n");
  std::vector<unsigned char> b;
  const uint32_t dataBytes = 44100 * 6 * 2;
  writeWavHeader(b, 6, 44100, dataBytes);
  b.resize(b.size() + dataBytes);
  const std::string wav = tmpPath("six.wav");
  writeBytes(wav, b);
  const std::string dest = tmpPath("six.flac");
  std::remove(dest.c_str());

  const auto r = convertFile(wav.c_str(), dest.c_str(), 0, -1, MIXER_OUTPUT_FLAC, 0);
  check(r.encode == OFFLINE_ENCODE_TOO_MANY_CHANNELS, "refused");
  check(r.decode == OFFLINE_DECODE_OK, "and not blamed on the decoder");
  check(r.channels == 6, "the count is reported");
  check(!exists(dest), "no file");
  std::remove(wav.c_str());
}

void testExistingOutputIsReplaced() {
  printf("existing output\n");
  const std::string wav = writeLongWav("replace.wav", 22050, 1.0);
  const std::string dest = tmpPath("replace.flac");
  writeBytes(dest, std::vector<unsigned char>(1 << 20, 0xAB));
  const auto r = convertFile(wav.c_str(), dest.c_str(), 0, -1, MIXER_OUTPUT_FLAC, 0);
  check(r.decode == OFFLINE_DECODE_OK && r.encode == OFFLINE_ENCODE_OK, "converts");
  check(decode(dest).frames == 22050, "the old contents are gone");
  std::remove(dest.c_str());
  std::remove(wav.c_str());
}

#if defined(_WIN32)
/// fopen would read the UTF-8 path as the system code page.
void testNonAsciiOutputPath() {
  printf("non-ASCII output path\n");
  const std::string wav = writeLongWav("ascii.wav", 22050, 0.5);
  const std::string dest = tmpPath("\xED\x95\x9C\xEA\xB8\x80_\xC3\xA9.flac");
  const auto r = convertFile(wav.c_str(), dest.c_str(), 0, -1, MIXER_OUTPUT_FLAC, 0);
  check(r.decode == OFFLINE_DECODE_OK && r.encode == OFFLINE_ENCODE_OK, "converts");
  check(decode(dest).frames == 11025, "decodes back through the same path");
  const int length = MultiByteToWideChar(CP_UTF8, 0, dest.c_str(), -1, nullptr, 0);
  std::wstring wide(length, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, dest.c_str(), -1, &wide[0], length);
  check(DeleteFileW(wide.c_str()) != 0, "the file has the name that was asked for");
  std::remove(wav.c_str());
}
#endif

/// ADTS is the path that holds nothing on Windows, and a whole file elsewhere. Either
/// way, the conversion and the decode must agree on what is in the file.
void testAdts() {
  printf("ADTS\n");
  const std::string aac = tmpPath("tone.aac");
  writeBytes(aac, std::vector<unsigned char>(std::begin(kToneAdts), std::end(kToneAdts)));
  const Decoded source = decode(aac);
  const std::string flac = tmpPath("from_aac.flac");
  const auto r = convertFile(aac.c_str(), flac.c_str(), 0, -1, MIXER_OUTPUT_FLAC, 0);
  check(r.decode == source.result, "same verdict as decoding");
  if (source.result == OFFLINE_DECODE_OK) {
    check(decode(flac).frames == source.frames, "same length as decoding");
  } else {
    check(!exists(flac), "no file if it cannot be read");
  }
  std::remove(flac.c_str());
  std::remove(aac.c_str());
}

}  // namespace

int main() {
  testMemoryDoesNotFollowLength();
  testMatchesDecodeThenEncode();
  testFlacDeclaresItsLength();
  testRange();
  testFailures();
  testTooManyChannels();
  testExistingOutputIsReplaced();
#if defined(_WIN32)
  testNonAsciiOutputPath();
#endif
  testAdts();
  if (gFailures == 0) {
    printf("ALL PASSED\n");
    return 0;
  }
  printf("%d FAILED\n", gFailures);
  return 1;
}
