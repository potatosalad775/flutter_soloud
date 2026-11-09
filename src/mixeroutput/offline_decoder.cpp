#include "offline_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../audiobuffer/aac_stream_decoder.h"
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

/// Encoded bytes fed to the chunked decoders at a time.
constexpr unsigned int kChunkBytes = 64 * 1024;

/// How far into a file MP3 is looked for, which is enough to see past the zero
/// padding some files start with and the junk others do.
constexpr size_t kHeaderBytes = 4096;

/// Consecutive frames that count as a stream. dr_mp3 resynchronises on anything,
/// and a lone header is three loosely constrained bytes that turn up by chance.
constexpr int kFrameRun = 4;

/// What is read to classify a file: a run of the largest possible ADTS frames,
/// starting at the last position looked at.
constexpr size_t kProbeBytes = kHeaderBytes + kFrameRun * 8192;

/// Decoded audio arrives here a block at a time. This keeps the part of it inside
/// the requested range and passes that on, so nothing outside it is ever held.
class PcmSink {
 public:
  PcmSink(int64_t startMicros, int64_t endMicros)
      : mStartMicros(startMicros), mEndMicros(endMicros) {}

  virtual ~PcmSink() = default;

  PcmSink(const PcmSink &) = delete;
  PcmSink &operator=(const PcmSink &) = delete;

  /// False if the format is one this sink cannot take.
  bool setFormat(int channels, int sampleRate) {
    mChannels = channels;
    mSampleRate = sampleRate;
    mStartFrame = toFrame(mStartMicros);
    mEndFrame = mEndMicros < 0 ? -1 : toFrame(mEndMicros);
    return acceptsFormat();
  }

  int64_t startFrame() const { return mStartFrame; }

  /// False once the end of the range has been reached.
  bool wantsMore() const { return mEndFrame < 0 || mPosition < mEndFrame; }

  /// Avoids growing a buffer when the length is known up front.
  bool reserve(int64_t totalFrames) {
    const int64_t end = mEndFrame < 0 ? totalFrames : std::min(totalFrames, mEndFrame);
    return end <= mStartFrame || prepareFor(end - mStartFrame);
  }

  /// Records that the decoder itself moved to [frame].
  void seekedTo(int64_t frame) { mPosition = frame; }

  /// Takes the next [frames] frames of the file. False if this sink failed.
  bool append(const float *interleaved, int64_t frames) {
    const int64_t first = std::max<int64_t>(mStartFrame, mPosition);
    const int64_t pastEnd = mPosition + frames;
    const int64_t last = mEndFrame < 0 ? pastEnd : std::min(pastEnd, mEndFrame);
    if (last > first && !consume(interleaved + (first - mPosition) * mChannels, last - first)) {
      return false;
    }
    mPosition = pastEnd;
    return true;
  }

 protected:
  virtual bool acceptsFormat() { return true; }
  virtual bool prepareFor(int64_t /*frames*/) { return true; }
  virtual bool consume(const float *interleaved, int64_t frames) = 0;

  int mChannels = 0;
  int mSampleRate = 0;

 private:
  int64_t toFrame(int64_t micros) const {
    return std::llround(static_cast<double>(micros) * mSampleRate / 1000000.0);
  }

  const int64_t mStartMicros;
  const int64_t mEndMicros;
  int64_t mStartFrame = 0;
  int64_t mEndFrame = -1;
  int64_t mPosition = 0;
};

/// Collects the audio into one interleaved buffer, which the caller takes over.
class BufferSink : public PcmSink {
 public:
  using PcmSink::PcmSink;

  ~BufferSink() override { std::free(mBuffer); }

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

 protected:
  bool prepareFor(int64_t frames) override { return ensureCapacity(frames); }

  bool consume(const float *interleaved, int64_t frames) override {
    if (!ensureCapacity(mFrames + frames)) {
      return false;
    }
    std::memcpy(mBuffer + mFrames * mChannels, interleaved,
                static_cast<size_t>(frames * mChannels) * sizeof(float));
    mFrames += frames;
    return true;
  }

 private:
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

  float *mBuffer = nullptr;
  int64_t mFrames = 0;
  int64_t mCapacity = 0;
};

/// Hands the audio straight to an encoder. The output file only comes into being
/// with the first frame, so a source with no audio in it never touches the disk.
class EncoderSink : public PcmSink {
 public:
  EncoderSink(int64_t startMicros, int64_t endMicros, MixerOutputFormat format,
              int bitrate, const char *outputPath)
      : PcmSink(startMicros, endMicros), mWriter(format, bitrate, outputPath) {}

