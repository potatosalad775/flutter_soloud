#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "mixer_output_encoder.h"
#include "sinc_resampler.h"

/// Opus encoder that produces an Ogg Opus stream.
class OpusOutputEncoder : public MixerOutputEncoder {
 public:
  OpusOutputEncoder();
  ~OpusOutputEncoder() override;

  bool initialize(int sampleRate, int channels) override;
  void setBitrate(int bitsPerSecond) override { m_bitrate = bitsPerSecond; }
  void setComplexity(int complexity) override { m_complexity = complexity; }
  bool encode(const float *input, size_t samples,
              std::vector<uint8_t> &output) override;
  bool finalize(std::vector<uint8_t> &output) override;
  size_t inputBytesPerSample() const override { return sizeof(float); }

 private:
  bool m_initialized = false;
  int m_sampleRate = 48000;
  int m_channels = 2;
  size_t m_samplesPerFrame = 960;
  uint64_t m_packetCount = 0;
  int64_t m_packetGranulepos = 0;

  /// Target bitrate in bits/second, or 0 to let libopus decide from the rate
  /// and channel count. Set before [initialize].
  int m_bitrate = 0;

  /// Encoder complexity 0..10, or -1 to leave the libopus default.
  int m_complexity = -1;

  /// The encoder's algorithmic delay in 48 kHz samples, from
  /// OPUS_GET_LOOKAHEAD. It goes in the OpusHead pre-skip field so decoders
  /// discard exactly the samples the encoder invented at the head.
  int32_t m_preSkip = 0;

  /// Frames of real (post-resample) input seen so far. The final granulepos
  /// is pinned to this so decoders trim the zero padding [finalize] adds to
  /// fill the last 20 ms frame, instead of playing it.
  int64_t m_totalInputFrames = 0;

  std::vector<float> m_inputBuffer;
  std::vector<uint8_t> m_outputAccumulator;

  void *m_encoder = nullptr;

  struct OggState;
  std::unique_ptr<OggState> m_ogg;

  // Opus only supports 8/12/16/24/48 kHz. If the engine runs at a different
  // rate, we resample to 48 kHz through a windowed-sinc polyphase filter.
  int m_engineSampleRate = 48000;
  soloud_offline::SincResampler m_resampler;
  std::vector<float> m_resampledBuffer;

#ifndef NO_XIPH_LIBS
  void flushPages(bool force = false);
  bool resampleTo48k(const float *input, size_t samples);
#endif
};
