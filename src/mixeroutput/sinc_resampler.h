#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace soloud_offline {

/// A streaming rational-ratio resampler built on a Kaiser-windowed sinc.
///
/// Replaces the linear interpolation the Opus encoder used to reach 48 kHz.
/// Linear interpolation is a very poor lowpass: its response droops several dB
/// well before Nyquist and leaves images only ~20 dB down, which lands as
/// dullness plus aliasing in exactly the top octave that ear-training material
/// is about.
///
/// The ratio is treated as exactly rational — 44100 -> 48000 is 160/147, not a
/// float approximation — so the phase never drifts over a long file.
///
/// Implementation is the standard polyphase decomposition: conceptually
/// upsample by L, lowpass, decimate by M, but only the coefficients that meet
/// a non-zero sample are ever evaluated. Each phase's taps are normalized to
/// sum to 1, so DC gain is exactly unity regardless of the window.
class SincResampler {
 public:
  SincResampler() = default;

  /// Configure for [inRate] -> [outRate] on [channels] interleaved channels.
  /// Returns false on a non-positive rate or an unsupported channel count.
  /// Safe to call again to reconfigure; it resets all streaming state.
  bool init(int inRate, int outRate, int channels);

  /// Whether [init] succeeded.
  bool isValid() const { return m_valid; }

  /// True when input and output rates match, in which case [process] is a
  /// straight copy.
  bool isPassthrough() const { return m_passthrough; }

  /// Resample [samples] interleaved floats from [input], appending the result
  /// to [output] (which is cleared first).
  ///
  /// Streaming: state carries across calls, so chunk boundaries are seamless
  /// and no samples are lost between them.
  void process(const float *input, size_t samples, std::vector<float> &output);

  /// Emit the samples still inside the filter's delay line, by feeding it
  /// silence. Call once after the last [process] so the tail is not lost.
  void flush(std::vector<float> &output);

  /// Input samples of delay the filter introduces, i.e. its half-width.
  int latencyFrames() const { return m_halfWidth; }

 private:
  /// Input-sample radius of the kernel. 24 each side (48 taps per output) with
  /// the Kaiser beta below puts the stopband far enough down that the images
  /// are inaudible, and offline encoding pays for it once.
  static constexpr int m_halfWidth = 24;

  /// Fraction of the theoretical Nyquist limit the passband actually reaches.
  ///
  /// 44.1 -> 48 kHz leaves no transition band at all if the cutoff sits
  /// exactly at Nyquist — the passband edge and the first image edge are both
  /// 22.05 kHz — so some headroom has to be bought somewhere. Pulling the
  /// cutoff in by 2% costs 0.002 dB at 19 kHz and 0.28 dB at 20 kHz, both
  /// inaudible, and takes the images from 17 dB down to 25 dB down.
  static constexpr double kCutoffFraction = 0.98;

  /// Kaiser beta. ~9.4 buys roughly 100 dB of stopband attenuation.
  static constexpr double kKaiserBeta = 9.4;

  bool m_valid = false;
  bool m_passthrough = false;

  int m_channels = 2;
  int m_l = 1;  ///< Interpolation factor, outRate / gcd.
  int m_m = 1;  ///< Decimation factor, inRate / gcd.

  /// Prototype filter, indexed [phase + k * L] for k in [0, 2 * halfWidth).
  std::vector<float> m_taps;

  /// Interleaved input history. m_histStart is the absolute frame index of
  /// its first frame; frames before the stream began are zero.
  std::vector<float> m_hist;
  int64_t m_histStart = 0;

  /// Absolute index of the next frame to be appended to [m_hist].
  int64_t m_framesIn = 0;

  /// Index of the next output frame to produce.
  int64_t m_outIndex = 0;

  void buildTaps();
  void appendFrames(const float *input, size_t samples);
  void emitAvailable(std::vector<float> &output);
};

}  // namespace soloud_offline