  int channels() const { return mChannels; }
  bool hasAudio() const { return mOpened; }
  OfflineEncodeResult result() const { return mResult; }

  OfflineEncodeResult finish() {
    mResult = mWriter.finish();
    return mResult;
  }

 protected:
  bool acceptsFormat() override {
    if (mChannels > 2) {
      mResult = OFFLINE_ENCODE_TOO_MANY_CHANNELS;
      return false;
    }
    return true;
  }

  bool consume(const float *interleaved, int64_t frames) override {
    if (!mOpened) {
      mResult = mWriter.open(mSampleRate, mChannels);
      if (mResult != OFFLINE_ENCODE_OK) {
        return false;
      }
      mOpened = true;
    }
    mResult = mWriter.write(interleaved, frames * mChannels);
    return mResult == OFFLINE_ENCODE_OK;
  }

 private:
  OfflineFileWriter mWriter;
  OfflineEncodeResult mResult = OFFLINE_ENCODE_OK;
  bool mOpened = false;
};

/// The file a decoder reads from. [base] is where the audio starts, past any ID3
/// tags, and is what a decoder sees as the start of the file: dr_flac and dr_wav
/// seek by offsets they counted themselves from where they began reading.
struct Source {
  SoLoud::File *file;
  int base;
};

size_t readFromFile(void *source, void *out, size_t bytes) {
  return static_cast<Source *>(source)->file->read(
      static_cast<unsigned char *>(out), static_cast<unsigned int>(bytes));
}

/// Moves to [offset], from the start of the audio when [fromStart].
void seekInFile(void *source, int offset, bool fromStart) {
  auto *s = static_cast<Source *>(source);
  s->file->seek(fromStart ? s->base + offset : offset + static_cast<int>(s->file->pos()));
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

enum class FileKind { Wav, Flac, Mp3, Ogg, Adts, Native, Unknown };

/// What a file holds, and where in it the audio starts.
struct Classified {
  FileKind kind;
  int offset;
};

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

/// The length of the MPEG audio frame at [h], which [isMp3Frame] has accepted, or
/// 0 for a free-format frame, which does not state one.
int mp3FrameBytes(const unsigned char *h) {
  static const short kBitrates[2][3][16] = {
      // MPEG 1, layers I, II and III.
      {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
       {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
       {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}},
      // MPEG 2 and 2.5.
      {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
       {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
       {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}}};
  static const int kRates[3] = {44100, 48000, 32000};

  const int version = (h[1] >> 3) & 0x03;  // 3 is MPEG 1, 2 is MPEG 2, 0 is MPEG 2.5.
  const int layer = 3 - ((h[1] >> 1) & 0x03);  // 0 is layer I.
  const bool mpeg1 = version == 3;
  const int rate = kRates[(h[2] >> 2) & 0x03] >> (mpeg1 ? 0 : version == 2 ? 1 : 2);
  const int bitrate = kBitrates[mpeg1 ? 0 : 1][layer][(h[2] >> 4) & 0x0F] * 1000;
  const int padding = (h[2] >> 1) & 0x01;
  if (bitrate == 0) {
    return 0;
  }
  if (layer == 0) {
    return (12 * bitrate / rate + padding) * 4;
  }
  return ((layer == 2 && !mpeg1) ? 72 : 144) * bitrate / rate + padding;
}

/// How many consecutive frames were walked, up to [kFrameRun], and whether the
/// walk ran out of file rather than finding something that was not a frame.
struct FrameRun {
  int frames = 0;
  bool reachedEnd = false;
};

/// Frames of one stream agree on version, layer and sample rate.
FrameRun mp3FrameRun(const unsigned char *h, size_t size) {
  FrameRun run;
  size_t offset = 0;
  while (run.frames < kFrameRun) {
    if (offset + 3 > size) {
      run.reachedEnd = true;
      break;
    }
    const unsigned char *frame = h + offset;
    if (!isMp3Frame(frame, size - offset) ||
        (offset > 0 && ((frame[1] & 0xFE) != (h[1] & 0xFE) ||
                        (frame[2] & 0x0C) != (h[2] & 0x0C)))) {
      break;
    }
    const int bytes = mp3FrameBytes(frame);
    if (bytes == 0) {
      break;
    }
    run.frames++;
    offset += static_cast<size_t>(bytes);
  }
  return run;
}

/// Frames of one ADTS stream agree on sample rate and channel layout.
FrameRun adtsFrameRun(const unsigned char *h, size_t size) {
  FrameRun run;
  size_t offset = 0;
  while (run.frames < kFrameRun) {
    if (offset + 7 > size) {
      run.reachedEnd = true;
      break;
    }
    const unsigned char *frame = h + offset;
    if (!NativeAudioDecoder::isAacAdts(frame, size - offset) ||
        (offset > 0 && ((frame[2] & 0x3D) != (h[2] & 0x3D) ||
                        (frame[3] & 0xC0) != (h[3] & 0xC0)))) {
      break;
    }
    run.frames++;
    offset += (static_cast<size_t>(frame[3] & 0x03) << 11) |
              (static_cast<size_t>(frame[4]) << 3) | (frame[5] >> 5);
  }
  return run;
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
///
/// Every signature is tried first, so a container such as CAF can never be scanned
/// for frames that its PCM happens to resemble.
Classified classify(const unsigned char *h, size_t size) {
  if (startsWith(h, size, "OggS")) {
    return {FileKind::Ogg, 0};
  }
  if (size >= 12 && startsWith(h, size, "RIFF") && std::memcmp(h + 8, "WAVE", 4) == 0) {
    return {FileKind::Wav, 0};
  }
  if (startsWith(h, size, "fLaC")) {
    return {FileKind::Flac, 0};
  }
  if (NativeAudioDecoder::isM4aOrMp4(h, size) || NativeAudioDecoder::isAc3OrEac3(h, size) ||
      isOtherPlatformContainer(h, size)) {
    return {FileKind::Native, 0};
  }

  size_t start = 0;
  while (start < size && start < kHeaderBytes && h[start] == 0) {
    start++;
  }
  if (start == kHeaderBytes) {
    return {FileKind::Unknown, 0};
  }

  const FrameRun adts = adtsFrameRun(h + start, size - start);
  if (adts.frames > 0 && (adts.frames == kFrameRun || adts.reachedEnd)) {
    return {FileKind::Adts, static_cast<int>(start)};
  }

  // At the very start, one frame in a file too short for more still counts, and a
  // free-format frame states no length to walk.
  if (isMp3Frame(h + start, size - start)) {
    const FrameRun run = mp3FrameRun(h + start, size - start);
    if (run.frames == kFrameRun || run.reachedEnd || mp3FrameBytes(h + start) == 0) {
      return {FileKind::Mp3, static_cast<int>(start)};
    }
  }

  // Junk before the first frame. Starting the decoder at the frame found also
  // means it cannot resynchronise on something in the junk.
  const size_t scanEnd = std::min(size, kHeaderBytes);
  for (size_t i = start + 1; i < scanEnd; i++) {
    if (!isMp3Frame(h + i, size - i)) {
      continue;
    }
    const FrameRun run = mp3FrameRun(h + i, size - i);
    if (run.frames == kFrameRun || (run.reachedEnd && run.frames >= 2)) {
      return {FileKind::Mp3, static_cast<int>(i)};
    }
  }
  return {FileKind::Unknown, 0};
}

/// The bytes the ID3v2 tag at [h] takes up, or 0 if there is no valid one. A
/// syncsafe size has the top bit of each byte clear, which is what tells a tag from
/// a file that merely starts with these letters.
size_t id3TagBytes(const unsigned char *h, size_t size) {
  if (size < 10 || !startsWith(h, size, "ID3") || h[3] == 0xFF || h[4] == 0xFF ||
      ((h[6] | h[7] | h[8] | h[9]) & 0x80) != 0) {
    return 0;
  }
  const size_t body = (static_cast<size_t>(h[6]) << 21) | (static_cast<size_t>(h[7]) << 14) |
                      (static_cast<size_t>(h[8]) << 7) | h[9];
  const bool footer = (h[5] & 0x10) != 0;
  return 10 + body + (footer ? 10 : 0);
}

/// Classifies what follows any ID3 tags. A tag says nothing about the audio behind
/// it, which is AAC as often as MP3, and an embedded picture can make it megabytes
/// long, so the file has to be read at the tag's end rather than within one window.
Classified classifyFile(SoLoud::File &file) {
  std::vector<unsigned char> window(kProbeBytes);
  const size_t length = file.length();
  size_t base = 0;
  size_t got = 0;
  // Files with several tags in a row exist. A bound keeps a crafted one cheap.
  for (int tags = 0; tags < 16; tags++) {
    file.seek(static_cast<int>(base));
    got = file.read(window.data(), static_cast<unsigned int>(window.size()));
    const size_t tag = id3TagBytes(window.data(), got);
    if (tag == 0) {
      break;
    }
    base += tag;
    if (base >= length) {
      return {FileKind::Unknown, 0};
    }
  }
  Classified found = classify(window.data(), got);
  found.offset += static_cast<int>(base);
  return found;
}

/// `opened == false` means the file was not what its signature promised, so a
/// platform codec gets to try. Otherwise `ok` is the decode's real outcome.
struct StreamOutcome {
  bool opened;
  bool ok;
};

StreamOutcome decodeWav(Source &source, PcmSink &sink) {
  drwav wav;
  if (!drwav_init(&wav, readFromFile, seekWav, nullptr, &source, nullptr)) {
    return {false, false};
  }
  bool ok = sink.setFormat(wav.channels, static_cast<int>(wav.sampleRate)) &&
            sink.reserve(static_cast<int64_t>(wav.totalPCMFrameCount));
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

StreamOutcome decodeFlac(Source &source, PcmSink &sink) {
  drflac *flac = drflac_open(readFromFile, seekFlac, nullptr, &source, nullptr);
  if (flac == nullptr) {
    return {false, false};
  }
  bool ok = sink.setFormat(flac->channels, static_cast<int>(flac->sampleRate)) &&
            sink.reserve(static_cast<int64_t>(flac->totalPCMFrameCount));
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
StreamOutcome decodeMp3(Source &source, PcmSink &sink) {
  drmp3 mp3;
  if (!drmp3_init(&mp3, readFromFile, seekMp3, nullptr, nullptr, &source, nullptr)) {
    return {false, false};
  }
  bool ok = sink.setFormat(static_cast<int>(mp3.channels), static_cast<int>(mp3.sampleRate));
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

/// Feeds a stream decoder a chunk at a time, so a range stops reading at its end
/// and no more than a chunk's worth of audio is ever in hand. [decode] takes
/// (chunk, &rate, &channels) and returns (pcm, error).
template <typename Decoder, typename Decode>
OfflineDecodeResult decodeChunks(SoLoud::File &file, PcmSink &sink, Decoder &decoder,
                                 int rate, int channelCount, Decode decode) {
  std::vector<unsigned char> chunk;
  bool formatKnown = false;
  bool endOfFile = false;
  bool endSignalled = false;
  int emptyPasses = 0;

  while (sink.wantsMore() || !formatKnown) {
    if (!endOfFile) {
      chunk.resize(kChunkBytes);
      const unsigned int got = file.read(chunk.data(), kChunkBytes);
      chunk.resize(got);
      endOfFile = got < kChunkBytes;
    } else {
      chunk.clear();
    }

    auto [pcm, error] = decode(chunk, &rate, &channelCount);
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
        if (!sink.setFormat(channelCount, rate)) {
          return OFFLINE_DECODE_FAILED;
        }
        formatKnown = true;
      }
      if (!sink.append(pcm.data(), static_cast<int64_t>(pcm.size() / channelCount))) {
        return OFFLINE_DECODE_FAILED;
      }
      emptyPasses = 0;
    }
    if (endOfFile && !endSignalled) {
      // Only once the last chunk is in. The AAC transform starts draining when told
      // the data has ended, and whatever arrives after that comes out spliced, with
      // the right length and the wrong audio.
      decoder.setDataEnded();
      endSignalled = true;
    } else if (endSignalled &&
               (!decoder.hasPendingData() || (pcm.empty() && ++emptyPasses >= 64))) {
      break;
    }
  }
  return OFFLINE_DECODE_OK;
}

/// Ogg Opus, Vorbis and FLAC, through the decoders the engine's own Ogg path uses.
OfflineDecodeResult decodeOgg(SoLoud::File &file, PcmSink &sink) {
  auto ignoreMetadata = [](AudioMetadata) {};
  StreamDecoder decoder;
  return decodeChunks(file, sink, decoder, 48000, 2,
                      [&](std::vector<unsigned char> &chunk, int *rate, int *channels) {
                        return decoder.decode(chunk, rate, channels, ignoreMetadata);
                      });
}

#if defined(_WIN32) || defined(_WIN64)
/// ADTS through the AAC transform itself rather than a source reader, which the
/// file-level path below cannot be trusted with. Media Foundation only sniffs the
/// first 2048 bytes for an ADTS header, so it refuses a stream behind an ID3 tag
/// longer than that. Worse, it believes a `.mp3` extension over the contents, and
/// reports success on 1152 frames of an ADTS file named that way. The samples are
/// identical to the source reader's, and this also holds none of them.
OfflineDecodeResult decodeAdts(SoLoud::File &file, PcmSink &sink) {
  AACDecoderWrapper decoder(DetectedType::BUFFER_AAC);
  if (!decoder.initializeDecoder(0, 0)) {
    return OFFLINE_DECODE_UNSUPPORTED_FORMAT;
  }
  return decodeChunks(file, sink, decoder, 0, 0,
                      [&](std::vector<unsigned char> &chunk, int *rate, int *channels) {
                        return decoder.decode(chunk, rate, channels);
                      });
}
#endif

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

  OfflineDecodeResult result = OFFLINE_DECODE_OK;
  if (!sink.setFormat(static_cast<int>(channels), static_cast<int>(decoded.sampleRate)) ||
      !sink.reserve(static_cast<int64_t>(total))) {
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

/// Runs the decoder for [found] into [sink].
OfflineDecodeResult decodeInto(const char *path, SoLoud::File &file, const Classified &found,
                               PcmSink &sink) {
  file.seek(found.offset);
  Source source{&file, found.offset};
  switch (found.kind) {
    case FileKind::Ogg:
      return decodeOgg(file, sink);
    case FileKind::Adts:
#if defined(_WIN32) || defined(_WIN64)
      return decodeAdts(file, sink);
#else
      return decodeNative(path, sink);
#endif
    case FileKind::Native:
      return decodeNative(path, sink);
    default:
      break;
  }
  const StreamOutcome outcome = found.kind == FileKind::Wav    ? decodeWav(source, sink)
                                : found.kind == FileKind::Flac ? decodeFlac(source, sink)
                                                               : decodeMp3(source, sink);
  if (!outcome.opened) {
    return decodeNative(path, sink);
  }
  return outcome.ok ? OFFLINE_DECODE_OK : OFFLINE_DECODE_FAILED;
}

/// Opens and classifies [path]. DiskFile rather than ifstream: it opens UTF-8 paths
/// on Windows.
OfflineDecodeResult openSource(const char *path, SoLoud::DiskFile &file, Classified &found) {
  if (file.open(path) != SoLoud::SO_NO_ERROR || file.length() == 0) {
    return OFFLINE_DECODE_FILE_ERROR;
  }
  found = classifyFile(file);
  return found.kind == FileKind::Unknown ? OFFLINE_DECODE_UNSUPPORTED_FORMAT : OFFLINE_DECODE_OK;
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

  SoLoud::DiskFile file;
  Classified found{FileKind::Unknown, 0};
  const OfflineDecodeResult opened = openSource(path, file, found);
  if (opened != OFFLINE_DECODE_OK) {
    return opened;
  }

  BufferSink sink(startMicros, endMicros);
  const OfflineDecodeResult result = decodeInto(path, file, found, sink);
  if (result != OFFLINE_DECODE_OK) {
    return result;
  }
  return sink.finish(samples, frames, channels, sampleRate);
}

void freeDecodedPcm(void *samples) { std::free(samples); }

OfflineConvertResult convertFile(const char *sourcePath, const char *outputPath,
                                 int64_t startMicros, int64_t endMicros,
                                 MixerOutputFormat format, int bitrate) {
  OfflineConvertResult result;
  if (sourcePath == nullptr || outputPath == nullptr || startMicros < 0 ||
      (endMicros >= 0 && endMicros <= startMicros)) {
    result.decode = OFFLINE_DECODE_INVALID_PARAMETER;
    return result;
  }

  SoLoud::DiskFile file;
  Classified found{FileKind::Unknown, 0};
  result.decode = openSource(sourcePath, file, found);
  if (result.decode != OFFLINE_DECODE_OK) {
    return result;
  }
  // Decoding first used to make this harmless. Streaming would truncate the source
  // while still reading it.
  if (isSameFile(sourcePath, outputPath)) {
    result.decode = OFFLINE_DECODE_INVALID_PARAMETER;
    return result;
  }

  EncoderSink sink(startMicros, endMicros, format, bitrate, outputPath);
  result.decode = decodeInto(sourcePath, file, found, sink);
  result.channels = sink.channels();
  result.encode = sink.result();
  if (result.encode != OFFLINE_ENCODE_OK) {
    // The sink stopped the decoder, so its failure is the real one.
    result.decode = OFFLINE_DECODE_OK;
    return result;
  }
  if (result.decode != OFFLINE_DECODE_OK) {
    return result;
  }
  if (!sink.hasAudio()) {
    result.decode = OFFLINE_DECODE_UNSUPPORTED_FORMAT;
    return result;
  }
  result.encode = sink.finish();
  return result;
}

}  // namespace soloud_offline
